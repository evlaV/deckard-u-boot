/* SPDX-License-Identifier: GPL-2.0-only */

/* this file handles configuration of VBIF/CTL/SSPP/INTF/LM/PP/DSC in DPU */

#define DEBUG 7
#include <asm/io.h>
#include <clk.h>
#include <dm/device.h>
#include <dm/device_compat.h>
#include <dm/read.h>
#include <video.h>
#include <video_bridge.h>
#include <linux/sizes.h>

#include "dpu_hw_mdss.h"
#include "dpu_hwio.h"
#include "catalog/dpu_hw_catalog.h"

#define FLD_INTF_2_SW_TRG_MUX             BIT(8)
#define SPLIT_DISPLAY_LOWER_PIPE_CTRL   0x3F0

/* ==================== CTL registers ===================== */
#define   CTL_LAYER(lm)                 \
	(((lm) == LM_5) ? (0x024) : (((lm) - LM_0) * 0x004))
#define   CTL_LAYER_EXT(lm)             \
	(0x40 + (((lm) - LM_0) * 0x004))
#define   CTL_LAYER_EXT2(lm)             \
	(0x70 + (((lm) - LM_0) * 0x004))
#define   CTL_LAYER_EXT3(lm)             \
	(0xA0 + (((lm) - LM_0) * 0x004))
#define CTL_LAYER_EXT4(lm)             \
	(0xB8 + (((lm) - LM_0) * 0x004))
#define   CTL_TOP                       0x014
#define   CTL_FLUSH                     0x018
#define   CTL_START                     0x01C
#define   CTL_PREPARE                   0x0d0
#define   CTL_SW_RESET                  0x030
#define   CTL_LAYER_EXTN_OFFSET         0x40
#define   CTL_MERGE_3D_ACTIVE           0x0E4
#define   CTL_DSC_ACTIVE                0x0E8
#define   CTL_WB_ACTIVE                 0x0EC
#define   CTL_CWB_ACTIVE                0x0F0
#define   CTL_INTF_ACTIVE               0x0F4
#define   CTL_CDM_ACTIVE                0x0F8
#define   CTL_FETCH_PIPE_ACTIVE         0x0FC
#define   CTL_MERGE_3D_FLUSH            0x100
#define   CTL_DSC_FLUSH                0x104
#define   CTL_WB_FLUSH                  0x108
#define   CTL_CWB_FLUSH                 0x10C
#define   CTL_INTF_FLUSH                0x110
#define   CTL_CDM_FLUSH                0x114
#define   CTL_PERIPH_FLUSH              0x128
#define   CTL_INTF_MASTER               0x134
#define   CTL_DSPP_n_FLUSH(n)           ((0x13C) + ((n) * 4))

#define CTL_MIXER_BORDER_OUT            BIT(24)
#define CTL_FLUSH_MASK_CTL              BIT(17)

#define DPU_REG_RESET_TIMEOUT_US        2000
#define  MERGE_3D_IDX   23
#define  DSC_IDX        22
#define CDM_IDX         26
#define  PERIPH_IDX     30
#define  INTF_IDX       31
#define WB_IDX          16
#define CWB_IDX         28
#define  DSPP_IDX       29  /* From DPU hw rev 7.x.x */
#define CTL_INVALID_BIT                 0xffff
#define CTL_DEFAULT_GROUP_ID		0xf


/* ==================== SSPP registers ===================== */
#define SSPP_SRC_SIZE                      0x00
#define SSPP_SRC_XY                        0x08
#define SSPP_OUT_SIZE                      0x0c
#define SSPP_OUT_XY                        0x10
#define SSPP_SRC0_ADDR                     0x14
#define SSPP_SRC1_ADDR                     0x18
#define SSPP_SRC2_ADDR                     0x1C
#define SSPP_SRC3_ADDR                     0x20
#define SSPP_SRC_YSTRIDE0                  0x24
#define SSPP_SRC_YSTRIDE1                  0x28
#define SSPP_SRC_FORMAT                    0x30
#define SSPP_SRC_UNPACK_PATTERN            0x34
#define SSPP_SRC_OP_MODE                   0x38
#define SSPP_SRC_CONSTANT_COLOR            0x3c
#define SSPP_EXCL_REC_CTL                  0x40
#define SSPP_UBWC_STATIC_CTRL              0x44
#define SSPP_FETCH_CONFIG                  0x48
#define SSPP_DANGER_LUT                    0x60
#define SSPP_SAFE_LUT                      0x64
#define SSPP_CREQ_LUT                      0x68
#define SSPP_QOS_CTRL                      0x6C
#define SSPP_SRC_ADDR_SW_STATUS            0x70
#define SSPP_CREQ_LUT_0                    0x74
#define SSPP_CREQ_LUT_1                    0x78
#define SSPP_DECIMATION_CONFIG             0xB4
#define SSPP_SW_PIX_EXT_C0_LR              0x100
#define SSPP_SW_PIX_EXT_C0_TB              0x104
#define SSPP_SW_PIX_EXT_C0_REQ_PIXELS      0x108
#define SSPP_SW_PIX_EXT_C1C2_LR            0x110
#define SSPP_SW_PIX_EXT_C1C2_TB            0x114
#define SSPP_SW_PIX_EXT_C1C2_REQ_PIXELS    0x118
#define SSPP_SW_PIX_EXT_C3_LR              0x120
#define SSPP_SW_PIX_EXT_C3_TB              0x124
#define SSPP_SW_PIX_EXT_C3_REQ_PIXELS      0x128
#define SSPP_TRAFFIC_SHAPER                0x130
#define SSPP_CDP_CNTL                      0x134
#define SSPP_UBWC_ERROR_STATUS             0x138
#define SSPP_CDP_CNTL_REC1                 0x13c
#define SSPP_TRAFFIC_SHAPER_PREFILL        0x150
#define SSPP_TRAFFIC_SHAPER_REC1_PREFILL   0x154
#define SSPP_TRAFFIC_SHAPER_REC1           0x158
#define SSPP_OUT_SIZE_REC1                 0x160
#define SSPP_OUT_XY_REC1                   0x164
#define SSPP_SRC_XY_REC1                   0x168
#define SSPP_SRC_SIZE_REC1                 0x16C
#define SSPP_MULTIRECT_OPMODE              0x170
#define SSPP_SRC_FORMAT_REC1               0x174
#define SSPP_SRC_UNPACK_PATTERN_REC1       0x178
#define SSPP_SRC_OP_MODE_REC1              0x17C
#define SSPP_SRC_CONSTANT_COLOR_REC1       0x180
#define SSPP_EXCL_REC_SIZE_REC1            0x184
#define SSPP_EXCL_REC_XY_REC1              0x188
#define SSPP_EXCL_REC_SIZE                 0x1B4
#define SSPP_EXCL_REC_XY                   0x1B8
#define SSPP_CLK_CTRL                      0x330

