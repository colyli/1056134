/*
 * platform_device probing code for ARM performance counters.
 *
 * Copyright (C) 2009 picoChip Designs, Ltd., Jamie Iles
 * Copyright (C) 2010 ARM Ltd., Will Deacon <will.deacon@arm.com>
 */
#define pr_fmt(fmt) "hw perfevents: " fmt

#include <linux/bug.h>
#include <linux/cpumask.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/kconfig.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/percpu.h>
#include <linux/perf/arm_pmu.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/smp.h>

static int probe_current_pmu(struct arm_pmu *pmu,
			     const struct pmu_probe_info *info)
{
	int cpu = get_cpu();
	unsigned int cpuid = read_cpuid_id();
	int ret = -ENODEV;

	pr_info("probing PMU on CPU %d\n", cpu);

	for (; info->init != NULL; info++) {
		if ((cpuid & info->mask) != info->cpuid)
			continue;
		ret = info->init(pmu);
		break;
	}

	put_cpu();
	return ret;
}

static int pmu_parse_percpu_irq(struct arm_pmu *pmu, int irq)
{
	int cpu;
	struct pmu_hw_events __percpu *hw_events = pmu->hw_events;

	/*
	 * Replace irq_get_percpu_devid_partition since partitionned percpu
	 * interrupts are not supported
	 */
	cpumask_setall(&pmu->supported_cpus);

	for_each_cpu(cpu, &pmu->supported_cpus)
		per_cpu(hw_events->irq, cpu) = irq;

	return 0;
}

static bool pmu_has_irq_affinity(struct device_node *node)
{
	return !!of_find_property(node, "interrupt-affinity", NULL);
}

static int pmu_parse_irq_affinity(struct device_node *node, int i)
{
	struct device_node *dn;
	int cpu;

	/*
	 * If we don't have an interrupt-affinity property, we guess irq
	 * affinity matches our logical CPU order, as we used to assume.
	 * This is fragile, so we'll warn in pmu_parse_irqs().
	 */
	if (!pmu_has_irq_affinity(node))
		return i;

	dn = of_parse_phandle(node, "interrupt-affinity", i);
	if (!dn) {
		pr_warn("failed to parse interrupt-affinity[%d] for %s\n",
			i, node->name);
		return -EINVAL;
	}

	/* Now look up the logical CPU number */
	for_each_possible_cpu(cpu) {
		struct device_node *cpu_dn;

		cpu_dn = of_cpu_device_node_get(cpu);
		of_node_put(cpu_dn);

		if (dn == cpu_dn)
			break;
	}

	if (cpu >= nr_cpu_ids) {
		pr_warn("failed to find logical CPU for %s\n", dn->name);
	}

	of_node_put(dn);

	return cpu;
}

static int pmu_parse_irqs(struct arm_pmu *pmu)
{
	int i = 0, num_irqs;
	struct platform_device *pdev = pmu->plat_device;
	struct pmu_hw_events __percpu *hw_events = pmu->hw_events;

	num_irqs = platform_irq_count(pdev);
	if (num_irqs < 0) {
		pr_err("unable to count PMU IRQs\n");
		return num_irqs;
	}

	/*
	 * In this case we have no idea which CPUs are covered by the PMU.
	 * To match our prior behaviour, we assume all CPUs in this case.
	 */
	if (num_irqs == 0) {
		pr_warn("no irqs for PMU, sampling events not supported\n");
		pmu->pmu.capabilities |= PERF_PMU_CAP_NO_INTERRUPT;
		cpumask_setall(&pmu->supported_cpus);
		return 0;
	}

	if (num_irqs == 1) {
		int irq = platform_get_irq(pdev, 0);
		if (irq && irq_is_percpu(irq))
			return pmu_parse_percpu_irq(pmu, irq);
	}

	if (!pmu_has_irq_affinity(pdev->dev.of_node)) {
		pr_warn("no interrupt-affinity property for %pOF, guessing.\n",
			pdev->dev.of_node);
	}

	/*
	 * Some platforms have all PMU IRQs OR'd into a single IRQ, with a
	 * special platdata function that attempts to demux them.
	 */
	if (dev_get_platdata(&pdev->dev))
		cpumask_setall(&pmu->supported_cpus);

	for (i = 0; i < num_irqs; i++) {
		int cpu, irq;

		irq = platform_get_irq(pdev, i);
		if (WARN_ON(irq <= 0))
			continue;

		if (irq_is_percpu(irq)) {
			pr_warn("multiple PPIs or mismatched SPI/PPI detected\n");
			return -EINVAL;
		}

		cpu = pmu_parse_irq_affinity(pdev->dev.of_node, i);
		if (cpu < 0)
			return cpu;
		if (cpu >= nr_cpu_ids)
			continue;

		if (per_cpu(hw_events->irq, cpu)) {
			pr_warn("multiple PMU IRQs for the same CPU detected\n");
			return -EINVAL;
		}

		per_cpu(hw_events->irq, cpu) = irq;
		cpumask_set_cpu(cpu, &pmu->supported_cpus);
	}

	return 0;
}

int arm_pmu_device_probe(struct platform_device *pdev,
			 const struct of_device_id *of_table,
			 const struct pmu_probe_info *probe_table)
{
	const struct of_device_id *of_id;
	armpmu_init_fn init_fn;
	struct device_node *node = pdev->dev.of_node;
	struct arm_pmu *pmu;
	int ret = -ENODEV;

	pmu = armpmu_alloc();
	if (!pmu)
		return -ENOMEM;

	pmu->plat_device = pdev;

	ret = pmu_parse_irqs(pmu);
	if (ret)
		goto out_free;

	if (node && (of_id = of_match_node(of_table, pdev->dev.of_node))) {
		init_fn = of_id->data;

		/* secure-reg-access handling is removed since following patch
		 * is missing:
		 * 8d1a0ae724ad ARM: perf: Set ARMv7 SDER SUNIDEN bit
		 */
		ret = init_fn(pmu);
	} else if (probe_table) {
		cpumask_setall(&pmu->supported_cpus);
		ret = probe_current_pmu(pmu, probe_table);
	}

	if (ret) {
		pr_info("%pOF: failed to probe PMU!\n", node);
		goto out_free;
	}

	ret = armpmu_request_irqs(pmu);
	if (ret)
		goto out_free_irqs;

	ret = armpmu_register(pmu);
	if (ret)
		goto out_free;

	return 0;

out_free_irqs:
	armpmu_free_irqs(pmu);
out_free:
	pr_info("%pOF: failed to register PMU devices!\n", node);
	armpmu_free(pmu);
	return ret;
}
