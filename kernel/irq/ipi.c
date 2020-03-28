/*
 * linux/kernel/irq/ipi.c
 *
 * Copyright (C) 2015 Imagination Technologies Ltd
 * Author: Qais Yousef <qais.yousef@imgtec.com>
 *
 * This file contains driver APIs to the IPI subsystem.
 */

#define pr_fmt(fmt) "genirq/ipi: " fmt

#include <linux/irqdomain.h>
#include <linux/irq.h>

/**
 * irq_reserve_ipi() - Setup an IPI to destination cpumask
 * @domain:	IPI domain
 * @dest:	cpumask of cpus which can receive the IPI
 *
 * Allocate a virq that can be used to send IPI to any CPU in dest mask.
 *
 * On success it'll return linux irq number and error code on failure
 */
int irq_reserve_ipi(struct irq_domain *domain,
			     const struct cpumask *dest)
{
	unsigned int nr_irqs, offset;
	struct irq_data *data;
	int virq, i;

	if (!domain ||!irq_domain_is_ipi(domain)) {
		pr_warn("Reservation on a non IPI domain\n");
		return -EINVAL;
	}

	if (!cpumask_subset(dest, cpu_possible_mask)) {
		pr_warn("Reservation is not in possible_cpu_mask\n");
		return -EINVAL;
	}

	nr_irqs = cpumask_weight(dest);
	if (!nr_irqs) {
		pr_warn("Reservation for empty destination mask\n");
		return -EINVAL;
	}

	if (irq_domain_is_ipi_single(domain)) {
		/*
		 * If the underlying implementation uses a single HW irq on
		 * all cpus then we only need a single Linux irq number for
		 * it. We have no restrictions vs. the destination mask. The
		 * underlying implementation can deal with holes nicely.
		 */
		nr_irqs = 1;
		offset = 0;
	} else {
		unsigned int next;

		/*
		 * The IPI requires a seperate HW irq on each CPU. We require
		 * that the destination mask is consecutive. If an
		 * implementation needs to support holes, it can reserve
		 * several IPI ranges.
		 */
		offset = cpumask_first(dest);
		/*
		 * Find a hole and if found look for another set bit after the
		 * hole. For now we don't support this scenario.
		 */
		next = cpumask_next_zero(offset, dest);
		if (next < nr_cpu_ids)
			next = cpumask_next(next, dest);
		if (next < nr_cpu_ids) {
			pr_warn("Destination mask has holes\n");
			return -EINVAL;
		}
	}

	virq = irq_domain_alloc_descs(-1, nr_irqs, 0, NUMA_NO_NODE);
	if (virq <= 0) {
		pr_warn("Can't reserve IPI, failed to alloc descs\n");
		return -ENOMEM;
	}

	virq = __irq_domain_alloc_irqs(domain, virq, nr_irqs, NUMA_NO_NODE,
				       (void *) dest, true, NULL);

	if (virq <= 0) {
		pr_warn("Can't reserve IPI, failed to alloc hw irqs\n");
		goto free_descs;
	}

	for (i = 0; i < nr_irqs; i++) {
		data = irq_get_irq_data(virq + i);
		cpumask_copy(data->common->affinity, dest);
		data->common->ipi_offset = offset;
		irq_set_status_flags(virq + i, IRQ_NO_BALANCING);
	}
	return virq;

free_descs:
	irq_free_descs(virq, nr_irqs);
	return -EBUSY;
}

/**
 * irq_destroy_ipi() - unreserve an IPI that was previously allocated
 * @irq:	linux irq number to be destroyed
 * @dest:	cpumask of cpus which should have the IPI removed
 *
 * The IPIs allocated with irq_reserve_ipi() are retuerned to the system
 * destroying all virqs associated with them.
 *
 * Return 0 on success or error code on failure.
 */
int irq_destroy_ipi(unsigned int irq, const struct cpumask *dest)
{
	struct irq_data *data = irq_get_irq_data(irq);
	struct cpumask *ipimask = data ? irq_data_get_affinity_mask(data) : NULL;
	struct irq_domain *domain;
	unsigned int nr_irqs;

	if (!irq || !data || !ipimask)
		return -EINVAL;

	domain = data->domain;
	if (WARN_ON(domain == NULL))
		return -EINVAL;

	if (!irq_domain_is_ipi(domain)) {
		pr_warn("Trying to destroy a non IPI domain!\n");
		return -EINVAL;
	}

	if (WARN_ON(!cpumask_subset(dest, ipimask)))
		/*
		 * Must be destroying a subset of CPUs to which this IPI
		 * was set up to target
		 */
		return -EINVAL;

	if (irq_domain_is_ipi_per_cpu(domain)) {
		irq = irq + cpumask_first(dest) - data->common->ipi_offset;
		nr_irqs = cpumask_weight(dest);
	} else {
		nr_irqs = 1;
	}

	irq_domain_free_irqs(irq, nr_irqs);
	return 0;
}