/* SSPP_SRC_OP_MODE & OP_MODE_REC1 */
#define MDSS_MDP_OP_DEINTERLACE            BIT(22)
#define MDSS_MDP_OP_DEINTERLACE_ODD        BIT(23)
#define MDSS_MDP_OP_IGC_ROM_1              BIT(18)
#define MDSS_MDP_OP_IGC_ROM_0              BIT(17)
#define MDSS_MDP_OP_IGC_EN                 BIT(16)
#define MDSS_MDP_OP_FLIP_UD                BIT(14)
#define MDSS_MDP_OP_FLIP_LR                BIT(13)
#define MDSS_MDP_OP_BWC_EN                 BIT(0)
#define MDSS_MDP_OP_PE_OVERRIDE            BIT(31)
#define MDSS_MDP_OP_BWC_LOSSLESS           (0 << 1)
#define MDSS_MDP_OP_BWC_Q_HIGH             (1 << 1)
#define MDSS_MDP_OP_BWC_Q_MED              (2 << 1)

/* SSPP_QOS_CTRL */
#define SSPP_QOS_CTRL_VBLANK_EN            BIT(16)
#define SSPP_QOS_CTRL_DANGER_SAFE_EN       BIT(0)
#define SSPP_QOS_CTRL_DANGER_VBLANK_MASK   0x3
#define SSPP_QOS_CTRL_DANGER_VBLANK_OFF    4
#define SSPP_QOS_CTRL_CREQ_VBLANK_MASK     0x3
#define SSPP_QOS_CTRL_CREQ_VBLANK_OFF      20

/* DPU_SSPP_SCALER_QSEED2 */
#define SSPP_VIG_OP_MODE                   0x0
#define SCALE_CONFIG                       0x04
#define COMP0_3_PHASE_STEP_X               0x10
#define COMP0_3_PHASE_STEP_Y               0x14
#define COMP1_2_PHASE_STEP_X               0x18
#define COMP1_2_PHASE_STEP_Y               0x1c
#define COMP0_3_INIT_PHASE_X               0x20
#define COMP0_3_INIT_PHASE_Y               0x24
#define COMP1_2_INIT_PHASE_X               0x28
#define COMP1_2_INIT_PHASE_Y               0x2C
#define VIG_0_QSEED2_SHARP                 0x30

/* SSPP_TRAFFIC_SHAPER and _REC1 */
#define SSPP_TRAFFIC_SHAPER_BPC_MAX        0xFF

/*
 * Definitions for ViG op modes
 */
#define VIG_OP_CSC_DST_DATAFMT BIT(19)
#define VIG_OP_CSC_SRC_DATAFMT BIT(18)
#define VIG_OP_CSC_EN          BIT(17)
#define VIG_OP_MEM_PROT_CONT   BIT(15)
#define VIG_OP_MEM_PROT_VAL    BIT(14)
#define VIG_OP_MEM_PROT_SAT    BIT(13)
#define VIG_OP_MEM_PROT_HUE    BIT(12)
#define VIG_OP_HIST            BIT(8)
#define VIG_OP_SKY_COL         BIT(7)
#define VIG_OP_FOIL            BIT(6)
#define VIG_OP_SKIN_COL        BIT(5)
#define VIG_OP_PA_EN           BIT(4)
#define VIG_OP_PA_SAT_ZERO_EXP BIT(2)
#define VIG_OP_MEM_PROT_BLEND  BIT(1)

/*
 * Definitions for CSC 10 op modes
 */
#define SSPP_VIG_CSC_10_OP_MODE            0x0
#define VIG_CSC_10_SRC_DATAFMT BIT(1)
#define VIG_CSC_10_EN          BIT(0)
#define CSC_10BIT_OFFSET       4

/* ==================== Mixer registers ===================== */
#define LM_OP_MODE                        0x00
#define LM_OUT_SIZE                       0x04
#define LM_BORDER_COLOR_0                 0x08
#define LM_BORDER_COLOR_1                 0x010

/* These register are offset to mixer base + stage base */
#define LM_BLEND0_OP                     0x20
#define LM_BLEND0_CONST_ALPHA            0x24
#define LM_FG_COLOR_FILL_COLOR_0         0x28
#define LM_FG_COLOR_FILL_COLOR_1         0x2C
#define LM_FG_COLOR_FILL_SIZE            0x30
#define LM_FG_COLOR_FILL_XY              0x34

#define LM_BLEND0_FG_ALPHA               0x04
#define LM_BLEND0_BG_ALPHA               0x08

#define LM_MISR_CTRL                     0x310
#define LM_MISR_SIGNATURE                0x314

/* ==================== VBIF registers ===================== */
#define VBIF_VERSION			0x0000
#define VBIF_CLK_FORCE_CTRL0		0x0008
#define VBIF_CLK_FORCE_CTRL1		0x000C
#define VBIF_QOS_REMAP_00		0x0020
#define VBIF_QOS_REMAP_01		0x0024
#define VBIF_QOS_REMAP_10		0x0028
#define VBIF_QOS_REMAP_11		0x002C
#define VBIF_WRITE_GATHER_EN		0x00AC
#define VBIF_IN_RD_LIM_CONF0		0x00B0
#define VBIF_IN_RD_LIM_CONF1		0x00B4
#define VBIF_IN_RD_LIM_CONF2		0x00B8
#define VBIF_IN_WR_LIM_CONF0		0x00C0
#define VBIF_IN_WR_LIM_CONF1		0x00C4
#define VBIF_IN_WR_LIM_CONF2		0x00C8
#define VBIF_OUT_RD_LIM_CONF0		0x00D0
#define VBIF_OUT_WR_LIM_CONF0		0x00D4
#define VBIF_OUT_AXI_AMEMTYPE_CONF0	0x0160
#define VBIF_OUT_AXI_AMEMTYPE_CONF1	0x0164
#define VBIF_XIN_PND_ERR		0x0190
#define VBIF_XIN_SRC_ERR		0x0194
#define VBIF_XIN_CLR_ERR		0x019C
#define VBIF_XIN_HALT_CTRL0		0x0200
#define VBIF_XIN_HALT_CTRL1		0x0204
#define VBIF_XINL_QOS_RP_REMAP_000	0x0550
#define VBIF_XINL_QOS_LVL_REMAP_000(vbif)	(VBIF_XINL_QOS_RP_REMAP_000 + (vbif)->cap->qos_rp_remap_size)


