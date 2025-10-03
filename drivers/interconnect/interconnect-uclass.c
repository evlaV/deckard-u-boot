// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2025 Linaro Limited
 * Based on the Linux Driver:
 * Copyright (c) 2017-2019, Linaro Ltd.
 * Author: Georgi Djakov <georgi.djakov@linaro.org>
 */

#define LOG_CATEGORY UCLASS_INTERCONNECT

#include <dm.h>
#include <log.h>
#include <malloc.h>
#include <linux/err.h>
#include <interconnect.h>
#include <interconnect-uclass.h>
#include <dm/lists.h>
#include <dm/uclass-internal.h>
#include <dm/device-internal.h>
#include <dm/device_compat.h>

static struct icc_node *of_icc_get_from_provider(const struct ofnode_phandle_args *args);
static struct icc_path *icc_path_find(struct icc_node *src, struct icc_node *dst);
static struct icc_node *node_find(const int id);

/* Public API */

struct icc_path *of_icc_get(struct udevice *dev, const char *name)
{
	int index;

	index = dev_read_stringlist_search(dev, "interconnect-names", name);
	if (index < 0) {
		debug("fdt_stringlist_search() failed: %d\n", index);
		return ERR_PTR(index);
	}

	return of_icc_get_by_index(dev, index);
}

struct icc_path *of_icc_get_by_index(struct udevice *dev, int index)
{
	struct ofnode_phandle_args src_args, dst_args;
	struct icc_node *src_node, *dst_node;
	struct icc_path *path;
	int ret;

	debug("%s(dev=%p, idx=%d)\n", __func__, dev, index);

	ret = dev_read_phandle_with_args(dev, "interconnects",
					 "#interconnect-cells", 0, index * 2,
					 &src_args);
	if (ret) {
		debug("%s: dev_read_phandle_with_args src failed: %d\n",
		      __func__, ret);
		return ERR_PTR(ret);
	}

	ret = dev_read_phandle_with_args(dev, "interconnects",
					 "#interconnect-cells", 0, index * 2 + 1,
					 &dst_args);
	if (ret) {
		debug("%s: dev_read_phandle_with_args dst failed: %d\n",
		      __func__, ret);
		return ERR_PTR(ret);
	}

	src_node = of_icc_get_from_provider(&src_args);
	if (IS_ERR(src_node)) {
		dev_err(dev, "error finding src node\n");
		return ERR_CAST(src_node);
	}

	dst_node = of_icc_get_from_provider(&dst_args);
	if (IS_ERR(dst_node)) {
		dev_err(dev, "error finding dst node\n");
		return ERR_CAST(dst_node);
	}

	path = icc_path_find(src_node, dst_node);
	if (IS_ERR(path))
		dev_err(dev, "%s: invalid path=%ld\n", __func__, PTR_ERR(path));

	debug("%s(path=%p)\n", __func__, path);

	return path;
}

int icc_put(struct icc_path *path)
{
	struct icc_node *node;
	size_t i;
	int ret;

	debug("%s(path=%p)\n", __func__, path);

	if (!path || IS_ERR(path))
		return 0;

	ret = icc_set_bw(path, 0, 0);
	if (ret) {
		pr_err("%s: error (%d)\n", __func__, ret);
		return ret;
	}

	for (i = 0; i < path->num_nodes; i++) {
		node = path->reqs[i].node;

		if (node->users)
			node->users--;
		if (!node->users)
			device_remove(node->dev, DM_REMOVE_NORMAL);

		hlist_del(&path->reqs[i].req_node);
	}

	kfree(path);

	return 0;
}

static int __icc_enable(struct icc_path *path, bool enable)
{
	int i;

	if (!path)
		return 0;

	if (IS_ERR(path) || !path->num_nodes)
		return -EINVAL;

	for (i = 0; i < path->num_nodes; i++)
		path->reqs[i].enabled = enable;

	return icc_set_bw(path, path->reqs[0].avg_bw,
			  path->reqs[0].peak_bw);
}

int icc_enable(struct icc_path *path)
{
	debug("%s(path=%p)\n", __func__, path);
	return __icc_enable(path, true);
}

int icc_disable(struct icc_path *path)
{
	debug("%s(path=%p)\n", __func__, path);
	return __icc_enable(path, false);
}

static int apply_constraints(struct icc_path *path)
{
	struct icc_node *next, *prev = NULL;
	const struct interconnect_ops *ops;
	struct icc_provider *p;
	int ret = -EINVAL;
	int i;

	debug("%s(path=%p)\n", __func__, path);

	for (i = 0; i < path->num_nodes; i++) {
		next = path->reqs[i].node;
		p = next->provider;

		/* both endpoints should be valid master-slave pairs */
		if (!prev || (p != prev->provider && !p->inter_set)) {
			prev = next;
			continue;
		}

		ops = device_get_ops(p->dev);

		/* set the constraints */
		if (ops->set) {
			ret = ops->set(prev, next);
			if (ret)
				goto out;
		}

		prev = next;
	}
out:
	return ret;
}

