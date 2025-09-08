/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 * Copyright (c) 2015-2018, The Linux Foundation. All rights reserved.
 */

#ifndef _DPU_HW_MDSS_H
#define _DPU_HW_MDSS_H

#define DPU_DBG_NAME			"dpu"

#define DPU_NONE                        0

#ifndef DPU_CSC_MATRIX_COEFF_SIZE
#define DPU_CSC_MATRIX_COEFF_SIZE	9
#endif

#ifndef DPU_CSC_CLAMP_SIZE
#define DPU_CSC_CLAMP_SIZE		6
#endif

#ifndef DPU_CSC_BIAS_SIZE
#define DPU_CSC_BIAS_SIZE		3
#endif

#ifndef DPU_MAX_PLANES
#define DPU_MAX_PLANES			4
#endif

#define STAGES_PER_PLANE		2
#define PIPES_PER_STAGE			2
#define PIPES_PER_PLANE			(PIPES_PER_STAGE * STAGES_PER_PLANE)
#ifndef DPU_MAX_DE_CURVES
#define DPU_MAX_DE_CURVES		3
#endif

#define DPU_BLEND_FG_ALPHA_FG_CONST	(0 << 0)
#define DPU_BLEND_FG_ALPHA_BG_CONST	(1 << 0)
#define DPU_BLEND_FG_ALPHA_FG_PIXEL	(2 << 0)
#define DPU_BLEND_FG_ALPHA_BG_PIXEL	(3 << 0)
#define DPU_BLEND_FG_INV_ALPHA		(1 << 2)
#define DPU_BLEND_FG_MOD_ALPHA		(1 << 3)
#define DPU_BLEND_FG_INV_MOD_ALPHA	(1 << 4)
#define DPU_BLEND_FG_TRANSP_EN		(1 << 5)
#define DPU_BLEND_BG_ALPHA_FG_CONST	(0 << 8)
#define DPU_BLEND_BG_ALPHA_BG_CONST	(1 << 8)
#define DPU_BLEND_BG_ALPHA_FG_PIXEL	(2 << 8)
#define DPU_BLEND_BG_ALPHA_BG_PIXEL	(3 << 8)
#define DPU_BLEND_BG_INV_ALPHA		(1 << 10)
#define DPU_BLEND_BG_MOD_ALPHA		(1 << 11)
#define DPU_BLEND_BG_INV_MOD_ALPHA	(1 << 12)
#define DPU_BLEND_BG_TRANSP_EN		(1 << 13)

enum msm_dp_controller {
	MSM_DP_CONTROLLER_0,
	MSM_DP_CONTROLLER_1,
	MSM_DP_CONTROLLER_2,
	MSM_DP_CONTROLLER_3,
	MSM_DP_CONTROLLER_COUNT,
};

enum msm_dsi_controller {
	MSM_DSI_CONTROLLER_0,
	MSM_DSI_CONTROLLER_1,
	MSM_DSI_CONTROLLER_COUNT,
};

enum dpu_vsync_source {
	DPU_VSYNC_SOURCE_GPIO_0,
	DPU_VSYNC_SOURCE_GPIO_1,
	DPU_VSYNC_SOURCE_GPIO_2,
	DPU_VSYNC_SOURCE_INTF_0 = 3,
	DPU_VSYNC_SOURCE_INTF_1,
	DPU_VSYNC_SOURCE_INTF_2,
	DPU_VSYNC_SOURCE_INTF_3,
	DPU_VSYNC_SOURCE_WD_TIMER_4 = 11,
	DPU_VSYNC_SOURCE_WD_TIMER_3,
	DPU_VSYNC_SOURCE_WD_TIMER_2,
	DPU_VSYNC_SOURCE_WD_TIMER_1,
	DPU_VSYNC_SOURCE_WD_TIMER_0,
};

enum dpu_hw_blk_type {
	DPU_HW_BLK_TOP = 0,
	DPU_HW_BLK_SSPP,
	DPU_HW_BLK_LM,
	DPU_HW_BLK_CTL,
	DPU_HW_BLK_PINGPONG,
	DPU_HW_BLK_DCWB_PINGPONG,
	DPU_HW_BLK_INTF,
	DPU_HW_BLK_WB,
	DPU_HW_BLK_DSPP,
	DPU_HW_BLK_MERGE_3D,
	DPU_HW_BLK_DSC,
	DPU_HW_BLK_CDM,
	DPU_HW_BLK_CWB,
	DPU_HW_BLK_MAX,
};

enum dpu_sspp {
	SSPP_NONE,
	SSPP_VIG0,
	SSPP_VIG1,
	SSPP_VIG2,
	SSPP_VIG3,
	SSPP_RGB0,
	SSPP_RGB1,
	SSPP_RGB2,
	SSPP_RGB3,
	SSPP_DMA0,
	SSPP_DMA1,
	SSPP_DMA2,
	SSPP_DMA3,
	SSPP_DMA4,
	SSPP_DMA5,
	SSPP_CURSOR0,
	SSPP_CURSOR1,
	SSPP_MAX
};