/* ==================== pingpong registers ===================== */
#define PP_TEAR_CHECK_EN                0x000
#define PP_SYNC_CONFIG_VSYNC            0x004
#define PP_SYNC_CONFIG_HEIGHT           0x008
#define PP_SYNC_WRCOUNT                 0x00C
#define PP_VSYNC_INIT_VAL               0x010
#define PP_INT_COUNT_VAL                0x014
#define PP_SYNC_THRESH                  0x018
#define PP_START_POS                    0x01C
#define PP_RD_PTR_IRQ                   0x020
#define PP_WR_PTR_IRQ                   0x024
#define PP_OUT_LINE_COUNT               0x028
#define PP_LINE_COUNT                   0x02C
#define PP_AUTOREFRESH_CONFIG           0x030

#define PP_FBC_MODE                     0x034
#define PP_FBC_BUDGET_CTL               0x038
#define PP_FBC_LOSSY_MODE               0x03C
#define PP_DSC_MODE                     0x0a0
#define PP_DCE_DATA_IN_SWAP             0x0ac
#define PP_DCE_DATA_OUT_SWAP            0x0c8

#define PP_DITHER_EN			0x000
#define PP_DITHER_BITDEPTH		0x004
#define PP_DITHER_MATRIX		0x008

#define DITHER_DEPTH_MAP_INDEX 9


/* ==================== interface registers ===================== */
#define INTF_TIMING_ENGINE_EN           0x000
#define INTF_CONFIG                     0x004
#define INTF_HSYNC_CTL                  0x008
#define INTF_VSYNC_PERIOD_F0            0x00C
#define INTF_VSYNC_PERIOD_F1            0x010
#define INTF_VSYNC_PULSE_WIDTH_F0       0x014
#define INTF_VSYNC_PULSE_WIDTH_F1       0x018
#define INTF_DISPLAY_V_START_F0         0x01C
#define INTF_DISPLAY_V_START_F1         0x020
#define INTF_DISPLAY_V_END_F0           0x024
#define INTF_DISPLAY_V_END_F1           0x028
#define INTF_ACTIVE_V_START_F0          0x02C
#define INTF_ACTIVE_V_START_F1          0x030
#define INTF_ACTIVE_V_END_F0            0x034
#define INTF_ACTIVE_V_END_F1            0x038
#define INTF_DISPLAY_HCTL               0x03C
#define INTF_ACTIVE_HCTL                0x040
#define INTF_BORDER_COLOR               0x044
#define INTF_UNDERFLOW_COLOR            0x048
#define INTF_HSYNC_SKEW                 0x04C
#define INTF_POLARITY_CTL               0x050
#define INTF_TEST_CTL                   0x054
#define INTF_TP_COLOR0                  0x058
#define INTF_TP_COLOR1                  0x05C
#define INTF_CONFIG2                    0x060
#define INTF_DISPLAY_DATA_HCTL          0x064
#define INTF_ACTIVE_DATA_HCTL           0x068

#define INTF_DSI_CMD_MODE_TRIGGER_EN    0x084
#define INTF_PANEL_FORMAT               0x090

#define INTF_FRAME_LINE_COUNT_EN        0x0A8
#define INTF_FRAME_COUNT                0x0AC
#define INTF_LINE_COUNT                 0x0B0

#define INTF_DEFLICKER_CONFIG           0x0F0
#define INTF_DEFLICKER_STRNG_COEFF      0x0F4
#define INTF_DEFLICKER_WEAK_COEFF       0x0F8

#define INTF_TPG_ENABLE                 0x100
#define INTF_TPG_MAIN_CONTROL           0x104
#define INTF_TPG_VIDEO_CONFIG           0x108
#define INTF_TPG_COMPONENT_LIMITS       0x10C
#define INTF_TPG_RECTANGLE              0x110
#define INTF_TPG_INITIAL_VALUE          0x114
#define INTF_TPG_BLK_WHITE_PATTERN_FRAMES 0x118
#define INTF_TPG_RGB_MAPPING            0x11C
#define INTF_PROG_FETCH_START           0x170
#define INTF_PROG_ROT_START             0x174

#define INTF_MISR_CTRL                  0x180
#define INTF_MISR_SIGNATURE             0x184

#define INTF_MUX                        0x25C
#define INTF_STATUS                     0x26C
#define INTF_AVR_CONTROL                0x270
#define INTF_AVR_MODE                   0x274
#define INTF_AVR_TRIGGER                0x278
#define INTF_AVR_VTOTAL                 0x27C
#define INTF_TEAR_MDP_VSYNC_SEL         0x280
#define INTF_TEAR_TEAR_CHECK_EN         0x284
#define INTF_TEAR_SYNC_CONFIG_VSYNC     0x288
#define INTF_TEAR_SYNC_CONFIG_HEIGHT    0x28C
#define INTF_TEAR_SYNC_WRCOUNT          0x290
#define INTF_TEAR_VSYNC_INIT_VAL        0x294
#define INTF_TEAR_INT_COUNT_VAL         0x298
#define INTF_TEAR_SYNC_THRESH           0x29C
#define INTF_TEAR_START_POS             0x2A0
#define INTF_TEAR_RD_PTR_IRQ            0x2A4
#define INTF_TEAR_WR_PTR_IRQ            0x2A8
#define INTF_TEAR_OUT_LINE_COUNT        0x2AC
#define INTF_TEAR_LINE_COUNT            0x2B0
#define INTF_TEAR_AUTOREFRESH_CONFIG    0x2B4

#define INTF_CFG_ACTIVE_H_EN	BIT(29)
#define INTF_CFG_ACTIVE_V_EN	BIT(30)

#define INTF_CFG2_DATABUS_WIDEN	BIT(0)
#define INTF_CFG2_DATA_HCTL_EN	BIT(4)
#define INTF_CFG2_DCE_DATA_COMPRESS     BIT(12)


/* ==================== DSC registers ===================== */
#define DSC_CMN_MAIN_CNF           0x00