static int aggregate_requests(struct icc_node *node)
{
	const struct interconnect_ops *ops = device_get_ops(node->provider->dev);
	struct icc_req *r;
	u32 avg_bw, peak_bw;

	debug("%s(dev=%s)\n", __func__, node->dev->name);

	node->avg_bw = 0;
	node->peak_bw = 0;

	if (ops->pre_aggregate)
		ops->pre_aggregate(node);

	hlist_for_each_entry(r, &node->req_list, req_node) {
		if (r->enabled) {
			avg_bw = r->avg_bw;
			peak_bw = r->peak_bw;
		} else {
			avg_bw = 0;
			peak_bw = 0;
		}
		if (ops->aggregate)
			ops->aggregate(node, r->tag, avg_bw, peak_bw,
				       &node->avg_bw, &node->peak_bw);
	}

	return 0;
}

int icc_set_bw(struct icc_path *path, u32 avg_bw, u32 peak_bw)
{
	struct icc_node *node;
	u32 old_avg, old_peak;
	size_t i;
	int ret;

	debug("%s(path=%p)\n", __func__, path);

	if (!path)
		return 0;

	if (IS_ERR(path) || !path->num_nodes)
		return -EINVAL;

	old_avg = path->reqs[0].avg_bw;
	old_peak = path->reqs[0].peak_bw;

	for (i = 0; i < path->num_nodes; i++) {
		node = path->reqs[i].node;

		/* update the consumer request for this path */
		path->reqs[i].avg_bw = avg_bw;
		path->reqs[i].peak_bw = peak_bw;

		/* aggregate requests for this node */
		aggregate_requests(node);
	}

	ret = apply_constraints(path);
	if (ret) {
		debug("interconnect: error applying constraints (%d)\n", ret);

		for (i = 0; i < path->num_nodes; i++) {
			node = path->reqs[i].node;
			path->reqs[i].avg_bw = old_avg;
			path->reqs[i].peak_bw = old_peak;
			aggregate_requests(node);
		}
		apply_constraints(path);
	}

	return ret;
}

/* Provider API */

static struct icc_path *icc_path_init(struct icc_node *dst, ssize_t num_nodes)
{
	struct icc_node *node = dst;
	struct icc_path *path;
	struct udevice *dev;
	int i, ret;

	debug("%s(dev=%s)\n", __func__, node->dev->name);

	path = calloc(sizeof(struct icc_req), num_nodes);
	if (!path)
		return ERR_PTR(-ENOMEM);

	path->num_nodes = num_nodes;

	for (i = num_nodes - 1; i >= 0; i--) {
		hlist_add_head(&path->reqs[i].req_node, &node->req_list);
		debug("%s: req %s\n", __func__, node->dev->name);
		path->reqs[i].node = node;
		path->reqs[i].enabled = true;

		/* Probe this node since used in an active path */
		ret = uclass_get_device_tail(node->dev, 0, &dev);
		if (ret)
			return ERR_PTR(ret);

		node->users++;

		/* reference to previous node was saved during path traversal */
		node = node->reverse;
	}

	return path;
}

static struct icc_path *icc_path_find(struct icc_node *src, struct icc_node *dst)
{
	struct icc_path *path = ERR_PTR(-EPROBE_DEFER);
	struct icc_node *n, *node = NULL;
	struct list_head traverse_list;
	struct list_head edge_list;
	struct list_head visited_list;
	size_t i, depth = 1;
	bool found = false;

	debug("%s(src=%s,dest=%p\n", __func__, src->dev->name, dst->dev->name);

	INIT_LIST_HEAD(&traverse_list);
	INIT_LIST_HEAD(&edge_list);
	INIT_LIST_HEAD(&visited_list);

	list_add(&src->search_list, &traverse_list);
	src->reverse = NULL;

	do {
		list_for_each_entry_safe(node, n, &traverse_list, search_list) {
			if (node == dst) {
				found = true;
				list_splice_init(&edge_list, &visited_list);
				list_splice_init(&traverse_list, &visited_list);
				break;
			}
			for (i = 0; i < node->num_links; i++) {
				struct icc_node *tmp;

				tmp = node_find(node->links[i]);
				if (!tmp) {
					pr_err("%s: missing link to node id %d\n",
					       __func__, node->links[i]);
					path = ERR_PTR(-ENOENT);
					goto out;
				}

				if (tmp->is_traversed)
					continue;

				tmp->is_traversed = true;
				tmp->reverse = node;
				list_add_tail(&tmp->search_list, &edge_list);
			}
		}

		if (found)
			break;

		list_splice_init(&traverse_list, &visited_list);
		list_splice_init(&edge_list, &traverse_list);

		/* count the hops including the source */
		depth++;

	} while (!list_empty(&traverse_list));

out:
	/* reset the traversed state */
	list_for_each_entry_reverse(n, &visited_list, search_list)
		n->is_traversed = false;

