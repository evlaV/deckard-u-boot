// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2015-2018, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2023, Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <video.h>
#include "dpu_hw_interrupts.h"
#include "catalog/dpu_hw_catalog.h"
#include "dpu_hw_mdss.h"

#define VIG_BASE_MASK \
	(BIT(DPU_SSPP_QOS) |\
	BIT(DPU_SSPP_CDP) |\
	BIT(DPU_SSPP_TS_PREFILL) | BIT(DPU_SSPP_EXCL_RECT))

#define VIG_MASK \
	(VIG_BASE_MASK | \
	BIT(DPU_SSPP_CSC_10BIT))

#define VIG_MSM8953_MASK \
	(BIT(DPU_SSPP_QOS) |\
	 BIT(DPU_SSPP_SCALER_QSEED2) |\
	 BIT(DPU_SSPP_CSC))

#define VIG_MSM8996_MASK \
	(BIT(DPU_SSPP_QOS) | BIT(DPU_SSPP_CDP) |\
	 BIT(DPU_SSPP_TS_PREFILL) | BIT(DPU_SSPP_SCALER_QSEED2) |\
	 BIT(DPU_SSPP_CSC))

#define VIG_MSM8998_MASK \
	(VIG_MASK | BIT(DPU_SSPP_SCALER_QSEED3_COMPATIBLE))

#define VIG_SDM845_MASK_NO_SDMA \
	(VIG_MASK | BIT(DPU_SSPP_QOS_8LVL) | BIT(DPU_SSPP_SCALER_QSEED3_COMPATIBLE))

#define VIG_SDM845_MASK_SDMA \
	(VIG_SDM845_MASK_NO_SDMA | BIT(DPU_SSPP_SMART_DMA_V2))

#define VIG_QCM2290_MASK (VIG_BASE_MASK | BIT(DPU_SSPP_QOS_8LVL))

#define DMA_MSM8953_MASK \
	(BIT(DPU_SSPP_QOS))

#define DMA_MSM8996_MASK \
	(BIT(DPU_SSPP_QOS) | BIT(DPU_SSPP_TS_PREFILL) | BIT(DPU_SSPP_CDP))

#define DMA_MSM8998_MASK \
	(BIT(DPU_SSPP_QOS) |\
	BIT(DPU_SSPP_TS_PREFILL) | BIT(DPU_SSPP_TS_PREFILL_REC1) |\
	BIT(DPU_SSPP_CDP) | BIT(DPU_SSPP_EXCL_RECT))

#define VIG_SC7280_MASK \
	(VIG_SDM845_MASK_NO_SDMA | BIT(DPU_SSPP_INLINE_ROTATION))

#define VIG_SC7280_MASK_SDMA \
	(VIG_SC7280_MASK | BIT(DPU_SSPP_SMART_DMA_V2))

#define DMA_SDM845_MASK_NO_SDMA \
	(BIT(DPU_SSPP_QOS) | BIT(DPU_SSPP_QOS_8LVL) |\
	BIT(DPU_SSPP_TS_PREFILL) | BIT(DPU_SSPP_TS_PREFILL_REC1) |\
	BIT(DPU_SSPP_CDP) | BIT(DPU_SSPP_EXCL_RECT))

#define DMA_CURSOR_SDM845_MASK_NO_SDMA \
	(DMA_SDM845_MASK_NO_SDMA | BIT(DPU_SSPP_CURSOR))

#define DMA_SDM845_MASK_SDMA \
	(DMA_SDM845_MASK_NO_SDMA | BIT(DPU_SSPP_SMART_DMA_V2))

#define DMA_CURSOR_SDM845_MASK_SDMA \
	(DMA_CURSOR_SDM845_MASK_NO_SDMA | BIT(DPU_SSPP_SMART_DMA_V2))

#define DMA_CURSOR_MSM8996_MASK \
	(DMA_MSM8996_MASK | BIT(DPU_SSPP_CURSOR))