/* DPU_DSC_ENC register offsets */
#define ENC_DF_CTRL                0x00
#define ENC_GENERAL_STATUS         0x04
#define ENC_HSLICE_STATUS          0x08
#define ENC_OUT_STATUS             0x0C
#define ENC_INT_STAT               0x10
#define ENC_INT_CLR                0x14
#define ENC_INT_MASK               0x18
#define DSC_MAIN_CONF              0x30
#define DSC_PICTURE_SIZE           0x34
#define DSC_SLICE_SIZE             0x38
#define DSC_MISC_SIZE              0x3C
#define DSC_HRD_DELAYS             0x40
#define DSC_RC_SCALE               0x44
#define DSC_RC_SCALE_INC_DEC       0x48
#define DSC_RC_OFFSETS_1           0x4C
#define DSC_RC_OFFSETS_2           0x50
#define DSC_RC_OFFSETS_3           0x54
#define DSC_RC_OFFSETS_4           0x58
#define DSC_FLATNESS_QP            0x5C
#define DSC_RC_MODEL_SIZE          0x60
#define DSC_RC_CONFIG              0x64
#define DSC_RC_BUF_THRESH_0        0x68
#define DSC_RC_BUF_THRESH_1        0x6C
#define DSC_RC_BUF_THRESH_2        0x70
#define DSC_RC_BUF_THRESH_3        0x74
#define DSC_RC_MIN_QP_0            0x78
#define DSC_RC_MIN_QP_1            0x7C
#define DSC_RC_MIN_QP_2            0x80
#define DSC_RC_MAX_QP_0            0x84
#define DSC_RC_MAX_QP_1            0x88
#define DSC_RC_MAX_QP_2            0x8C
#define DSC_RC_RANGE_BPG_OFFSETS_0 0x90
#define DSC_RC_RANGE_BPG_OFFSETS_1 0x94
#define DSC_RC_RANGE_BPG_OFFSETS_2 0x98

/* DPU_DSC_CTL register offsets */
#define DSC_CTL                    0x00
#define DSC_CFG                    0x04
#define DSC_DATA_IN_SWAP           0x08
#define DSC_CLK_CTRL               0x0C

#define dpu_write(reg, val) writel(val, reg)

struct qcom_dpu_priv {
	void *dpu_base;
	void *vbif_base;
	const struct dpu_mdss_cfg *cat;
	struct clk_bulk bulk;
	struct udevice *bridge0;
	struct udevice *bridge1;
};

#if 0
struct qcom_dpu_priv *local_priv;
struct dpu_kms {
	void *mmio;
} dpu_kms_inst;

static void dpu_kms_mdp_snapshot(void)
{
	int i;
	const struct dpu_mdss_cfg *cat = local_priv->cat;
	struct dpu_kms *dpu_kms = &dpu_kms_inst;
	void __iomem *base;

	dpu_kms->mmio = local_priv->dpu_base;

	/* dump CTL sub-blocks HW regs info */
	for (i = 0; i < cat->ctl_count; i++)
		msm_disp_snapshot_add_block(cat->ctl[i].len,
				dpu_kms->mmio + cat->ctl[i].base, "%s",
				cat->ctl[i].name);

	/* dump DSPP sub-blocks HW regs info */
	for (i = 0; i < cat->dspp_count; i++) {
		base = dpu_kms->mmio + cat->dspp[i].base;
		msm_disp_snapshot_add_block(cat->dspp[i].len, base,
					    "%s", cat->dspp[i].name);

		if (cat->dspp[i].sblk && cat->dspp[i].sblk->pcc.len > 0)
			msm_disp_snapshot_add_block(cat->dspp[i].sblk->pcc.len,
						    base + cat->dspp[i].sblk->pcc.base, "%s_%s",
						    cat->dspp[i].name,
						    cat->dspp[i].sblk->pcc.name);
	}

	/* dump INTF sub-blocks HW regs info */
	for (i = 0; i < cat->intf_count; i++)
		msm_disp_snapshot_add_block(cat->intf[i].len,
				dpu_kms->mmio + cat->intf[i].base, "%s",
				cat->intf[i].name);

	/* dump PP sub-blocks HW regs info */
	for (i = 0; i < cat->pingpong_count; i++) {
		base = dpu_kms->mmio + cat->pingpong[i].base;
		msm_disp_snapshot_add_block(cat->pingpong[i].len, base,
					    "%s", cat->pingpong[i].name);

		/* TE2 sub-block has length of 0, so will not print it */

		if (cat->pingpong[i].sblk && cat->pingpong[i].sblk->dither.len > 0)
			msm_disp_snapshot_add_block(cat->pingpong[i].sblk->dither.len,
						    base + cat->pingpong[i].sblk->dither.base,
						    "%s_%s", cat->pingpong[i].name,
						    cat->pingpong[i].sblk->dither.name);
	}

	/* dump SSPP sub-blocks HW regs info */
	for (i = 0; i < cat->sspp_count; i++) {
		base = dpu_kms->mmio + cat->sspp[i].base;
		msm_disp_snapshot_add_block(cat->sspp[i].len, base,
					    "%s", cat->sspp[i].name);

		if (cat->sspp[i].sblk && cat->sspp[i].sblk->scaler_blk.len > 0)
			msm_disp_snapshot_add_block(cat->sspp[i].sblk->scaler_blk.len,
						    base + cat->sspp[i].sblk->scaler_blk.base,
						    "%s_%s", cat->sspp[i].name,
						    cat->sspp[i].sblk->scaler_blk.name);

		if (cat->sspp[i].sblk && cat->sspp[i].sblk->csc_blk.len > 0)
			msm_disp_snapshot_add_block(cat->sspp[i].sblk->csc_blk.len,
						    base + cat->sspp[i].sblk->csc_blk.base,
						    "%s_%s", cat->sspp[i].name,
						    cat->sspp[i].sblk->csc_blk.name);
	}

	/* dump LM sub-blocks HW regs info */
	for (i = 0; i < cat->mixer_count; i++)
		msm_disp_snapshot_add_block(cat->mixer[i].len,
				dpu_kms->mmio + cat->mixer[i].base,
				"%s", cat->mixer[i].name);

	/* dump WB sub-blocks HW regs info */
	for (i = 0; i < cat->wb_count; i++)
		msm_disp_snapshot_add_block(cat->wb[i].len,
				dpu_kms->mmio + cat->wb[i].base, "%s",
				cat->wb[i].name);

	if (cat->mdp[0].features & BIT(DPU_MDP_PERIPH_0_REMOVED)) {
		msm_disp_snapshot_add_block(MDP_PERIPH_TOP0,
				dpu_kms->mmio + cat->mdp[0].base, "top");
		msm_disp_snapshot_add_block(cat->mdp[0].len - MDP_PERIPH_TOP0_END,
				dpu_kms->mmio + cat->mdp[0].base + MDP_PERIPH_TOP0_END, "top_2");
	} else {
		msm_disp_snapshot_add_block(cat->mdp[0].len,
				dpu_kms->mmio + cat->mdp[0].base, "top");
	}

	/* dump CWB sub-blocks HW regs info */
	for (i = 0; i < cat->cwb_count; i++)
		msm_disp_snapshot_add_block(cat->cwb[i].len,
					    dpu_kms->mmio + cat->cwb[i].base, cat->cwb[i].name);

	/* dump DSC sub-blocks HW regs info */
	for (i = 0; i < cat->dsc_count; i++) {
		base = dpu_kms->mmio + cat->dsc[i].base;
		msm_disp_snapshot_add_block(cat->dsc[i].len, base,
					    "%s", cat->dsc[i].name);

		if (cat->dsc[i].features & BIT(DPU_DSC_HW_REV_1_2)) {
			struct dpu_dsc_blk enc = cat->dsc[i].sblk->enc;
			struct dpu_dsc_blk ctl = cat->dsc[i].sblk->ctl;

			msm_disp_snapshot_add_block(enc.len, base + enc.base, "%s_%s",
						    cat->dsc[i].name, enc.name);
			msm_disp_snapshot_add_block(ctl.len, base + ctl.base, "%s_%s",
						    cat->dsc[i].name, ctl.name);
		}
	}

	if (cat->cdm)
		msm_disp_snapshot_add_block(cat->cdm->len,
					    dpu_kms->mmio + cat->cdm->base,
					    "%s", cat->cdm->name);

	for (i = 0; i < local_priv->cat->vbif_count; i++) {
		const struct dpu_vbif_cfg *vbif = &local_priv->cat->vbif[i];

		msm_disp_snapshot_add_block(vbif->len,
					    local_priv->vbif_base + vbif->base,
					    "%s", vbif->name);
	}
}
#endif

