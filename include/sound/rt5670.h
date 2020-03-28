/*
 * linux/sound/rt5670.h -- Platform data for RT5670
 *
 * Copyright 2014 Realtek Microelectronics
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __LINUX_SND_RT5670_H
#define __LINUX_SND_RT5670_H

enum rt5670_gpios {
	RT5670_GPIO_NONE = 0,
	RT5670_GPIO2,
};


struct rt5670_platform_data {
	int jd_mode;
	bool in2_diff;
	bool dev_gpio;

	bool dmic_en;
	enum rt5670_gpios hs_ground_control_gpio;
	unsigned int dmic1_data_pin;
	/* 0 = GPIO6; 1 = IN2P; 3 = GPIO7*/
	unsigned int dmic2_data_pin;
	/* 0 = GPIO8; 1 = IN3N; */
	unsigned int dmic3_data_pin;
	/* 0 = GPIO9; 1 = GPIO10; 2 = GPIO5*/
};

#endif