#define DMA_CURSOR_MSM8998_MASK \
	(DMA_MSM8998_MASK | BIT(DPU_SSPP_CURSOR))

#define RGB_MSM8953_MASK \
	(BIT(DPU_SSPP_QOS))

#define RGB_MSM8996_MASK \
	(BIT(DPU_SSPP_QOS) | BIT(DPU_SSPP_CDP) |\
	 BIT(DPU_SSPP_TS_PREFILL) | BIT(DPU_SSPP_SCALER_RGB))

#define MIXER_MSM8998_MASK \
	(BIT(DPU_MIXER_SOURCESPLIT))

#define MIXER_SDM845_MASK \
	(BIT(DPU_MIXER_SOURCESPLIT) | BIT(DPU_DIM_LAYER) | BIT(DPU_MIXER_COMBINED_ALPHA))

#define MIXER_QCM2290_MASK \
	(BIT(DPU_DIM_LAYER) | BIT(DPU_MIXER_COMBINED_ALPHA))

#define PINGPONG_MSM8996_MASK \
	(BIT(DPU_PINGPONG_DSC))

#define PINGPONG_SDM845_MASK \
	(BIT(DPU_PINGPONG_DITHER) | BIT(DPU_PINGPONG_DSC))

#define PINGPONG_SM8150_MASK \
	(BIT(DPU_PINGPONG_DITHER) | BIT(DPU_PINGPONG_DSC))

#define CTL_SC7280_MASK \
	(BIT(DPU_CTL_ACTIVE_CFG) | \
	 BIT(DPU_CTL_FETCH_ACTIVE) | \
	 BIT(DPU_CTL_VM_CFG) | \
	 BIT(DPU_CTL_DSPP_SUB_BLOCK_FLUSH))

#define CTL_SM8550_MASK \
	(CTL_SC7280_MASK | BIT(DPU_CTL_HAS_LAYER_EXT4))

#define DSPP_SC7180_MASK BIT(DPU_DSPP_PCC)

#define INTF_SC7180_MASK \
	(BIT(DPU_INTF_INPUT_CTRL) | \
	 BIT(DPU_INTF_STATUS_SUPPORTED) | \
	 BIT(DPU_DATA_HCTL_EN))

#define INTF_SC7280_MASK (INTF_SC7180_MASK)

#define WB_SDM845_MASK (BIT(DPU_WB_LINE_MODE) | \
			 BIT(DPU_WB_UBWC) | \
			 BIT(DPU_WB_YUV_CONFIG) | \
			 BIT(DPU_WB_PIPE_ALPHA) | \
			 BIT(DPU_WB_XY_ROI_OFFSET) | \
			 BIT(DPU_WB_QOS) | \
			 BIT(DPU_WB_QOS_8LVL) | \
			 BIT(DPU_WB_CDP))

#define WB_SM8250_MASK (WB_SDM845_MASK | \
			 BIT(DPU_WB_INPUT_CTRL))

#define DEFAULT_PIXEL_RAM_SIZE		(50 * 1024)
#define DEFAULT_DPU_LINE_WIDTH		2048
#define DEFAULT_DPU_OUTPUT_LINE_WIDTH	2560

#define MAX_HORZ_DECIMATION	4
#define MAX_VERT_DECIMATION	4

#define STRCAT(X, Y) (X Y)

static const uint32_t plane_formats[] = {
	VIDEO_RGBA8888,
};

static const u32 wb2_formats_rgb_yuv[] = {
	VIDEO_RGBA8888,
};

/*************************************************************
 * SSPP sub blocks config
 *************************************************************/

#define SSPP_SCALER_VER(maj, min) (((maj) << 16) | (min))

/* SSPP common configuration */
#define _RGB_SBLK() \
	{ \
	.scaler_blk = {.name = "scaler", \
		.base = 0x200, .len = 0x28,}, \
	.format_list = plane_formats, \
	.num_formats = ARRAY_SIZE(plane_formats), \
	}