static void mdss_sspp_config_rec1(void *dpu_base, const struct dpu_mdss_cfg *cat, uint32_t fb,
				  unsigned fb_stride, unsigned sspp_id, unsigned height,
				  unsigned width, int x_off)
{
	void *base = NULL;
	int i;

	for (i = 0; i < cat->sspp_count; ++i) {
		if (cat->sspp[i].id == sspp_id) {
			base = dpu_base + cat->sspp[i].base;
			break;
		}
	}

	if (!base) {
		WARN_ON(!base);
		return;
	}

	// TOFIX use define for all bits & values
	writel(fb, base + SSPP_SRC0_ADDR);
	writel(x_off, base + SSPP_SRC_XY);
	writel((height << 16) | (width >> 1), base + SSPP_SRC_SIZE);
	writel((height << 16) | (width >> 1), base + SSPP_OUT_SIZE);
	writel(0, base + SSPP_OUT_XY);
	writel(0x236FF, base + SSPP_SRC_FORMAT);
	writel(0x03020001, base + SSPP_SRC_UNPACK_PATTERN);
	writel(0x80000000, base + SSPP_SRC_OP_MODE);
	writel(0x9, base + SSPP_CDP_CNTL);

	writel(fb, base + SSPP_SRC1_ADDR);
	writel(x_off + (width >> 1), base + SSPP_SRC_XY_REC1);
	writel((height << 16) | (width >> 1), base + SSPP_SRC_SIZE_REC1);
	writel((height << 16) | (width >> 1), base + SSPP_OUT_SIZE_REC1);
	writel((width >> 1), base + SSPP_OUT_XY_REC1);
	writel(0x236FF, base + SSPP_SRC_FORMAT_REC1);
	writel(0x03020001, base + SSPP_SRC_UNPACK_PATTERN_REC1);
	writel(0x80000000, base + SSPP_SRC_OP_MODE_REC1);
	writel(0x9, base + SSPP_CDP_CNTL_REC1);

	writel(0x87, base + SSPP_FETCH_CONFIG);

	writel((fb_stride << 16) | fb_stride, base + SSPP_SRC_YSTRIDE0);
	writel(0, base + SSPP_SRC_YSTRIDE1);
	writel(0, base + SSPP_SW_PIX_EXT_C0_LR);
	writel(0, base + SSPP_SW_PIX_EXT_C0_TB);
	writel((height << 16) | (width >> 1), base + SSPP_SW_PIX_EXT_C0_REQ_PIXELS);
	writel(0, base + SSPP_SW_PIX_EXT_C1C2_LR);
	writel(0, base + SSPP_SW_PIX_EXT_C1C2_TB);
	writel((height << 16) | (width >> 1), base + SSPP_SW_PIX_EXT_C1C2_REQ_PIXELS);
	writel(0, base + SSPP_SW_PIX_EXT_C3_LR);
	writel(0, base + SSPP_SW_PIX_EXT_C3_TB);
	writel((height << 16) | (width >> 1), base + SSPP_SW_PIX_EXT_C3_REQ_PIXELS);
	writel(0x3, base + SSPP_MULTIRECT_OPMODE);
	writel(0x80000000, base + SSPP_UBWC_ERROR_STATUS);
	writel(0x3FFFF, base + SSPP_DANGER_LUT);
	writel(0xFE00, base + SSPP_SAFE_LUT);
	writel(0x22335777, base + SSPP_CREQ_LUT_0);
	writel(0x112222, base + SSPP_CREQ_LUT_1);
	writel(0x1, base + SSPP_QOS_CTRL);
	//writel(0x5, base + SSPP_CLK_CTRL);
}

static void mdss_vbif_setup(void *regs_vbif)
{
	writel(0x33333333, regs_vbif + VBIF_OUT_AXI_AMEMTYPE_CONF0);
   	writel(0x33333333, regs_vbif + VBIF_OUT_AXI_AMEMTYPE_CONF1);

#if 0
	writel(0x400040, regs_vbif + VBIF_XINL_QOS_RP_REMAP_000);
	writel(0x400040, regs_vbif + VBIF_XINL_QOS_RP_REMAP_000 + reg_lvl);
	writel(0x11411141, regs_vbif + VBIF_XINL_QOS_RP_REMAP_000 + 0x8);
	writel(0x11411141, regs_vbif + VBIF_XINL_QOS_RP_REMAP_000 + reg_lvl + 0x8);
	writel(0x22522252, regs_vbif + VBIF_XINL_QOS_RP_REMAP_000 + 0x10);
	writel(0x22522252, regs_vbif + VBIF_XINL_QOS_RP_REMAP_000 + reg_lvl + 0x10);
	writel(0x33533353, regs_vbif + VBIF_XINL_QOS_RP_REMAP_000 + 0x18);
	writel(0x33533353, regs_vbif + VBIF_XINL_QOS_RP_REMAP_000 + reg_lvl + 0x18);
	writel(0x44544454, regs_vbif + VBIF_XINL_QOS_RP_REMAP_000 + 0x20);
	writel(0x44544454, regs_vbif + VBIF_XINL_QOS_RP_REMAP_000 + reg_lvl + 0x20);
	writel(0x55555555, regs_vbif + VBIF_XINL_QOS_RP_REMAP_000 + 0x28);
	writel(0x55555555, regs_vbif + VBIF_XINL_QOS_RP_REMAP_000 + reg_lvl + 0x28);
	writel(0x66566656, regs_vbif + VBIF_XINL_QOS_RP_REMAP_000 + 0x30);
	writel(0x66566656, regs_vbif + VBIF_XINL_QOS_RP_REMAP_000 + reg_lvl + 0x30);
	writel(0x77677767, regs_vbif + VBIF_XINL_QOS_RP_REMAP_000 + 0x38);
	writel(0x77677767, regs_vbif + VBIF_XINL_QOS_RP_REMAP_000 + reg_lvl + 0x38);
#endif
}

