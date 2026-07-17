/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 * Copyright (c) 2015-2021, The Linux Foundation. All rights reserved.
 *
 * This is a minimal port of the Linux dpu hw abstractions.
 */


#ifndef __DPU_HW_UTILS_H__
#define __DPU_HW_UTILS_H__

#include <linux/types.h>
#include <drm/drm_rect.h>

#include "dpu_hw_mdss.h"

/*
 * This is the common struct maintained by each sub block
 * for mapping the register offsets in this block to the
 * absoulute IO address
 * @blk_addr:     hw block register mapped address
 * @log_mask:     log mask for this block
 */
struct dpu_hw_blk_reg_map {
	void __iomem *blk_addr;
	// u32 log_mask;
};

/**
 * struct dpu_hw_blk - opaque hardware block object
 */
struct dpu_hw_blk {
	/* opaque */
};

struct dpu_hw_mixer_cfg {
	u32 out_width;
	u32 out_height;
	bool right_mixer;
	int flags;
};

struct dpu_hw_mixer {
	struct dpu_hw_blk base;
	struct dpu_hw_blk_reg_map hw;

	/* lm */
	enum dpu_lm  idx;
	const struct dpu_lm_cfg   *cap;
	const struct dpu_mdp_cfg  *mdp;
	const struct dpu_ctl_cfg  *ctl;

	/* ops */
	// struct dpu_hw_lm_ops ops;

	/* store mixer info specific to display */
	struct dpu_hw_mixer_cfg cfg;
};

struct dpu_hw_intf {
	struct dpu_hw_blk_reg_map hw;

	/* intf */
	enum dpu_intf idx;
	const struct dpu_intf_cfg *cap;

	const struct dpu_mdss_version *mdss_ver;

	/* ops */
	// struct dpu_hw_intf_ops ops;
};

struct dpu_hw_dsc {
	struct dpu_hw_blk base;
	struct dpu_hw_blk_reg_map hw;

	/* dsc */
	enum dpu_dsc idx;
	const struct dpu_dsc_cfg *cap;

	/* ops */
	// struct dpu_hw_dsc_ops ops;
};

struct dpu_hw_pingpong {
	struct dpu_hw_blk base;
	struct dpu_hw_blk_reg_map hw;

	/* pingpong */
	enum dpu_pingpong idx;
	const struct dpu_pingpong_cfg *cap;
	/*
	 * We don't use merge_3d, cases where we would are currently
	 * all DSC panels and the DSC blocks solve the same issue.
	 */
	// struct dpu_hw_merge_3d *merge_3d;

	/* ops */
	// struct dpu_hw_pingpong_ops ops;
};

/**
 * struct dpu_hw_sspp - pipe description
 * @base: hardware block base structure
 * @hw: block hardware details
 * @ubwc: UBWC configuration data
 * @idx: pipe index
 * @cap: pointer to layer_cfg
 * @mdss_ver: MDSS version info to use for feature checks
 * @ops: pointer to operations possible for this pipe
 */
struct dpu_hw_sspp {
	struct dpu_hw_blk base;
	struct dpu_hw_blk_reg_map hw;
	// const struct qcom_ubwc_cfg_data *ubwc;

	/* Pipe */
	enum dpu_sspp idx;
	const struct dpu_sspp_cfg *cap;

	const struct dpu_mdss_version *mdss_ver;

	/* Ops */
	// struct dpu_hw_sspp_ops ops;
};

/**
 * struct dpu_hw_fmt_layout - format information of the source pixel data
 * @num_planes: number of planes (including meta data planes)
 * @width: image width
 * @height: image height
 * @total_size: total size in bytes
 * @plane_addr: address of each plane
 * @plane_size: length of each plane
 * @plane_pitch: pitch of each plane
 */
struct dpu_hw_fmt_layout {
	uint32_t num_planes;
	uint32_t width;
	uint32_t height;
	uint32_t total_size;
	uint32_t plane_addr[DPU_MAX_PLANES];
	uint32_t plane_size[DPU_MAX_PLANES];
	uint32_t plane_pitch[DPU_MAX_PLANES];
};

/**
 * enum dpu_sspp_multirect_index - multirect mode
 * @DPU_SSPP_RECT_SOLO: multirect disabled
 * @DPU_SSPP_RECT_0: rect0 of a multirect pipe
 * @DPU_SSPP_RECT_1: rect1 of a multirect pipe
 *
 * Note: HW supports multirect with either RECT0 or
 * RECT1. Considering no benefit of such configs over
 * SOLO mode and to keep the plane management simple,
 * we dont support single rect multirect configs.
 */
enum dpu_sspp_multirect_index {
	DPU_SSPP_RECT_SOLO = 0,
	DPU_SSPP_RECT_0,
	DPU_SSPP_RECT_1,
};

#define multirect_idx_str(idx) ((const char *[]){ "Solo", "Primary", "Secondary" }[idx])

enum dpu_sspp_multirect_mode {
	DPU_SSPP_MULTIRECT_NONE = 0,
	DPU_SSPP_MULTIRECT_PARALLEL,
	DPU_SSPP_MULTIRECT_TIME_MX,
};

/**
 * struct dpu_hw_stage_cfg - blending stage cfg
 * @stage : SSPP_ID at each stage
 * @multirect_index: index of the rectangle of SSPP.
 */
