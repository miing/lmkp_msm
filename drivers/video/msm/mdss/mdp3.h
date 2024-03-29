/* Copyright (c) 2013, The Linux Foundation. All rights reserved.
 * Copyright (C) 2007 Google Incorporated
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#ifndef MDP3_H
#define MDP3_H

#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/earlysuspend.h>

#include <mach/iommu_domains.h>

#include "mdp3_dma.h"

enum  {
	MDP3_CLK_AHB,
	MDP3_CLK_CORE,
	MDP3_CLK_VSYNC,
	MDP3_CLK_LCDC,
	MDP3_MAX_CLK
};

enum {
	MDP3_IOMMU_DOMAIN,
	MDP3_IOMMU_DOMAIN_MAX
};

enum {
	MDP3_IOMMU_CTX_PPP_0,
	MDP3_IOMMU_CTX_PPP_1,
	MDP3_IOMMU_CTX_DMA_0,
	MDP3_IOMMU_CTX_DMA_1,
	MDP3_IOMMU_CTX_MAX
};

enum {
	MDP3_BW_CLIENT_DMA_P,
	MDP3_BW_CLIENT_DMA_S,
	MDP3_BW_CLIENT_DMA_E,
	MDP3_BW_CLIENT_PPP,
};

struct mdp3_iommu_domain_map {
	u32 domain_type;
	char *client_name;
	struct msm_iova_partition partitions[1];
	int npartitions;
	int domain_idx;
	struct iommu_domain *domain;
};

struct mdp3_iommu_ctx_map {
	u32 ctx_type;
	struct mdp3_iommu_domain_map *domain;
	char *ctx_name;
	struct device *ctx;
	int attached;
};

#define MDP3_MAX_INTR 28

struct mdp3_intr_cb {
	void (*cb)(int type, void *);
	void *data;
};

struct mdp3_hw_resource {
	struct platform_device *pdev;
	u32 mdp_rev;

	struct mutex res_mutex;

	struct clk *clocks[MDP3_MAX_CLK];
	int clock_ref_count[MDP3_MAX_CLK];

	char __iomem *mdp_base;
	size_t mdp_reg_size;

	u32 irq;
	u32 bus_handle;

	struct ion_client *ion_client;
	struct mdp3_iommu_domain_map *domains;
	struct mdp3_iommu_ctx_map *iommu_contexts;

	struct mdp3_dma dma[MDP3_DMA_MAX];
	struct mdp3_intf intf[MDP3_DMA_OUTPUT_SEL_MAX];

	spinlock_t irq_lock;
	u32 irqMask;
	struct mdp3_intr_cb callbacks[MDP3_MAX_INTR];

	struct early_suspend suspend_handler;
};

extern struct mdp3_hw_resource *mdp3_res;

struct mdp3_dma *mdp3_get_dma_pipe(int capability);
struct mdp3_intf *mdp3_get_display_intf(int type);
void mdp3_irq_enable(int type);
void mdp3_irq_disable(int type);
void mdp3_irq_disable_nosync(int type);
int mdp3_set_intr_callback(u32 type, struct mdp3_intr_cb *cb);
int mdp3_clk_set_rate(int clk_type, unsigned long clk_rate);
int mdp3_clk_enable(int enable);
int mdp3_bus_scale_set_quota(int client, u64 ab_quota, u64 ib_quota);

#define MDP3_REG_WRITE(addr, val) writel_relaxed(val, mdp3_res->mdp_base + addr)
#define MDP3_REG_READ(addr) readl_relaxed(mdp3_res->mdp_base + addr)

#endif /* MDP3_H */
