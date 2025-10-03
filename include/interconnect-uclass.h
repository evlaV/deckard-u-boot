/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2025 Linaro Limited
 */

#ifndef _INTERCONNECT_UCLASS_H
#define _INTERCONNECT_UCLASS_H

#include <interconnect.h>

#define icc_units_to_bps(bw)  ((bw) * 1000ULL)

struct udevice;
struct icc_node;

struct icc_req {
	struct hlist_node req_node;
	struct icc_node *node;
	struct device *dev;
	bool enabled;
	u32 tag;
	u32 avg_bw;
	u32 peak_bw;
};

struct icc_path {
	size_t num_nodes;
	struct icc_req reqs[];
};

struct icc_provider {
	struct udevice		*dev;
	bool			inter_set;
	unsigned int		xlate_num_nodes;
	struct icc_node		**xlate_nodes;
};

struct icc_node {
	struct udevice		*dev;
	int			id;
	int			*links;
	size_t			num_links;
	u8			is_unlinked:1;
	int			users;

	struct icc_provider	*provider;
	struct list_head	node_list;
	struct list_head	search_list;
	struct icc_node		*reverse;
	u8			is_traversed:1;
	struct hlist_head	req_list;
	u32			avg_bw;
	u32			peak_bw;
	u32			init_avg;
	u32			init_peak;
	void			*data;
};

struct interconnect_ops {
	struct icc_node *(*of_xlate)(struct udevice *dev,
				     const struct ofnode_phandle_args *args);
	int (*set)(struct icc_node *src, struct icc_node *dst);
	void (*pre_aggregate)(struct icc_node *node);
	int (*aggregate)(struct icc_node *node, u32 tag, u32 avg_bw,
			 u32 peak_bw, u32 *agg_avg, u32 *agg_peak);
};

struct icc_node *icc_node_create(const struct icc_provider *provider,
				 int id, const char *node_name);
int icc_link_create(struct icc_node *node, const int dst_id);
void icc_node_add(struct icc_node *node, struct icc_provider *provider);
void icc_node_del(struct icc_node *node);

#endif
