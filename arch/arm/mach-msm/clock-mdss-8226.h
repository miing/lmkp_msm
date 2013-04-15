/* Copyright (c) 2013, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __ARCH_ARM_MACH_MSM_CLOCK_MDSS_8226
#define __ARCH_ARM_MACH_MSM_CLOCK_MDSS_8226

extern struct clk_ops clk_ops_dsi_byte_pll;
extern struct clk_ops clk_ops_dsi_pixel_pll;

void mdss_clk_ctrl_pre_init(struct clk *ahb_clk);
void mdss_clk_ctrl_post_init(void);

#endif /* __ARCH_ARM_MACH_MSM_CLOCK_MDSS_8226 */