#define _DMA_SBLK() \
	{ \
	.format_list = plane_formats, \
	.num_formats = ARRAY_SIZE(plane_formats), \
	}

static const struct dpu_sspp_sub_blks dpu_dma_sblk = _DMA_SBLK();

/*************************************************************
 * MIXER sub blocks config
 *************************************************************/
static const struct dpu_lm_sub_blks sdm845_lm_sblk = {
	.maxwidth = DEFAULT_DPU_OUTPUT_LINE_WIDTH,
	.maxblendstages = 11, /* excluding base layer */
	.blendstage_base = { /* offsets relative to mixer base */
		0x20, 0x38, 0x50, 0x68, 0x80, 0x98,
		0xb0, 0xc8, 0xe0, 0xf8, 0x110
	},
};

/*************************************************************
 * DSPP sub blocks config
 *************************************************************/
static const struct dpu_dspp_sub_blks sdm845_dspp_sblk = {
	.pcc = {.name = "pcc", .base = 0x1700,
		.len = 0x90, .version = 0x40000},
};

/*************************************************************
 * PINGPONG sub blocks config
 *************************************************************/
static const struct dpu_pingpong_sub_blks sc7280_pp_sblk = {
	.dither = {.name = "dither", .base = 0xe0,
	.len = 0x20, .version = 0x20000},
};

/*************************************************************
 * DSC sub blocks config
 *************************************************************/
static const struct dpu_dsc_sub_blks dsc_sblk_0 = {
	.enc = {.name = "enc", .base = 0x100, .len = 0x9c},
	.ctl = {.name = "ctl", .base = 0xF00, .len = 0x10},
};

static const struct dpu_dsc_sub_blks dsc_sblk_1 = {
	.enc = {.name = "enc", .base = 0x200, .len = 0x9c},
	.ctl = {.name = "ctl", .base = 0xF80, .len = 0x10},
};

/*************************************************************
 * CDM block config
 *************************************************************/
static const struct dpu_cdm_cfg dpu_cdm_5_x = {
	.name = "cdm_0",
	.id = CDM_0,
	.len = 0x228,
	.base = 0x79200,
};

/*************************************************************
 * VBIF sub blocks config
 *************************************************************/
/* VBIF QOS remap */
static const u32 sdm845_nrt_pri_lvl[] = {3, 3, 3, 3, 3, 3, 3, 3};
static const u32 sm8650_rt_pri_lvl[] = {4, 4, 5, 5, 5, 5, 5, 6};

static const struct dpu_vbif_cfg sm8650_vbif[] = {
	{
	.name = "vbif_rt", .id = VBIF_RT,
	.base = 0, .len = 0x1074,
	.features = BIT(DPU_VBIF_QOS_REMAP),
	.xin_halt_timeout = 0x4000,
	.qos_rp_remap_size = 0x40,
	.qos_rt_tbl = {
		.npriority_lvl = ARRAY_SIZE(sm8650_rt_pri_lvl),
		.priority_lvl = sm8650_rt_pri_lvl,
		},
	.qos_nrt_tbl = {
		.npriority_lvl = ARRAY_SIZE(sdm845_nrt_pri_lvl),
		.priority_lvl = sdm845_nrt_pri_lvl,
		},
	.memtype_count = 16,
	.memtype = {3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3},
	},
};

/*************************************************************
 * PERF data config
 *************************************************************/

/* SSPP QOS LUTs */
static const struct dpu_qos_lut_entry sc7180_qos_linear[] = {
	{.fl = 0, .lut = 0x0011222222335777},
};

static const struct dpu_qos_lut_entry sc7180_qos_macrotile[] = {
	{.fl = 0, .lut = 0x0011223344556677},
};

static const struct dpu_qos_lut_entry sc7180_qos_nrt[] = {
	{.fl = 0, .lut = 0x0},
};

/*************************************************************
 * Hardware catalog
 *************************************************************/
#include "catalog/dpu_10_0_sm8650.h"