static void mdss_intf_setup(void *dpu_base, const struct dpu_mdss_cfg *cat,
			    unsigned intf_id, unsigned pp_id)
{
	unsigned intf = intf_id - INTF_0;
	unsigned pp = pp_id - PINGPONG_0;
	void *base = dpu_base + cat->intf[intf].base;

	/* config for 90Hz */
	// TOFIX calculate from timings and panel data
	writel(BIT(23) | BIT(31), base + INTF_CONFIG); //INTF_CONFIG_DSI_VIDEO_STOP_MODE
	writel(0x3100014, base + INTF_HSYNC_CTL);
	writel(0x001eec90, base + INTF_VSYNC_PERIOD_F0);
	writel(0x310, base + INTF_VSYNC_PULSE_WIDTH_F0);
	writel(0x2F70028, base + INTF_DISPLAY_HCTL);
	writel(0x4050, base + INTF_DISPLAY_V_START_F0);
	writel(0x1A174F, base + INTF_DISPLAY_V_END_F0);
	writel(0, base + INTF_ACTIVE_HCTL);
	writel(0, base + INTF_ACTIVE_V_START_F0);
	writel(0, base + INTF_ACTIVE_V_END_F0);
	writel(0, base + INTF_BORDER_COLOR);
	writel(0xFF, base + INTF_UNDERFLOW_COLOR);
	writel(0, base + INTF_HSYNC_SKEW);
	writel(0, base + INTF_POLARITY_CTL);
	writel(3, base + INTF_FRAME_LINE_COUNT_EN);
	writel(0x213F, base + INTF_PANEL_FORMAT);
	writel(0x1011, base + INTF_CONFIG2);
	writel(0x18F0028, base + INTF_DISPLAY_DATA_HCTL);
	writel(0, base + INTF_ACTIVE_DATA_HCTL);
	writel(0x001ee361, base + INTF_PROG_FETCH_START);

	writel(0xF0000 | pp, base + INTF_MUX);
}

static void mdss_intf_fetch_start_config(void *dpu_base, const struct dpu_mdss_cfg *cat,
					 unsigned ctl_id, unsigned intf_id,
					 unsigned dsc0_id, unsigned dsc1_id)
{
	unsigned intf = intf_id - INTF_0;
	unsigned ctl = ctl_id - CTL_0;
	unsigned dsc0 = dsc0_id - DSC_0;
	unsigned dsc1 = dsc1_id - DSC_0;
	void *ctl_base = dpu_base + cat->ctl[ctl].base;

	/* enable 2 DSI interface */
	writel(BIT(intf), ctl_base + CTL_INTF_MASTER);

	// TOFIX make dsc optional
	writel(BIT(dsc0) | BIT(dsc1), ctl_base + CTL_DSC_ACTIVE);
	writel(BIT(dsc0) | BIT(dsc1), ctl_base + CTL_DSC_FLUSH);

	writel(BIT(intf), ctl_base + CTL_INTF_ACTIVE);
	writel(BIT(intf), ctl_base + CTL_INTF_FLUSH);
}

/* configure the mixer blending in ctl */
void mdp_setup_stage_rec1(void *dpu_base, const struct dpu_mdss_cfg *cat,
		     unsigned ctl_id, unsigned lm_id, unsigned sspp_id,
		     unsigned height, unsigned width, bool split)
{
	unsigned ctl = ctl_id - CTL_0;
	unsigned lm = lm_id - LM_0;
	void *base = dpu_base + cat->ctl[ctl].base;
	void *lm_base = dpu_base + cat->mixer[lm].base;
	u32 op_mode = 0x2;

	switch (sspp_id) {
	case SSPP_DMA0:
		writel(BIT(24) | BIT(19), base + CTL_LAYER(lm_id));
		break;
	case SSPP_DMA1:
		writel(BIT(24) | BIT(22), base + CTL_LAYER(lm_id));
		break;
	default:
		WARN_ON(sspp_id);
	};

	switch (sspp_id) {
	case SSPP_DMA0:
		writel(BIT(9), base + CTL_LAYER_EXT2(lm_id));
		break;
	case SSPP_DMA1:
		writel(BIT(13), base + CTL_LAYER_EXT2(lm_id));
		break;
	default:
		WARN_ON(sspp_id);
	};

	writel(0, base + CTL_LAYER_EXT(lm_id));
	writel(0, base + CTL_LAYER_EXT3(lm_id));
	writel(0, base + CTL_LAYER_EXT4(lm_id));

	writel((height << 16) | (width >> 1), lm_base + LM_OUT_SIZE);

	if (split)
		op_mode |= BIT(31); /* split right half in stage */
	writel(op_mode, lm_base + LM_OP_MODE);

	writel(0x100, lm_base + LM_BLEND0_OP);
	writel(0xFF0000, lm_base + LM_BLEND0_CONST_ALPHA);
}

