/*
 * Copyright (C) 2016 Linaro.
 * Viresh Kumar <viresh.kumar@linaro.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/err.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "cpufreq-dt.h"

static const struct of_device_id machines[] __initconst = {
	{ .compatible = "rockchip,rk2928", },
	{ .compatible = "rockchip,rk3036", },
	{ .compatible = "rockchip,rk3066a", },
	{ .compatible = "rockchip,rk3066b", },
	{ .compatible = "rockchip,rk3188", },
	{ .compatible = "rockchip,rk3228", },
	{ .compatible = "rockchip,rk3288", },
	{ .compatible = "rockchip,rk3366", },
	{ .compatible = "rockchip,rk3368", },
	{ .compatible = "rockchip,rk3399", },

	{ }
};

static int __init cpufreq_dt_platdev_init(void)
{
	struct device_node *np = of_find_node_by_path("/");
	const struct of_device_id *match;

	if (!np)
		return -ENODEV;

	match = of_match_node(machines, np);
	of_node_put(np);
	if (!match)
		return -ENODEV;

	return PTR_ERR_OR_ZERO(platform_device_register_data(NULL, "cpufreq-dt",
			       -1, match->data,
			       sizeof(struct cpufreq_dt_platform_data)));
}
device_initcall(cpufreq_dt_platdev_init);