enum dpu_sspp_type {
	SSPP_TYPE_VIG,
	SSPP_TYPE_RGB,
	SSPP_TYPE_DMA,
	SSPP_TYPE_CURSOR,
	SSPP_TYPE_MAX
};

enum dpu_lm {
	LM_0 = 1,
	LM_1,
	LM_2,
	LM_3,
	LM_4,
	LM_5,
	LM_6,
	LM_7,
	LM_MAX
};

enum dpu_stage {
	DPU_STAGE_BASE = 0,
	DPU_STAGE_0,
	DPU_STAGE_1,
	DPU_STAGE_2,
	DPU_STAGE_3,
	DPU_STAGE_4,
	DPU_STAGE_5,
	DPU_STAGE_6,
	DPU_STAGE_7,
	DPU_STAGE_8,
	DPU_STAGE_9,
	DPU_STAGE_10,
	DPU_STAGE_MAX
};
enum dpu_dspp {
	DSPP_0 = 1,
	DSPP_1,
	DSPP_2,
	DSPP_3,
	DSPP_MAX
};

enum dpu_ctl {
	CTL_0 = 1,
	CTL_1,
	CTL_2,
	CTL_3,
	CTL_4,
	CTL_5,
	CTL_MAX
};

enum dpu_dsc {
	DSC_NONE = 0,
	DSC_0,
	DSC_1,
	DSC_2,
	DSC_3,
	DSC_4,
	DSC_5,
	DSC_6,
	DSC_7,
	DSC_MAX
};

enum dpu_cdm {
	CDM_0 = 1,
	CDM_MAX
};

enum dpu_pingpong {
	PINGPONG_NONE,
	PINGPONG_0,
	PINGPONG_1,
	PINGPONG_2,
	PINGPONG_3,
	PINGPONG_4,
	PINGPONG_5,
	PINGPONG_6,
	PINGPONG_7,
	PINGPONG_CWB_0,
	PINGPONG_CWB_1,
	PINGPONG_CWB_2,
	PINGPONG_CWB_3,
	PINGPONG_S0,
	PINGPONG_MAX
};

enum dpu_merge_3d {
	MERGE_3D_0 = 1,
	MERGE_3D_1,
	MERGE_3D_2,
	MERGE_3D_3,
	MERGE_3D_4,
	MERGE_3D_5,
	MERGE_3D_MAX
};

enum dpu_intf {
	INTF_0 = 1,
	INTF_1,
	INTF_2,
	INTF_3,
	INTF_4,
	INTF_5,
	INTF_6,
	INTF_7,
	INTF_8,
	INTF_MAX
};

/*
 * Historically these values correspond to the values written to the
 * DISP_INTF_SEL register, which had to programmed manually. On newer MDP
 * generations this register is NOP, but we keep the values for historical
 * reasons.
 */
enum dpu_intf_type {
	INTF_NONE = 0x0,
	INTF_DSI = 0x1,
	INTF_HDMI = 0x3,
	INTF_LCDC = 0x5,
	/* old eDP found on 8x74 and 8x84 */
	INTF_EDP = 0x9,
	/* both DP and eDP,  handled by the new DP driver */
	INTF_DP = 0xa,

	/* virtual interfaces */
	INTF_WB = 0x100,
};

enum dpu_intf_mode {
	INTF_MODE_NONE = 0,
	INTF_MODE_CMD,
	INTF_MODE_VIDEO,
	INTF_MODE_WB_BLOCK,
	INTF_MODE_WB_LINE,
	INTF_MODE_MAX
};

enum dpu_wb {
	WB_0 = 1,
	WB_1,
	WB_2,
	WB_3,
	WB_MAX
};

enum dpu_cwb {
	CWB_0 = 0x1,
	CWB_1,
	CWB_2,
	CWB_3,
	CWB_MAX
};

enum dpu_wd_timer {
	WD_TIMER_0 = 0x1,
	WD_TIMER_1,
	WD_TIMER_2,
	WD_TIMER_3,
	WD_TIMER_4,
	WD_TIMER_5,
	WD_TIMER_MAX
};

enum dpu_vbif {
	VBIF_RT,
	VBIF_NRT,
	VBIF_MAX,
};

/**
 * enum dpu_3d_blend_mode
 * Desribes how the 3d data is blended
 * @BLEND_3D_NONE      : 3d blending not enabled
 * @BLEND_3D_FRAME_INT : Frame interleaving
 * @BLEND_3D_H_ROW_INT : Horizontal row interleaving
 * @BLEND_3D_V_ROW_INT : vertical row interleaving
 * @BLEND_3D_COL_INT   : column interleaving
 * @BLEND_3D_MAX       :
 */
enum dpu_3d_blend_mode {
	BLEND_3D_NONE = 0,
	BLEND_3D_FRAME_INT,
	BLEND_3D_H_ROW_INT,
	BLEND_3D_V_ROW_INT,
	BLEND_3D_COL_INT,
	BLEND_3D_MAX
};

#endif  /* _DPU_HW_MDSS_H */