	if (found)
		path = icc_path_init(dst, depth);

	return path;
}

static struct icc_node *of_icc_get_from_provider(const struct ofnode_phandle_args *args)
{
	const struct interconnect_ops *ops;
	struct udevice *dev;
	int ret;

	ret = uclass_get_device_by_ofnode(UCLASS_INTERCONNECT, args->node,
					  &dev);
	if (ret) {
		debug("%s: uclass_get_device_by_ofnode failed: %d\n",
		      __func__, ret);
		return ERR_PTR(ret);
	}
	ops = device_get_ops(dev);

	return ops->of_xlate(dev, args);
}

static struct icc_node *node_find(const int id)
{
	struct udevice *dev;

	for (uclass_find_first_device(UCLASS_ICC_NODE, &dev);
	     dev;
	     uclass_find_next_device(&dev)) {
		struct icc_node *node = dev_get_uclass_plat(dev);

		if (node->id == id)
			return node;
	}

	return NULL;
}

struct icc_node *icc_node_create(const struct icc_provider *provider,
				 int id, const char *node_name)
{
	struct icc_node *node;
	struct udevice *dev;
	char name[64];
	int ret;

	snprintf(name, 64, "%s@%d", node_name, id);

	ret = device_bind_driver(provider->dev, "icc_node", strdup(name), &dev);
	if (ret)
		return ERR_PTR(ret);

	device_set_name_alloced(dev);

	node = dev_get_uclass_plat(dev);
	node->id = id;

	return node;
}

int icc_link_create(struct icc_node *node, const int dst_id)
{
	int *new;

	if (!node->provider)
		return -EINVAL;

	new = realloc(node->links,
		      (node->num_links + 1) * sizeof(*node->links));
	if (!new)
		return -ENOMEM;

	node->links = new;
	node->links[node->num_links++] = dst_id;

	return 0;
}

void icc_node_add(struct icc_node *node, struct icc_provider *provider)
{
	node->provider = provider;
}

static int interconnect_pre_remove(struct udevice *dev)
{
	struct udevice *pos;
	int ret;

	debug("%s(dev=%s)\n", __func__, dev->name);

	device_foreach_child(pos, dev) {
		debug("%s(pos=%s)\n", __func__, pos->name);
		ret = device_remove(pos, DM_REMOVE_NORMAL);
		if (ret)
			return ret;
	}

	return 0;
}

static int interconnect_pre_unbind(struct udevice *dev)
{
	struct udevice *pos, *n;
	int ret;

	debug("%s(dev=%s)\n", __func__, dev->name);

	device_foreach_child_safe(pos, n, dev) {
		debug("%s(pos=%s)\n", __func__, pos->name);
		ret = device_unbind(pos);
		if (ret)
			return ret;
	}

	return 0;
}

static int icc_node_bind(struct udevice *dev)
{
	struct icc_node *node = dev_get_uclass_plat(dev);

	debug("%s(dev=%s)\n", __func__, dev->name);

	node->dev = dev;

	return 0;
}

static int icc_node_probe(struct udevice *dev)
{
	struct icc_node *node = dev_get_uclass_plat(dev);

	debug("%s(dev=%s,parent=%p)\n", __func__, dev->name, dev->parent->name);

	node->avg_bw = 0;
	node->peak_bw = 0;

	return 0;
}

static int icc_node_remove(struct udevice *dev)
{
	struct icc_node *node = dev_get_uclass_plat(dev);

	debug("%s(dev=%s,users=%d,parent=%p)\n", __func__,
	      dev->name, node->users, dev->parent->name);

	if (node->users)
		return -EBUSY;

	return 0;
}

static int icc_node_unbind(struct udevice *dev)
{
	struct icc_node *node = dev_get_uclass_plat(dev);

	debug("%s(dev=%s)\n", __func__, dev->name);

	kfree(node->links);

	return 0;
}

UCLASS_DRIVER(interconnect) = {
	.id		= UCLASS_INTERCONNECT,
	.name		= "interconnect",
	.pre_remove	= interconnect_pre_remove,
	.pre_unbind	= interconnect_pre_unbind,
	.per_device_plat_auto = sizeof(struct icc_provider),
};

U_BOOT_DRIVER(icc_node) = {
	.name		= "icc_node",
	.id		= UCLASS_ICC_NODE,
	.bind		= icc_node_bind,
	.probe		= icc_node_probe,
	.remove		= icc_node_remove,
	.unbind		= icc_node_unbind,
};

UCLASS_DRIVER(icc_node) = {
	.id		= UCLASS_ICC_NODE,
	.name		= "icc_node",
	.per_device_plat_auto = sizeof(struct icc_node),
};