void mdp_dsc_setup(void *dpu_base, const struct dpu_mdss_cfg *cat, unsigned dsc_id,
		   unsigned pp_id)
{
	unsigned dsc = dsc_id - DSC_0;
	unsigned pp = pp_id - PINGPONG_0;
	void *base = dpu_base + cat->dsc[dsc].base;
	const struct dpu_dsc_sub_blks *sblk = cat->dsc[dsc].sblk;

	// TOFIX calculate from timings and panel data
	dpu_write(base + DSC_CMN_MAIN_CNF, 0x103);
	dpu_write(base + sblk->enc.base + ENC_DF_CTRL, 0x95C0202);
	dpu_write(base + sblk->enc.base + DSC_MAIN_CONF, 0x20120258);
	dpu_write(base + sblk->enc.base + DSC_PICTURE_SIZE, 0x8700870);
	dpu_write(base + sblk->enc.base + DSC_SLICE_SIZE, 0x30021C);
	dpu_write(base + sblk->enc.base + DSC_MISC_SIZE, 0x21C);
	dpu_write(base + sblk->enc.base + DSC_HRD_DELAYS, 0x20E0200);
	dpu_write(base + sblk->enc.base + DSC_RC_SCALE, 0x20);
	dpu_write(base + sblk->enc.base + DSC_RC_SCALE_INC_DEC, 0x704A6);
	dpu_write(base + sblk->enc.base + DSC_RC_OFFSETS_1, 0xC);
	dpu_write(base + sblk->enc.base + DSC_RC_OFFSETS_2, 0x21F020B);
	dpu_write(base + sblk->enc.base + DSC_RC_OFFSETS_3, 0x10F01800);
	dpu_write(base + sblk->enc.base + DSC_RC_OFFSETS_4, 0x0);
	dpu_write(base + sblk->enc.base + DSC_FLATNESS_QP, 0x983);
	dpu_write(base + sblk->enc.base + DSC_RC_MODEL_SIZE, 0x2000);
	dpu_write(base + sblk->enc.base + DSC_RC_CONFIG, 0x3316B06);
	dpu_write(base + sblk->ctl.base + DSC_CFG, 0x3801);
	dpu_write(base + sblk->enc.base + DSC_RC_BUF_THRESH_0, 0x382A1C0E);
	dpu_write(base + sblk->enc.base + DSC_RC_BUF_THRESH_1, 0x69625446);
	dpu_write(base + sblk->enc.base + DSC_RC_BUF_THRESH_2, 0x7B797770);
	dpu_write(base + sblk->enc.base + DSC_RC_BUF_THRESH_3, 0x7E7D);
	dpu_write(base + sblk->enc.base + DSC_RC_MIN_QP_0, 0x308400);
	dpu_write(base + sblk->enc.base + DSC_RC_MAX_QP_0, 0x731484);
	dpu_write(base + sblk->enc.base + DSC_RC_RANGE_BPG_OFFSETS_0, 0x3CF80002);
	dpu_write(base + sblk->enc.base + DSC_RC_MIN_QP_1, 0x318C63);
	dpu_write(base + sblk->enc.base + DSC_RC_MAX_QP_1, 0xA4A0E7);
	dpu_write(base + sblk->enc.base + DSC_RC_RANGE_BPG_OFFSETS_1, 0x36E38E3A);
	dpu_write(base + sblk->enc.base + DSC_RC_MIN_QP_2, 0xD394A5);
	dpu_write(base + sblk->enc.base + DSC_RC_MAX_QP_2, 0xF6B58B);
	dpu_write(base + sblk->enc.base + DSC_RC_RANGE_BPG_OFFSETS_2, 0x34D34D36);
	dpu_write(base + sblk->ctl.base + DSC_CTL, pp);
}

void mdp_ctl_flush(void *dpu_base, const struct dpu_mdss_cfg *cat,
		   unsigned ctl_id, unsigned sspp_id,
		   bool dsc, unsigned lm0_id, unsigned lm1_id)
{
	unsigned ctl = ctl_id - CTL_0;
	void *ctl_base = dpu_base + cat->ctl[ctl].base;
	uint32_t flush = BIT(17) |	// CTL
			 BIT(31); 	// INTF

	switch (sspp_id) {
	case SSPP_DMA0:
		writel(BIT(0), ctl_base + CTL_FETCH_PIPE_ACTIVE);
		flush |= BIT(11);
		break;
	case SSPP_DMA1:
		writel(BIT(1), ctl_base + CTL_FETCH_PIPE_ACTIVE);
		flush |= BIT(12);
		break;
	default:
		WARN_ON(sspp_id);
	}

	switch (lm0_id) {
	case 0:
		break;
	case LM_0:
		flush |= BIT(6);
		break;
	case LM_1:
		flush |= BIT(7);
		break;
	case LM_2:
		flush |= BIT(8);
		break;
	case LM_3:
		flush |= BIT(9);
		break;
	default:
		WARN_ON(lm0_id);
	}

	switch (lm1_id) {
	case 0:
		break;
	case LM_0:
		flush |= BIT(6);
		break;
	case LM_1:
		flush |= BIT(7);
		break;
	case LM_2:
		flush |= BIT(8);
		break;
	case LM_3:
		flush |= BIT(9);
		break;
	default:
		WARN_ON(lm1_id);
	}

	if (dsc)
		flush |= BIT(22);

	writel(0xF0000000, ctl_base + CTL_TOP);
	writel(flush, ctl_base + CTL_FLUSH);
}

void mdp_intf_enable(void *dpu_base, const struct dpu_mdss_cfg *cat, unsigned intf_id)
{
	unsigned intf = intf_id - INTF_0;
	void *base = dpu_base + cat->intf[intf].base;

	writel(0x1, base + INTF_TIMING_ENGINE_EN);
}

void mdp_intf_disable(void *dpu_base, const struct dpu_mdss_cfg *cat, unsigned intf_id)
{
	unsigned intf = intf_id - INTF_0;
	void *base = dpu_base + cat->intf[intf].base;

	writel(0, base + INTF_TIMING_ENGINE_EN);
}