struct dpu_hw_stage_cfg {
	// [zpos][mixer]
	enum dpu_sspp stage[DPU_STAGE_MAX][PIPES_PER_STAGE];
	enum dpu_sspp_multirect_index multirect_index
					[DPU_STAGE_MAX][PIPES_PER_STAGE];
};

/**
 * struct dpu_sw_pipe_cfg : software pipe configuration
 * @src_rect:  src ROI, caller takes into account the different operations
 *             such as decimation, flip etc to program this field
 * @dst_rect: destination ROI.
 * @rotation: simplified drm rotation hint
 */
struct dpu_sw_pipe_cfg {
	struct drm_rect src_rect;
	struct drm_rect dst_rect;
	unsigned int rotation;
};

/**
 * struct dpu_sw_pipe - software pipe description
 * @sspp:      backing SSPP pipe
 * @multirect_index:     index of the rectangle of SSPP
 * @multirect_mode:      parallel or time multiplex multirect mode
 */
struct dpu_sw_pipe {
	struct dpu_hw_sspp *sspp;
	enum dpu_sspp_multirect_index multirect_index;
	enum dpu_sspp_multirect_mode multirect_mode;
};

/* pending_flush_mask */
#define  MERGE_3D_IDX   23
#define  DSC_IDX        22
#define CDM_IDX         26
#define  PERIPH_IDX     30
#define  INTF_IDX       31
#define WB_IDX          16
#define CWB_IDX         28

/**
 * struct dpu_hw_ctl : CTL PATH driver object
 * @base: hardware block base structure
 * @hw: block register map object
 * @idx: control path index
 * @caps: control path capabilities
 * @mixer_count: number of mixers
 * @mixer_hw_caps: mixer hardware capabilities
 * @pending_flush_mask: storage for pending ctl_flush managed via ops
 * @pending_intf_flush_mask: pending INTF flush
 * @pending_dsc_flush_mask: pending DSC flush
 * @mdss_ver: MDSS revision information
 * @ops: operation list
 */
struct dpu_hw_ctl {
	struct dpu_hw_blk base;
	struct dpu_hw_blk_reg_map hw;

	/* ctl path */
	int idx;
	const struct dpu_ctl_cfg *cap;
	int mixer_count;
	const struct dpu_lm_cfg *mixer_hw_caps;
	u32 pending_flush_mask;
	u32 pending_intf_flush_mask;
	u32 pending_dsc_flush_mask;

	const struct dpu_mdss_version *mdss_ver;

	/* ops */
	// struct dpu_hw_ctl_ops ops;
};

/**
 * enum dpu_enc_split_role - Role this physical encoder will play in a
 *	split-panel configuration, with primary and secondary interfaces.
 *	The primary intf has extra responsibilities, like managing the VBLANK IRQ.
 * @ENC_ROLE_SOLO:	This is the one and only panel.
 * @ENC_ROLE_PRIMARY:	This encoder is the primary of a split panel config.
 * @ENC_ROLE_SECONDARY:	This encoder is not the primary of a split panel config.
 */
enum dpu_enc_split_role {
	ENC_ROLE_SOLO,
	ENC_ROLE_PRIMARY,
	ENC_ROLE_SECONDARY,
};

/*
 * DSC and PP blocks are tied together. In Quad Pipe only
 * the first PP block is connected to the intf, the second
 * one is just used for DSC.
 */
struct dpu_phys_encoder {
	struct dpu_hw_pingpong pingpong[2];
	int num_pingpong;
	struct dpu_hw_dsc dsc[2];
	int num_dsc;
	struct dpu_hw_mixer mixer[2];
	int num_mixer;
	struct dpu_hw_sspp *sspp;
	struct dpu_hw_ctl *ctl;
	struct dpu_hw_intf intf;
	enum dpu_enc_split_role split_role;
	struct drm_rect plane;
};

struct dpu_topology {
	int num_encs;
	struct dpu_phys_encoder encs[2];
	/* These can be shared between the encoders */
	int num_sspp;
	struct dpu_hw_sspp sspp[2];
	int num_ctl;
	struct dpu_hw_ctl ctl[2];
	/*
	 * Indicates quad pipe with DSC merge ->
	 * CTL per-INTF if false and
	 * split_role != SOLO then 1 CTL is
	 * used for both INTFs
	 */
	bool dsc_merge;
};

/* List of requirements used to determine the resources */
struct dpu_resource_request {
	bool dsc;
	/* Expected to be the full mode width even for dual DSI */
	u32 width;
	u32 height;
	// u8 bpp;
	void *dpu_base;
	enum dpu_intf_type intf_type;
	unsigned long core_clk_max_rate;
	const struct dpu_mdss_cfg *cat;
	int rotation;
};

#define enc_is_primary(enc) (enc->split_role < ENC_ROLE_SECONDARY)
#define enc_is_split(enc) (enc->split_role > ENC_ROLE_SOLO)
#define split_role_str(sr) ((const char *[]){ "Solo", "Primary", "Secondary" }[sr])

int dpu_hw_get_encoder_topology(const struct dpu_resource_request *req, bool dual_dsi, struct dpu_topology *topology);

#endif /* __DPU_HW_UTILS_H__ */