int qcom_dpu_probe(struct udevice *dev)
{
	struct video_uc_plat *uc_plat = dev_get_uclass_plat(dev);
	struct qcom_dpu_priv *priv = dev_get_priv(dev);
	struct video_priv *uc_priv = dev_get_uclass_priv(dev);
	struct display_timing timings, timings1;
	int ret;

	priv->dpu_base = dev_remap_addr_name(dev, "mdp");
	priv->vbif_base = dev_remap_addr_name(dev, "vbif");
	priv->cat = &dpu_sm8650_cfg;

	video_set_flush_dcache(dev, true);

	ret = clk_get_bulk(dev, &priv->bulk);
	if (ret) {
		dev_err(dev, "clk bulk get error %d\n", ret);
		return ret;
	}

	ret = clk_enable_bulk(&priv->bulk);
	if (ret) {
		dev_err(dev, "clk bulk enable error %d\n", ret);
		return ret;
	}

	// TOFIX follow port/endpoint instead
	ret = uclass_get_device(UCLASS_VIDEO_BRIDGE, 0, &priv->bridge0);
	if (ret) {
		dev_dbg(dev, "No video bridge, err %d\n", ret);
		return ret;
	}

	ret = video_bridge_attach(priv->bridge0);
	if (ret) {
		dev_err(dev, "fail to attach bridge\n");
		return ret;
	}

	// TOFIX follow port/endpoint instead
	ret = uclass_get_device(UCLASS_VIDEO_BRIDGE, 1, &priv->bridge1);
	if (ret) {
		dev_dbg(dev, "No video bridge1, err %d\n", ret);
		return ret;
	}

	ret = video_bridge_attach(priv->bridge1);
	if (ret) {
		dev_err(dev, "fail to attach bridge1\n");
		return ret;
	}

	video_bridge_get_display_timing(priv->bridge0, &timings);
	if (ret) {
		dev_err(dev, "fail to get display timings\n");
		return ret;
	}

	video_bridge_get_display_timing(priv->bridge0, &timings1);
	if (ret) {
		dev_err(dev, "fail to get display timings\n");
		return ret;
	}

	ret = video_bridge_set_backlight(priv->bridge0, 100);
	if (ret) {
		dev_err(dev, "bridge enable backlight error %d\n", ret);
		return ret;
	}

	ret = video_bridge_set_backlight(priv->bridge1, 100);
	if (ret) {
		dev_err(dev, "bridge1 enable backlight error %d\n", ret);
		return ret;
	}

	/* Configure & start DPU */

	uc_priv->bpix = VIDEO_BPP32;
	uc_priv->format = VIDEO_X8B8G8R8;
	uc_priv->xsize = timings.hactive.typ + timings1.hactive.typ;
	uc_priv->ysize = timings.vactive.typ;
	// Panels are rotated, vidconsole will rotate all console writes
	uc_priv->rot = 2;

	uc_priv->vrmode = 1;

	mdss_vbif_setup(priv->vbif_base);

	// TOFIX implement resources manager

	/* Panel 0 */
	mdss_intf_setup(priv->dpu_base, priv->cat, INTF_1, PINGPONG_0);

	mdp_dsc_setup(priv->dpu_base, priv->cat, DSC_0, PINGPONG_0);
	// TOFIX use companion PP id from catalog
	mdp_dsc_setup(priv->dpu_base, priv->cat, DSC_1, PINGPONG_1);

	// TOFIX use timings instead of static height/width
	mdp_setup_stage_rec1(priv->dpu_base, priv->cat, CTL_0, LM_0, SSPP_DMA0, 2160, 2160, false);
	// TOFIX use companion LM id from catalog
	mdp_setup_stage_rec1(priv->dpu_base, priv->cat, CTL_0, LM_1, SSPP_DMA0, 2160, 2160, true);

	//left display
	mdss_sspp_config_rec1(priv->dpu_base, priv->cat, uc_plat->base,
			      (2160 * 2 * 4), SSPP_DMA0, 2160, 2160, 0);

	mdss_intf_fetch_start_config(priv->dpu_base, priv->cat,
				     CTL_0, INTF_1, DSC_0, DSC_1);

	// TOFIX fill a flush bitfield from the config commands
	mdp_ctl_flush(priv->dpu_base, priv->cat, CTL_0, SSPP_DMA0, true,
		      LM_0, LM_1);

	/* Panel 1 */
	mdss_intf_setup(priv->dpu_base, priv->cat, INTF_2, PINGPONG_2);

	mdp_dsc_setup(priv->dpu_base, priv->cat, DSC_2, PINGPONG_2);
	// TOFIX use companion PP id from catalog
	mdp_dsc_setup(priv->dpu_base, priv->cat, DSC_3, PINGPONG_3);

	// TOFIX use timings instead of static height/width
	mdp_setup_stage_rec1(priv->dpu_base, priv->cat, CTL_1, LM_2, SSPP_DMA1, 2160, 2160, false);
	// TOFIX use companion LM id from catalog
	mdp_setup_stage_rec1(priv->dpu_base, priv->cat, CTL_1, LM_3, SSPP_DMA1, 2160, 2160, true);

	//right display
	mdss_sspp_config_rec1(priv->dpu_base, priv->cat, uc_plat->base,
			(2160 * 2 * 4), SSPP_DMA1, 2160, 2160, 2160);

	mdss_intf_fetch_start_config(priv->dpu_base, priv->cat,
			CTL_1, INTF_2, DSC_2, DSC_3);

	// TOFIX fill a flush bitfield from the config commands
	mdp_ctl_flush(priv->dpu_base, priv->cat, CTL_1, SSPP_DMA1, true,
			LM_2, LM_3);

	// TOFIX should we use bonded dsi instead ?
	//writel(FLD_INTF_2_SW_TRG_MUX, priv->dpu_base + SPLIT_DISPLAY_LOWER_PIPE_CTRL);

	mdp_intf_enable(priv->dpu_base, priv->cat, INTF_1);
	mdp_intf_enable(priv->dpu_base, priv->cat, INTF_2);

#if 0
	local_priv = priv;
	dpu_kms_mdp_snapshot();
#endif
	return 0;
}

static int qcom_dpu_remove(struct udevice *dev)
{
	struct qcom_dpu_priv *priv = dev_get_priv(dev);

	if (!priv->bridge0 || !priv->bridge1)
		return 0;

	video_bridge_set_backlight(priv->bridge0, 0);
	video_bridge_set_backlight(priv->bridge1, 0);

	mdp_intf_enable(priv->dpu_base, priv->cat, INTF_1);
	mdp_intf_enable(priv->dpu_base, priv->cat, INTF_2);

	clk_disable_bulk(&priv->bulk);

	return 0;
}

#define LCD_MAX_WIDTH 4320
#define LCD_MAX_HEIGHT 2160
static int qcom_dpu_bind(struct udevice *dev)
{
	struct video_uc_plat *uc_plat = dev_get_uclass_plat(dev);

	uc_plat->size = (LCD_MAX_WIDTH * LCD_MAX_HEIGHT *
			((1 << VIDEO_BPP32) / 8));
	uc_plat->align = SZ_2M;
	dev_dbg(dev, "frame buffer max size %d bytes align %x\n",
		uc_plat->size, uc_plat->align);
	uc_plat->base = 0xaf400000; //0x9bb00000;

	return 0;
}

static const struct udevice_id dpu_ids[] = {
	{ .compatible = "qcom,sm8650-dpu" },
	{ }
};

U_BOOT_DRIVER(qcom_dpu) = {
	.name	= "qcom_dpu",
	.id	= UCLASS_VIDEO,
	.of_match = dpu_ids,
	.bind	= qcom_dpu_bind,
	.probe	= qcom_dpu_probe,
	.remove	= qcom_dpu_remove,
	.priv_auto	= sizeof(struct qcom_dpu_priv),
	.flags  = DM_FLAG_LEAVE_PD_ON,
};
