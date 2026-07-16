/* SPDX-License-Identifier: GPL-2.0-only */

/* this file handles configuration of VBIF/CTL/SSPP/INTF/LM/PP/DSC in DPU */

#define pr_fmt(fmt) "[DPU] "fmt
// #define LOG_DEBUG

#include <asm/io.h>
#include <clk.h>
#include <dm/device.h>
#include <dm/device_compat.h>
#include <dm/device-internal.h>
#include <dm/read.h>
#include <dm/ofnode_graph.h>
#include <video.h>
#include <video_bridge.h>
#include <linux/sizes.h>
#include <linux/delay.h>
#include <video_console.h>
#include <lmb.h>
#include <drm/display/drm_dsc_helper.h>
#include "panel.h"

#include "dpu_hw_mdss.h"
#include "dpu_hwio.h"
#include "catalog/dpu_hw_catalog.h"
#include "mdp_common.xml.h"
#include "mdp_format.h"
#include "qcom_mipi_dsi.h"
#include "dpu_hw.h"

static bool tried_probe = false;

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
#define LM_BLEND0_OP                     0x00

/* <v12 DPU with offset to mixer base + stage base */
#define LM_BLEND0_CONST_ALPHA            0x04
#define LM_FG_COLOR_FILL_COLOR_0         0x08
#define LM_FG_COLOR_FILL_COLOR_1         0x0C
#define LM_FG_COLOR_FILL_SIZE            0x10
#define LM_FG_COLOR_FILL_XY              0x14

/* >= v12 DPU */
#define LM_BG_SRC_SEL_V12                0x14
#define LM_BG_SRC_SEL_V12_RESET_VALUE    0x0000c0c0
#define LM_BORDER_COLOR_0_V12            0x1c
#define LM_BORDER_COLOR_1_V12            0x20

/* >= v12 DPU with offset to mixer base + stage base */
#define LM_BLEND0_FG_SRC_SEL_V12         0x04
#define LM_BLEND0_CONST_ALPHA_V12        0x08
#define LM_FG_COLOR_FILL_COLOR_0_V12     0x0c
#define LM_FG_COLOR_FILL_COLOR_1_V12     0x10
#define LM_FG_COLOR_FILL_SIZE_V12        0x14
#define LM_FG_COLOR_FILL_XY_V12          0x18

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

#define DSC_MODE_SPLIT_PANEL            BIT(0)
#define DSC_MODE_MULTIPLEX              BIT(1)
#define DSC_MODE_VIDEO                  BIT(2)

#define dpu_write(reg, val) writel(val, reg)

struct qcom_dpu_priv {
	void *dpu_base;
	void *vbif_base;
	const struct dpu_mdss_cfg *cat;
	struct clk_bulk bulk;
	struct udevice *bridge0;
	struct udevice *bridge1;
	struct dpu_topology topology;
	struct drm_dsc_config dsc;
	bool dsc_en;
	/*
	 * If we're just driving one panel with both controllers,
	 * false if two separate panels (or 1 panel 1 controller)
	 */
	bool dual_dsi;
	bool sync_dsi;
};

static inline void _DPU_REG_WRITE(struct dpu_hw_blk_reg_map *c, u32 off, u32 val)
{
	// debug("%s: %#llx: %#010x (%#llx + %#x)\n", __func__, (u64)base + off, val, (u64)base, off);
	writel(val, c->blk_addr + off);
}

#define DPU_REG_WRITE(c, off, val) { \
	/*debug("W: %#llx + %s (%#llx) <- %#x\n", (u64)(c)->blk_addr, #off, (u64)(c)->blk_addr+(off), (u32)val); */\
	_DPU_REG_WRITE(c, off, val); \
}

static inline u32 DPU_REG_READ(struct dpu_hw_blk_reg_map *c, u32 off)
{
	u32 val = readl(c->blk_addr + off);
	// debug("%s: %#llx: %#010x (%#llx + %#x)\n", __func__, (u64)base + off, val, (u64)base, off);
	return val;
}

static inline void dpu_hw_setup_rects_impl(struct dpu_sw_pipe *pipe, struct dpu_sw_pipe_cfg *cfg,
					   struct dpu_hw_sspp *ctx, u32 src_size_off,
					   u32 src_xy_off, u32 out_size_off, u32 out_xy_off)
{
	struct dpu_hw_blk_reg_map *c;
	u32 src_size, src_xy, dst_size, dst_xy;

	c = &ctx->hw;

	/* src and dest rect programming */
	src_xy = (cfg->src_rect.y1 << 16) | cfg->src_rect.x1;
	src_size = (drm_rect_height(&cfg->src_rect) << 16) |
		   drm_rect_width(&cfg->src_rect);
	dst_xy = (cfg->dst_rect.y1 << 16) | cfg->dst_rect.x1;
	dst_size = (drm_rect_height(&cfg->dst_rect) << 16) |
		drm_rect_width(&cfg->dst_rect);

	/* rectangle register programming */
	DPU_REG_WRITE(c, src_size_off, src_size);
	DPU_REG_WRITE(c, src_xy_off, src_xy);
	DPU_REG_WRITE(c, out_size_off, dst_size);
	DPU_REG_WRITE(c, out_xy_off, dst_xy);
}

static void dpu_hw_sspp_setup_rects(struct dpu_sw_pipe *pipe,
		struct dpu_sw_pipe_cfg *cfg)
{
	struct dpu_hw_sspp *ctx = pipe->sspp;
	u32 src_size_off, src_xy_off, out_size_off, out_xy_off;

	if (!ctx || !cfg)
		return;

	debug("SSPP_%s %s rect src=["DRM_RECT_FMT"], dst=["DRM_RECT_FMT"]\n", dpu_sspp_str(ctx->idx),
		 multirect_idx_str(pipe->multirect_index),
		 DRM_RECT_ARG(&cfg->src_rect), DRM_RECT_ARG(&cfg->dst_rect));

	if (pipe->multirect_index == DPU_SSPP_RECT_SOLO ||
	    pipe->multirect_index == DPU_SSPP_RECT_0) {
		src_size_off = SSPP_SRC_SIZE;
		src_xy_off = SSPP_SRC_XY;
		out_size_off = SSPP_OUT_SIZE;
		out_xy_off = SSPP_OUT_XY;
	} else {
		src_size_off = SSPP_SRC_SIZE_REC1;
		src_xy_off = SSPP_SRC_XY_REC1;
		out_size_off = SSPP_OUT_SIZE_REC1;
		out_xy_off = SSPP_OUT_XY_REC1;
	}

	dpu_hw_setup_rects_impl(pipe, cfg, ctx, src_size_off,
				src_xy_off, out_size_off, out_xy_off);


	/* We don't use chroma subsampling but we still need to program the pixel extension
	 * registers (????)
	 */
	DPU_REG_WRITE(&ctx->hw, SSPP_SW_PIX_EXT_C0_REQ_PIXELS, (drm_rect_height(&cfg->src_rect) << 16) | drm_rect_width(&cfg->src_rect));
	DPU_REG_WRITE(&ctx->hw, SSPP_SW_PIX_EXT_C0_LR, 0);
	DPU_REG_WRITE(&ctx->hw, SSPP_SW_PIX_EXT_C0_TB, 0);
	DPU_REG_WRITE(&ctx->hw, SSPP_SW_PIX_EXT_C1C2_LR, 0);
	DPU_REG_WRITE(&ctx->hw, SSPP_SW_PIX_EXT_C1C2_TB, 0);
	DPU_REG_WRITE(&ctx->hw, SSPP_SW_PIX_EXT_C1C2_REQ_PIXELS, (drm_rect_height(&cfg->src_rect) << 16) | drm_rect_width(&cfg->src_rect));
	DPU_REG_WRITE(&ctx->hw, SSPP_SW_PIX_EXT_C3_LR, 0);
	DPU_REG_WRITE(&ctx->hw, SSPP_SW_PIX_EXT_C3_TB, 0);
	DPU_REG_WRITE(&ctx->hw, SSPP_SW_PIX_EXT_C3_REQ_PIXELS, (drm_rect_height(&cfg->src_rect) << 16) | drm_rect_width(&cfg->src_rect));
}

void dpu_hw_setup_multirect_impl(struct dpu_sw_pipe *pipe,
				 struct dpu_hw_sspp *ctx, u32 op_mode_off)
{
	u32 mode_mask;

	if (pipe->multirect_index == DPU_SSPP_RECT_SOLO) {
		/**
		 * if rect index is RECT_SOLO, we cannot expect a
		 * virtual plane sharing the same SSPP id. So we go
		 * and disable multirect
		 */
		mode_mask = 0;
	} else {
		mode_mask = DPU_REG_READ(&ctx->hw, op_mode_off);
		mode_mask |= pipe->multirect_index;
		if (pipe->multirect_mode == DPU_SSPP_MULTIRECT_TIME_MX)
			mode_mask |= BIT(2);
		else
			mode_mask &= ~BIT(2);
	}

	DPU_REG_WRITE(&ctx->hw, op_mode_off, mode_mask);
}

static void dpu_hw_sspp_setup_multirect(struct dpu_sw_pipe *pipe)
{
	struct dpu_hw_sspp *ctx = pipe->sspp;

	if (!ctx)
		return;

	dpu_hw_setup_multirect_impl(pipe, ctx, SSPP_MULTIRECT_OPMODE);
}


static void dpu_hw_sspp_setup_sourceaddress(struct dpu_sw_pipe *pipe,
		struct dpu_hw_fmt_layout *layout)
{
	struct dpu_hw_sspp *ctx = pipe->sspp;
	u32 ystride0, ystride1;
	int i;

	if (!ctx)
		return;

	if (pipe->multirect_index == DPU_SSPP_RECT_SOLO) {
		for (i = 0; i < ARRAY_SIZE(layout->plane_addr); i++)
			DPU_REG_WRITE(&ctx->hw, SSPP_SRC0_ADDR + i * 0x4,
					layout->plane_addr[i]);
	} else if (pipe->multirect_index == DPU_SSPP_RECT_0) {
		DPU_REG_WRITE(&ctx->hw, SSPP_SRC0_ADDR,
				layout->plane_addr[0]);
		DPU_REG_WRITE(&ctx->hw, SSPP_SRC2_ADDR,
				layout->plane_addr[2]);
	} else {
		DPU_REG_WRITE(&ctx->hw, SSPP_SRC1_ADDR,
				layout->plane_addr[0]);
		DPU_REG_WRITE(&ctx->hw, SSPP_SRC3_ADDR,
				layout->plane_addr[2]);
	}

	if (pipe->multirect_index == DPU_SSPP_RECT_SOLO) {
		ystride0 = (layout->plane_pitch[0]) |
			(layout->plane_pitch[1] << 16);
		ystride1 = (layout->plane_pitch[2]) |
			(layout->plane_pitch[3] << 16);
	} else {
		ystride0 = DPU_REG_READ(&ctx->hw, SSPP_SRC_YSTRIDE0);
		ystride1 = DPU_REG_READ(&ctx->hw, SSPP_SRC_YSTRIDE1);

		if (pipe->multirect_index == DPU_SSPP_RECT_0) {
			ystride0 = (ystride0 & 0xFFFF0000) |
				(layout->plane_pitch[0] & 0x0000FFFF);
			ystride1 = (ystride1 & 0xFFFF0000)|
				(layout->plane_pitch[2] & 0x0000FFFF);
		} else {
			ystride0 = (ystride0 & 0x0000FFFF) |
				((layout->plane_pitch[0] << 16) &
				 0xFFFF0000);
			ystride1 = (ystride1 & 0x0000FFFF) |
				((layout->plane_pitch[2] << 16) &
				 0xFFFF0000);
		}
	}

	DPU_REG_WRITE(&ctx->hw, SSPP_SRC_YSTRIDE0, ystride0);
	DPU_REG_WRITE(&ctx->hw, SSPP_SRC_YSTRIDE1, ystride1);
}

static u32
dpu_encoder_dsc_initial_line_calc(struct drm_dsc_config *dsc,
				  u32 enc_ip_width)
{
	int ssm_delay, total_pixels, soft_slice_per_enc;

	soft_slice_per_enc = enc_ip_width / dsc->slice_width;

	/*
	 * minimum number of initial line pixels is a sum of:
	 * 1. sub-stream multiplexer delay (83 groups for 8bpc,
	 *    91 for 10 bpc) * 3
	 * 2. for two soft slice cases, add extra sub-stream multiplexer * 3
	 * 3. the initial xmit delay
	 * 4. total pipeline delay through the "lock step" of encoder (47)
	 * 5. 6 additional pixels as the output of the rate buffer is
	 *    48 bits wide
	 */
	ssm_delay = ((dsc->bits_per_component < 10) ? 84 : 92);
	total_pixels = ssm_delay * 3 + dsc->initial_xmit_delay + 47;
	if (soft_slice_per_enc > 1)
		total_pixels += (ssm_delay * 3);
	printf("%s: total_pixels=%d slice_width=%d\n", __func__, total_pixels, dsc->slice_width);
	return DIV_ROUND_UP(total_pixels, dsc->slice_width);
}

void dpu_hw_setup_format_impl(struct dpu_sw_pipe *pipe, const struct msm_format *fmt,
			      u32 flags, struct dpu_hw_sspp *ctx, u32 op_mode_off,
			      u32 unpack_pat_off, u32 format_off)
{
	struct dpu_hw_blk_reg_map *c;
	u32 chroma_samp, unpack, src_format;
	u32 opmode;
	// u32 fast_clear;

	c = &ctx->hw;
	opmode = DPU_REG_READ(c, op_mode_off);
	opmode &= ~(MDSS_MDP_OP_FLIP_LR | MDSS_MDP_OP_FLIP_UD |
			MDSS_MDP_OP_BWC_EN | MDSS_MDP_OP_PE_OVERRIDE);

	// if (flags & DPU_SSPP_FLIP_LR)
	// 	opmode |= MDSS_MDP_OP_FLIP_LR;
	// if (flags & DPU_SSPP_FLIP_UD)
	// 	opmode |= MDSS_MDP_OP_FLIP_UD;

	chroma_samp = fmt->chroma_sample;
	// if (flags & DPU_SSPP_SOURCE_ROTATED_90) {
	// 	if (chroma_samp == CHROMA_H2V1)
	// 		chroma_samp = CHROMA_H1V2;
	// 	else if (chroma_samp == CHROMA_H1V2)
	// 		chroma_samp = CHROMA_H2V1;
	// }

	src_format = (chroma_samp << 23) | (fmt->fetch_type << 19) |
		(fmt->bpc_a << 6) | (fmt->bpc_r_cr << 4) |
		(fmt->bpc_b_cb << 2) | (fmt->bpc_g_y << 0);

	// if (flags & DPU_SSPP_ROT_90)
	// 	src_format |= BIT(11); /* ROT90 */

	if (fmt->alpha_enable && fmt->fetch_type == MDP_PLANE_INTERLEAVED)
		src_format |= BIT(8); /* SRCC3_EN */

	// if (flags & DPU_SSPP_SOLID_FILL)
	// 	src_format |= BIT(22);

	unpack = (fmt->element[3] << 24) | (fmt->element[2] << 16) |
		(fmt->element[1] << 8) | (fmt->element[0] << 0);
	src_format |= ((fmt->unpack_count - 1) << 12) |
		((fmt->flags & MSM_FORMAT_FLAG_UNPACK_TIGHT ? 1 : 0) << 17) |
		((fmt->flags & MSM_FORMAT_FLAG_UNPACK_ALIGN_MSB ? 1 : 0) << 18) |
		((fmt->bpp - 1) << 9);

	// if (fmt->fetch_mode != MDP_FETCH_LINEAR) {
	// 	u32 hbb = ctx->ubwc->highest_bank_bit - 13;
	// 	u32 ctrl_val;

	// 	if (MSM_FORMAT_IS_UBWC(fmt))
	// 		opmode |= MDSS_MDP_OP_BWC_EN;
	// 	src_format |= (fmt->fetch_mode & 3) << 30; /*FRAME_FORMAT */

	// 	if (ctx->ubwc->ubwc_enc_version == UBWC_1_0) {
	// 		fast_clear = fmt->alpha_enable ? BIT(31) : 0;
	// 		ctrl_val = fast_clear | (ctx->ubwc->ubwc_swizzle & 0x1) |
	// 			BIT(8) | (hbb << 4);
	// 	} else if (ctx->ubwc->ubwc_enc_version == UBWC_2_0) {
	// 		fast_clear = fmt->alpha_enable ? BIT(31) : 0;
	// 		ctrl_val = fast_clear | ctx->ubwc->ubwc_swizzle | (hbb << 4);
	// 	} else if (ctx->ubwc->ubwc_enc_version == UBWC_3_0) {
	// 		ctrl_val = BIT(30) | (ctx->ubwc->ubwc_swizzle) | (hbb << 4);
	// 	} else if (ctx->ubwc->ubwc_enc_version == UBWC_4_0) {
	// 		ctrl_val = MSM_FORMAT_IS_YUV(fmt) ? 0 : BIT(30);
	// 		DPU_REG_WRITE(c, SSPP_UBWC_STATIC_CTRL_REC1,
	// 				MSM_FORMAT_IS_YUV(fmt) ? 0 : BIT(30));
	// 	} else if (ctx->ubwc->ubwc_enc_version <= UBWC_6_0) {
	// 		if (MSM_FORMAT_IS_YUV(fmt))
	// 			ctrl_val = 0;
	// 		else if (MSM_FORMAT_IS_DX(fmt)) /* or FP16, but it's unsupported */
	// 			ctrl_val = BIT(30);
	// 		else
	// 			ctrl_val = BIT(30) | BIT(31);
	// 		/* SDE also sets bits for lossy formats, but we don't support them yet */
	// 	} else {
	// 		DRM_WARN_ONCE("Unsupported UBWC version %x\n", ctx->ubwc->ubwc_enc_version);
	// 		ctrl_val = 0;
	// 	}

	// 	DPU_REG_WRITE(c, ubwc_ctrl_off, ctrl_val);
	// }

	opmode |= MDSS_MDP_OP_PE_OVERRIDE;

	/* if this is YUV pixel format, enable CSC */
	if (MSM_FORMAT_IS_YUV(fmt))
		src_format |= BIT(15);

	if (MSM_FORMAT_IS_DX(fmt))
		src_format |= BIT(14);

	/* update scaler opmode, if appropriate */
	// if (test_bit(DPU_SSPP_CSC, &ctx->cap->features))
	// 	dpu_hw_sspp_setup_opmode(ctx, VIG_OP_CSC_EN | VIG_OP_CSC_SRC_DATAFMT,
	// 		MSM_FORMAT_IS_YUV(fmt));
	// else if (test_bit(DPU_SSPP_CSC_10BIT, &ctx->cap->features))
	// 	dpu_hw_sspp_setup_csc10_opmode(ctx,
	// 		VIG_CSC_10_EN | VIG_CSC_10_SRC_DATAFMT,
	// 		MSM_FORMAT_IS_YUV(fmt));

	DPU_REG_WRITE(c, format_off, src_format);
	DPU_REG_WRITE(c, unpack_pat_off, unpack);
	DPU_REG_WRITE(c, op_mode_off, opmode);

	/* clear previous UBWC error */
	// DPU_REG_WRITE(c, ubwc_error_off, BIT(31));
}

static void dpu_hw_sspp_setup_format(struct dpu_sw_pipe *pipe,
		const struct msm_format *fmt)
{
	struct dpu_hw_sspp *ctx = pipe->sspp;
	u32 op_mode_off, unpack_pat_off, format_off, ubwc_ctrl_off, ubwc_error_off;

	if (!ctx || !fmt)
		return;

	if (pipe->multirect_index == DPU_SSPP_RECT_SOLO ||
	    pipe->multirect_index == DPU_SSPP_RECT_0) {
		op_mode_off = SSPP_SRC_OP_MODE;
		unpack_pat_off = SSPP_SRC_UNPACK_PATTERN;
		format_off = SSPP_SRC_FORMAT;
		ubwc_ctrl_off = SSPP_UBWC_STATIC_CTRL;
		ubwc_error_off = SSPP_UBWC_ERROR_STATUS;
	} else {
		op_mode_off = SSPP_SRC_OP_MODE_REC1;
		unpack_pat_off = SSPP_SRC_UNPACK_PATTERN_REC1;
		format_off = SSPP_SRC_FORMAT_REC1;

		/* reg wasn't present before DPU 8.0 */
		// if (ctx->mdss_ver->core_major_ver >= 8) {
		// 	ubwc_ctrl_off = SSPP_UBWC_STATIC_CTRL_REC1;
		// 	ubwc_error_off = SSPP_UBWC_ERROR_STATUS_REC1;
		// } else {
		// 	ubwc_ctrl_off = SSPP_UBWC_STATIC_CTRL;
		// 	ubwc_error_off = SSPP_UBWC_ERROR_STATUS;
		// }
	}

	// if (fmt->fetch_mode != MDP_FETCH_LINEAR) {
	// 	u32 hbb = ctx->ubwc->highest_bank_bit - 13;

	// 	DPU_REG_WRITE(&ctx->hw, SSPP_FETCH_CONFIG,
	// 		      DPU_FETCH_CONFIG_RESET_VALUE |
	// 		      hbb << 18);
	// }

	dpu_hw_setup_format_impl(pipe, fmt, 0, ctx, op_mode_off,
				 unpack_pat_off, format_off);
}

#define CDP_ENABLE		BIT(0)
#define CDP_UBWC_META_ENABLE	BIT(1)
#define CDP_TILE_AMORTIZE_ENABLE BIT(2)
#define CDP_PRELOAD_AHEAD_64	BIT(3)

static void dpu_hw_sspp_setup_cdp(struct dpu_sw_pipe *pipe,
				  const struct msm_format *fmt,
				  bool enable)
{
	struct dpu_hw_sspp *ctx = pipe->sspp;
	u32 cdp_cntl_offset = 0;

	if (!ctx)
		return;

	if (pipe->multirect_index == DPU_SSPP_RECT_SOLO ||
	    pipe->multirect_index == DPU_SSPP_RECT_0)
		cdp_cntl_offset = SSPP_CDP_CNTL;
	else
		cdp_cntl_offset = SSPP_CDP_CNTL_REC1;

	u32 cdp_cntl = CDP_PRELOAD_AHEAD_64;

	if (enable)
		cdp_cntl |= CDP_ENABLE;
	if (MSM_FORMAT_IS_UBWC(fmt))
		cdp_cntl |= CDP_UBWC_META_ENABLE;
	if (MSM_FORMAT_IS_UBWC(fmt) ||
	    MSM_FORMAT_IS_TILE(fmt))
		cdp_cntl |= CDP_TILE_AMORTIZE_ENABLE;

	DPU_REG_WRITE(&ctx->hw, cdp_cntl_offset, cdp_cntl);
}

static void mdss_vbif_setup(void *regs_vbif)
{
	writel(0x33333333, regs_vbif + VBIF_OUT_AXI_AMEMTYPE_CONF0);
   	writel(0x33333333, regs_vbif + VBIF_OUT_AXI_AMEMTYPE_CONF1);
}

/*
 * programmable_fetch_get_num_lines:
 *	Number of fetch lines in vertical front porch
 * @timing: Pointer to the intf timing information for the requested mode
 *
 * Returns the number of fetch lines in vertical front porch at which mdp
 * can start fetching the next frame.
 *
 * Number of needed prefetch lines is anything that cannot be absorbed in the
 * start of frame time (back porch + vsync pulse width).
 *
 * Some panels have very large VFP, however we only need a total number of
 * lines based on the chip worst case latencies.
 */
static u32 programmable_fetch_get_num_lines(
		const struct dpu_intf_cfg *hw_intf,
		const struct display_timing *timing)
{
	u32 worst_case_needed_lines =
	    hw_intf->prog_fetch_lines_worst_case;
	u32 start_of_frame_lines =
	    timing->vback_porch.typ + timing->vsync_len.typ;
	u32 needed_vfp_lines = worst_case_needed_lines - start_of_frame_lines;
	u32 actual_vfp_lines = 0;

	/* Fetch must be outside active lines, otherwise undefined. */
	if (start_of_frame_lines >= worst_case_needed_lines) {
		debug("prog fetch is not needed, large vbp+vsw\n");
		actual_vfp_lines = 0;
	} else if (timing->vfront_porch.typ < needed_vfp_lines) {
		/* Warn fetch needed, but not enough porch in panel config */
		debug("low vbp+vfp may lead to perf issues in some cases\n");
		debug("less vfp than fetch req, using entire vfp\n");
		actual_vfp_lines = timing->vfront_porch.typ;
	} else {
		debug("room in vfp for needed prefetch\n");
		actual_vfp_lines = needed_vfp_lines;
	}

	// debug("vfront_porch.typ %u v_back_porch %u vsync_pulse_width %u\n",
	// 	timing->vfront_porch.typ, timing->vback_porch.typ,
	// 	timing->vsync_len.typ);
	// debug("wc_lines %u needed_vfp_lines %u actual_vfp_lines %u\n",
	// 	worst_case_needed_lines, needed_vfp_lines, actual_vfp_lines);

	return actual_vfp_lines;
}

static void display_timing_to_drm_mode(const struct display_timing *timing,
				       struct drm_display_mode *mode)
{
	mode->clock = timing->pixelclock.typ / 1000;
	mode->hdisplay = timing->hactive.typ;
	mode->hsync_start = timing->hactive.typ + timing->hfront_porch.typ;
	mode->hsync_end = mode->hsync_start + timing->hsync_len.typ;
	mode->htotal = mode->hsync_end + timing->hback_porch.typ;

	mode->vdisplay = timing->vactive.typ;
	mode->vsync_start = timing->vactive.typ + timing->vfront_porch.typ;
	mode->vsync_end = mode->vsync_start + timing->vsync_len.typ;
	mode->vtotal = mode->vsync_end + timing->vback_porch.typ;

	mode->flags = timing->flags;
	// timing->pixelclock.typ = mode->clock * 1000; /* kHz to Hz */
	// timing->hactive.typ = mode->hdisplay;
	// timing->hfront_porch.typ = mode->hsync_start - mode->hdisplay;
	// timing->hsync_len.typ = mode->hsync_end - mode->hsync_start;
	// timing->hback_porch.typ = mode->htotal - mode->hsync_end;
	// timing->vactive.typ = mode->vdisplay;
	// timing->vfront_porch.typ = mode->vsync_start - mode->vdisplay;
	// timing->vsync_len.typ = mode->vsync_end - mode->vsync_start;
	// timing->vback_porch.typ = mode->vtotal - mode->vsync_end;

	// /* DRM_MODE_FLAG_* defines are already mapped to u-boot DISPLAY_FLAGS */
	// timing->flags = mode->flags;
}

/**
 * msm_dsc_get_bytes_per_line() - calculate bytes per line
 * @dsc: Pointer to drm dsc config struct
 * Returns: Integer value representing bytes per line. DSI and DP need
 *          to perform further calculations to turn this into pclk_per_intf,
 *          such as dividing by different values depending on if widebus is enabled.
 */
static inline u32 msm_dsc_get_bytes_per_line(const struct drm_dsc_config *dsc)
{
	return dsc->slice_count * dsc->slice_chunk_size;
}

static void dpu_hw_intf_setup_timing_engine(struct dpu_hw_intf *intf,
		const struct display_timing *p,
		const struct msm_format *fmt,
		const struct drm_dsc_config *dsc, bool dsc_en)
{
	struct dpu_hw_blk_reg_map *c = &intf->hw;
	u32 hsync_period, vsync_period;
	u32 display_v_start, display_v_end;
	u32 hsync_start_x, hsync_end_x;
	u32 hsync_data_start_x, hsync_data_end_x;
	u32 active_h_start, active_h_end;
	u32 active_v_start, active_v_end;
	u32 active_hctl, display_hctl, hsync_ctl;
	u32 polarity_ctl, den_polarity;
	u32 panel_format;
	u32 intf_cfg, intf_cfg2 = 0;
	u32 display_data_hctl = 0, active_data_hctl = 0;
	u32 data_width, width;
	bool dp_intf = false;
	bool wide_bus_en = dsc_en;

	/* read interface_cfg */
	intf_cfg = DPU_REG_READ(c, INTF_CONFIG);

	if (intf->cap->type == INTF_DP)
		dp_intf = true;

	width = p->hactive.typ;

	if (!dp_intf && dsc_en) {
		width = width * drm_dsc_get_bpp_int(dsc) /
				(dsc->bits_per_component * 3);
	}

	hsync_period = p->hsync_len.typ + p->hback_porch.typ + width +
		p->hfront_porch.typ;
	vsync_period = p->vsync_len.typ + p->vback_porch.typ + p->vactive.typ +
		p->vfront_porch.typ;

	display_v_start = ((p->vsync_len.typ + p->vback_porch.typ) *
	hsync_period) + 0; //p->hsync_skew;
	display_v_end = ((vsync_period - p->vfront_porch.typ) * hsync_period) +
	0; //p->hsync_skew - 1;

	// FIXME! match linux on pocket s2, where does this 1 come from?
	display_v_end -= 1;

	hsync_start_x = p->hback_porch.typ + p->hsync_len.typ;
	hsync_end_x = hsync_period - p->hfront_porch.typ - 1;

	if (false /*width != p->xres*/) { /* border fill added */
		// active_h_start = hsync_start_x;
		// active_h_end = active_h_start + p->xres - 1;
	} else {
		active_h_start = 0;
		active_h_end = 0;
	}

	if (false /*p->vactive.typ != p->yres*/) { /* border fill added */
		// active_v_start = display_v_start;
		// active_v_end = active_v_start + (p->yres * hsync_period) - 1;
	} else {
		active_v_start = 0;
		active_v_end = 0;
	}

	if (active_h_end) {
		active_hctl = (active_h_end << 16) | active_h_start;
		intf_cfg |= INTF_CFG_ACTIVE_H_EN;
	} else {
		active_hctl = 0;
	}

	if (active_v_end)
		intf_cfg |= INTF_CFG_ACTIVE_V_EN;

	hsync_ctl = (hsync_period << 16) | p->hsync_len.typ;
	display_hctl = (hsync_end_x << 16) | hsync_start_x;

	if (wide_bus_en)
		intf_cfg2 |= INTF_CFG2_DATABUS_WIDEN;

	data_width = width;

	/*
	 * If widebus is disabled:
	 * For uncompressed stream, the data is valid for the entire active
	 * window period.
	 * For compressed stream, data is valid for a shorter time period
	 * inside the active window depending on the compression ratio.
	 *
	 * If widebus is enabled:
	 * For uncompressed stream, data is valid for only half the active
	 * window, since the data rate is doubled in this mode.
	 * For compressed stream, data validity window needs to be adjusted for
	 * compression ratio and then further halved.
	 *
	 * For the compression mode in DP case, the p->width is already
	 * adjusted in drm_mode_to_intf_timing_params().
	 */
	if (dsc_en && !dp_intf) {
		if (wide_bus_en)
			data_width = DIV_ROUND_UP(msm_dsc_get_bytes_per_line(dsc), 6);
		else
			data_width = DIV_ROUND_UP(msm_dsc_get_bytes_per_line(dsc), 3);
	} else if (wide_bus_en && !dp_intf) {
		data_width = width >> 1;
	}

	/* TODO: handle DSC+DP case, we only handle DSC+DSI case so far */
	if (dsc_en && !dp_intf &&
	    intf->mdss_ver->core_major_ver >= 7)
		intf_cfg2 |= INTF_CFG2_DCE_DATA_COMPRESS;

	hsync_data_start_x = hsync_start_x;
	hsync_data_end_x =  hsync_start_x + data_width - 1;

	// printf("FIXME: hack hsync_data_end_x!!!\n");
	// hsync_data_end_x = 399;

	display_data_hctl = (hsync_data_end_x << 16) | hsync_data_start_x;

	if (dp_intf) {
		/* DP timing adjustment */
		display_v_start += p->hsync_len.typ + p->hback_porch.typ;
		display_v_end   -= p->hfront_porch.typ;

		active_h_start = hsync_start_x;
		active_h_end = active_h_start + width - 1;
		active_v_start = display_v_start;
		active_v_end = active_v_start + (p->vactive.typ * hsync_period) - 1;

		active_hctl = (active_h_end << 16) | active_h_start;
		display_hctl = active_hctl;

		intf_cfg |= INTF_CFG_ACTIVE_H_EN | INTF_CFG_ACTIVE_V_EN;
	}

	den_polarity = 0;
	polarity_ctl = (den_polarity << 2) | /*  DEN Polarity  */
		(0 << 1) | /* VSYNC Polarity */
		(0 << 0);  /* HSYNC Polarity */

	if (!MSM_FORMAT_IS_YUV(fmt))
		panel_format = (fmt->bpc_g_y |
				(fmt->bpc_b_cb << 2) |
				(fmt->bpc_r_cr << 4) |
				(0x21 << 8));
	else
		/* Interface treats all the pixel data in RGB888 format */
		panel_format = (BPC8 |
				(BPC8 << 2) |
				(BPC8 << 4) |
				(0x21 << 8));

	DPU_REG_WRITE(c, INTF_HSYNC_CTL, hsync_ctl);
	DPU_REG_WRITE(c, INTF_VSYNC_PERIOD_F0, vsync_period * hsync_period);
	DPU_REG_WRITE(c, INTF_VSYNC_PULSE_WIDTH_F0,
			p->vsync_len.typ * hsync_period);
	DPU_REG_WRITE(c, INTF_DISPLAY_HCTL, display_hctl);
	DPU_REG_WRITE(c, INTF_DISPLAY_V_START_F0, display_v_start);
	DPU_REG_WRITE(c, INTF_DISPLAY_V_END_F0, display_v_end);
	DPU_REG_WRITE(c, INTF_ACTIVE_HCTL,  active_hctl);
	DPU_REG_WRITE(c, INTF_ACTIVE_V_START_F0, active_v_start);
	DPU_REG_WRITE(c, INTF_ACTIVE_V_END_F0, active_v_end);
	DPU_REG_WRITE(c, INTF_BORDER_COLOR, 0);
	DPU_REG_WRITE(c, INTF_UNDERFLOW_COLOR, 0xff);
	DPU_REG_WRITE(c, INTF_HSYNC_SKEW, 0);
	DPU_REG_WRITE(c, INTF_POLARITY_CTL, polarity_ctl);
	DPU_REG_WRITE(c, INTF_FRAME_LINE_COUNT_EN, 0x3);
	DPU_REG_WRITE(c, INTF_CONFIG, intf_cfg);
	DPU_REG_WRITE(c, INTF_PANEL_FORMAT, panel_format);
	if (intf->mdss_ver->core_major_ver >= 5) {
		/*
		 * DATA_HCTL_EN controls data timing which can be different from
		 * video timing. It is recommended to enable it for all cases, except
		 * if compression is enabled in 1 pixel per clock mode
		 */
		if (!(dsc_en && !wide_bus_en))
			intf_cfg2 |= INTF_CFG2_DATA_HCTL_EN;

		DPU_REG_WRITE(c, INTF_CONFIG2, intf_cfg2);
		DPU_REG_WRITE(c, INTF_DISPLAY_DATA_HCTL, display_data_hctl);
		DPU_REG_WRITE(c, INTF_ACTIVE_DATA_HCTL, active_data_hctl);
	}
}

static void dpu_hw_intf_setup_prg_fetch(struct dpu_hw_intf *intf, const struct display_timing *p)
{
	u32 hactive = p->hactive.typ; /* Already divided by 2 in dual DSI case */
	u32 vfp_fetch_lines = programmable_fetch_get_num_lines(intf->cap, p);
	u32 horiz_total = 0;
	u32 vert_total = 0;
	u32 vfp_fetch_start_vsync_counter;
	u32 intf_cfg;

/*
	hactive = hactive * drm_dsc_get_bpp_int(dsc) /
				(dsc->bits_per_component * 3);
*/
	intf_cfg = DPU_REG_READ(&intf->hw, INTF_CONFIG);

	if (vfp_fetch_lines) {
		vert_total = p->vactive.typ + p->vback_porch.typ + p->vfront_porch.typ + p->vsync_len.typ;
		horiz_total = hactive + p->hback_porch.typ + p->hfront_porch.typ + p->hsync_len.typ;
		vfp_fetch_start_vsync_counter =
		    (vert_total - vfp_fetch_lines) * horiz_total + 1;

		debug("intf fetch_start: %u\n", vfp_fetch_start_vsync_counter);
		printf("FIXME: hack intf_prog_fetch_start\n");
		vfp_fetch_start_vsync_counter = 2024289; // 7657233 ???
		DPU_REG_WRITE(&intf->hw, INTF_PROG_FETCH_START, vfp_fetch_start_vsync_counter);

		intf_cfg |= BIT(31);
	} else {
		intf_cfg &= ~BIT(31);
	}

	DPU_REG_WRITE(&intf->hw, INTF_CONFIG, intf_cfg);
}

static void dpu_hw_intf_bind_pingpong_blk(
		struct dpu_hw_intf *intf,
		const enum dpu_pingpong pp)
{
	struct dpu_hw_blk_reg_map *c = &intf->hw;
	u32 mux_cfg;

	mux_cfg = DPU_REG_READ(c, INTF_MUX);
	mux_cfg &= ~0xf;

	if (pp)
		mux_cfg |= (pp - PINGPONG_0) & 0x7;
	else
		mux_cfg |= 0xf;

	DPU_REG_WRITE(c, INTF_MUX, mux_cfg);
}

static void dpu_hw_intf_enable_timing_engine(
		struct dpu_hw_intf *intf,
		u8 enable)
{
	struct dpu_hw_blk_reg_map *c = &intf->hw;
	/* Note: Display interface select is handled in top block hw layer */
	DPU_REG_WRITE(c, INTF_TIMING_ENGINE_EN, enable != 0);
}

static void dpu_hw_lm_setup_out(struct dpu_hw_mixer *ctx,
		struct dpu_hw_mixer_cfg *mixer)
{
	struct dpu_hw_blk_reg_map *c = &ctx->hw;
	u32 outsize;
	u32 op_mode;

	debug("Program %s mixer %s (%ux%u)\n", mixer->right_mixer ? "right" : "left",
		 ctx->cap->name, mixer->out_width, mixer->out_height);

	op_mode = DPU_REG_READ(c, LM_OP_MODE);

	outsize = mixer->out_height << 16 | mixer->out_width;
	DPU_REG_WRITE(c, LM_OUT_SIZE, outsize);

	/* SPLIT_LEFT_RIGHT */
	if (mixer->right_mixer)
		op_mode |= BIT(31);
	else
		op_mode &= ~BIT(31);
	DPU_REG_WRITE(c, LM_OP_MODE, op_mode);
}

static void dpu_hw_lm_setup_color3(struct dpu_hw_mixer *ctx,
	uint32_t mixer_op_mode)
{
	struct dpu_hw_blk_reg_map *c = &ctx->hw;
	int op_mode;

	/* read the existing op_mode configuration */
	op_mode = DPU_REG_READ(c, LM_OP_MODE);

	op_mode = (op_mode & (BIT(31) | BIT(30))) | mixer_op_mode;

	DPU_REG_WRITE(c, LM_OP_MODE, op_mode);
}

/* configure the mixer blending in ctl */
void mdp_setup_stage_rec1(struct qcom_dpu_priv *priv, const struct dpu_mdss_cfg *cat,
		     unsigned ctl_id, unsigned lm_id, unsigned sspp_id,
		     unsigned height, unsigned width, bool split)
{
	void *dpu_base = priv->dpu_base;
	unsigned ctl = ctl_id - CTL_0;
	unsigned lm = lm_id - LM_0;
	void *base = dpu_base + cat->ctl[ctl].base;
	void *lm_base = dpu_base + cat->mixer[lm].base;
	u32 op_mode = 0x2; // BLEND0_COLOR3_OUT
	u32 val;

	if (priv->dual_dsi)
		width /= 2;

	// FIXME: need to refactor all this and configure both SSPPs in one call!
		// This is to handle two SSPPs on two LMs
	val = BIT(24) | readl(base + CTL_LAYER(lm_id));

	switch (sspp_id) {
	case SSPP_VIG0:
		val |= 2;
		break;
	case SSPP_VIG1:
		val |= 2 << 3;
		break;
	default:
		WARN_ON(sspp_id);
	};

	writel(val, base + CTL_LAYER(lm_id));

	writel(0, base + CTL_LAYER_EXT(lm_id));
	writel(0, base + CTL_LAYER_EXT2(lm_id));
	writel(0, base + CTL_LAYER_EXT3(lm_id));
	writel(0, base + CTL_LAYER_EXT4(lm_id));

	writel((height << 16) | width, lm_base + LM_OUT_SIZE);

	if (split)
		op_mode |= BIT(31); /* split right half in stage */
	writel(op_mode, lm_base + LM_OP_MODE);

	writel(0x100, lm_base + LM_BLEND0_OP); // BG_ALPHA_SEL: 1
	// writel(0x400, lm_base + LM_BLEND0_OP); // BG_INV_ALPHA_SEL
	// see dpu_hw_lm_setup_blend_config_combined_alpha()
	writel(0xFF0000, lm_base + LM_BLEND0_CONST_ALPHA);
}

static int _dsc_calc_output_buf_max_addr(const struct dpu_dsc_cfg *dsc_cfg, int num_softslice)
{
	int max_addr = 2400 / num_softslice;

	if (dsc_cfg->features & BIT(DPU_DSC_NATIVE_42x_EN))
		max_addr /= 2;

	return max_addr - 1;
};

/* Open codes all the DSC_1.2 ops called from dpu_encoder_prep_dsc() -> dpu_encoder_dsc_pipe_cfg() */
void dpu_encoder_dsc_pipe_cfg(struct dpu_hw_dsc *hw_dsc, struct dpu_hw_pingpong *hw_pp,
			      struct drm_dsc_config *dsc, u32 mode, u32 initial_lines)
{
	// const struct dpu_mdss_cfg *cat = priv->cat;
	// void *dpu_base = priv->dpu_base;
	// struct drm_dsc_config *dsc = &priv->dsc;
	// dsc_id = dsc_id - DSC_0;
	unsigned pp = hw_pp->idx - PINGPONG_0;
	// void *base = dpu_base + cat->dsc[dsc_id].base;
	// const struct dpu_dsc_sub_blks *sblk = cat->dsc[dsc_id].sblk;

	u32 data = 0;
	u32 det_thresh_flatness;
	u32 num_active_slice_per_enc;
	u32 bpp;
	struct drm_dsc_rc_range_parameters *rc;
	int mux_cfg = 0xf; /* Disabled */

	/* dpu_hw_dsc_config_1_2() */
	if (mode & DSC_MODE_SPLIT_PANEL)
		data |= BIT(0);

	if (mode & DSC_MODE_MULTIPLEX)
		data |= BIT(1);

	num_active_slice_per_enc = dsc->slice_count;
	if (mode & DSC_MODE_MULTIPLEX)
		num_active_slice_per_enc = dsc->slice_count / 2;

	data |= (num_active_slice_per_enc & 0x3) << 7;

	DPU_REG_WRITE(&hw_dsc->hw, DSC_CMN_MAIN_CNF, data);

	data = (initial_lines & 0xff);

	if (mode & DSC_MODE_VIDEO)
		data |= BIT(9);

	data |= (_dsc_calc_output_buf_max_addr(hw_dsc->cap, num_active_slice_per_enc) << 18);

	DPU_REG_WRITE(&hw_dsc->hw, hw_dsc->cap->sblk->enc.base + ENC_DF_CTRL, data);

	data = (dsc->dsc_version_minor & 0xf) << 28;
	if (dsc->dsc_version_minor == 0x2) {
		if (dsc->native_422)
			data |= BIT(22);
		if (dsc->native_420)
			data |= BIT(21);
	}

	bpp = dsc->bits_per_pixel;
	/* as per hw requirement bpp should be programmed
	 * twice the actual value in case of 420 or 422 encoding
	 */
	if (dsc->native_422 || dsc->native_420)
		bpp = 2 * bpp;

	data |= bpp << 10;

	if (dsc->block_pred_enable)
		data |= BIT(20);

	if (dsc->convert_rgb)
		data |= BIT(4);

	data |= (dsc->line_buf_depth & 0xf) << 6;
	data |= dsc->bits_per_component & 0xf;

	DPU_REG_WRITE(&hw_dsc->hw, hw_dsc->cap->sblk->enc.base + DSC_MAIN_CONF, data);

	data = (dsc->pic_width & 0xffff) |
		((dsc->pic_height & 0xffff) << 16);

	DPU_REG_WRITE(&hw_dsc->hw, hw_dsc->cap->sblk->enc.base + DSC_PICTURE_SIZE, data);

	data = (dsc->slice_width & 0xffff) |
		((dsc->slice_height & 0xffff) << 16);

	DPU_REG_WRITE(&hw_dsc->hw, hw_dsc->cap->sblk->enc.base + DSC_SLICE_SIZE, data);

	DPU_REG_WRITE(&hw_dsc->hw, hw_dsc->cap->sblk->enc.base + DSC_MISC_SIZE,
		      (dsc->slice_chunk_size) & 0xffff);

	data = (dsc->initial_xmit_delay & 0xffff) |
		((dsc->initial_dec_delay & 0xffff) << 16);

	DPU_REG_WRITE(&hw_dsc->hw, hw_dsc->cap->sblk->enc.base + DSC_HRD_DELAYS, data);

	DPU_REG_WRITE(&hw_dsc->hw, hw_dsc->cap->sblk->enc.base + DSC_RC_SCALE,
		      dsc->initial_scale_value & 0x3f);

	data = (dsc->scale_increment_interval & 0xffff) |
		((dsc->scale_decrement_interval & 0x7ff) << 16);

	DPU_REG_WRITE(&hw_dsc->hw, hw_dsc->cap->sblk->enc.base + DSC_RC_SCALE_INC_DEC, data);

	data = (dsc->first_line_bpg_offset & 0x1f) |
		((dsc->second_line_bpg_offset & 0x1f) << 5);

	DPU_REG_WRITE(&hw_dsc->hw, hw_dsc->cap->sblk->enc.base + DSC_RC_OFFSETS_1, data);

	data = (dsc->nfl_bpg_offset & 0xffff) |
		((dsc->slice_bpg_offset & 0xffff) << 16);

	DPU_REG_WRITE(&hw_dsc->hw, hw_dsc->cap->sblk->enc.base + DSC_RC_OFFSETS_2, data);

	data = (dsc->initial_offset & 0xffff) |
		((dsc->final_offset & 0xffff) << 16);

	DPU_REG_WRITE(&hw_dsc->hw, hw_dsc->cap->sblk->enc.base + DSC_RC_OFFSETS_3, data);

	data = (dsc->nsl_bpg_offset & 0xffff) |
		((dsc->second_line_offset_adj & 0xffff) << 16);

	DPU_REG_WRITE(&hw_dsc->hw, hw_dsc->cap->sblk->enc.base + DSC_RC_OFFSETS_4, data);

	det_thresh_flatness = drm_dsc_flatness_det_thresh(dsc);
	data = (dsc->flatness_min_qp & 0x1f) |
		((dsc->flatness_max_qp & 0x1f) << 5) |
		((det_thresh_flatness & 0xff) << 10);

	DPU_REG_WRITE(&hw_dsc->hw, hw_dsc->cap->sblk->enc.base + DSC_FLATNESS_QP, data);

	DPU_REG_WRITE(&hw_dsc->hw, hw_dsc->cap->sblk->enc.base + DSC_RC_MODEL_SIZE,
		      (dsc->rc_model_size) & 0xffff);

	data = dsc->rc_edge_factor & 0xf;
	data |= (dsc->rc_quant_incr_limit0 & 0x1f) << 8;
	data |= (dsc->rc_quant_incr_limit1 & 0x1f) << 13;
	data |= (dsc->rc_tgt_offset_high & 0xf) << 20;
	data |= (dsc->rc_tgt_offset_low & 0xf) << 24;

	DPU_REG_WRITE(&hw_dsc->hw, hw_dsc->cap->sblk->enc.base + DSC_RC_CONFIG, data);

	/* program the dsc wrapper */
	data = BIT(0); /* encoder enable */
	if (dsc->native_422)
		data |= BIT(8);
	else if (dsc->native_420)
		data |= BIT(9);
	if (!dsc->convert_rgb)
		data |= BIT(10);
	if (dsc->bits_per_component == 8)
		data |= BIT(11);
	if (mode & DSC_MODE_SPLIT_PANEL)
		data |= BIT(12);
	if (mode & DSC_MODE_MULTIPLEX)
		data |= BIT(13);
	if (!(mode & DSC_MODE_VIDEO))
		data |= BIT(17);

	DPU_REG_WRITE(&hw_dsc->hw, hw_dsc->cap->sblk->ctl.base + DSC_CFG, data);

	/* dpu_hw_dsc_config_thresh_1_2() */
	rc = dsc->rc_range_params;

	/*
	 * With BUF_THRESH -- 14 in total
	 * each register contains 4 thresh values with the last register
	 * containing only 2 thresh values
	 */
	DPU_REG_WRITE(&hw_dsc->hw, hw_dsc->cap->sblk->enc.base + DSC_RC_BUF_THRESH_0,
		      (dsc->rc_buf_thresh[0] << 0) |
		      (dsc->rc_buf_thresh[1] << 8) |
		      (dsc->rc_buf_thresh[2] << 16) |
		      (dsc->rc_buf_thresh[3] << 24));
	DPU_REG_WRITE(&hw_dsc->hw, hw_dsc->cap->sblk->enc.base + DSC_RC_BUF_THRESH_1,
		      (dsc->rc_buf_thresh[4] << 0) |
		      (dsc->rc_buf_thresh[5] << 8) |
		      (dsc->rc_buf_thresh[6] << 16) |
		      (dsc->rc_buf_thresh[7] << 24));
	DPU_REG_WRITE(&hw_dsc->hw, hw_dsc->cap->sblk->enc.base + DSC_RC_BUF_THRESH_2,
		      (dsc->rc_buf_thresh[8] << 0) |
		      (dsc->rc_buf_thresh[9] << 8) |
		      (dsc->rc_buf_thresh[10] << 16) |
		      (dsc->rc_buf_thresh[11] << 24));
	DPU_REG_WRITE(&hw_dsc->hw, hw_dsc->cap->sblk->enc.base + DSC_RC_BUF_THRESH_3,
		      (dsc->rc_buf_thresh[12] << 0) |
		      (dsc->rc_buf_thresh[13] << 8));

	/*
	 * with min/max_QP -- 5 bits
	 * each register contains 5 min_qp or max_qp for total of 15
	 *
	 * With BPG_OFFSET -- 6 bits
	 * each register contains 5 BPG_offset for total of 15
	 */
	DPU_REG_WRITE(&hw_dsc->hw, hw_dsc->cap->sblk->enc.base + DSC_RC_MIN_QP_0,
		      (rc[0].range_min_qp << 0) |
		      (rc[1].range_min_qp << 5) |
		      (rc[2].range_min_qp << 10) |
		      (rc[3].range_min_qp << 15) |
		      (rc[4].range_min_qp << 20));
	DPU_REG_WRITE(&hw_dsc->hw, hw_dsc->cap->sblk->enc.base + DSC_RC_MAX_QP_0,
		      (rc[0].range_max_qp << 0) |
		      (rc[1].range_max_qp << 5) |
		      (rc[2].range_max_qp << 10) |
		      (rc[3].range_max_qp << 15) |
		      (rc[4].range_max_qp << 20));
	DPU_REG_WRITE(&hw_dsc->hw, hw_dsc->cap->sblk->enc.base + DSC_RC_RANGE_BPG_OFFSETS_0,
		      (rc[0].range_bpg_offset << 0) |
		      (rc[1].range_bpg_offset << 6) |
		      (rc[2].range_bpg_offset << 12) |
		      (rc[3].range_bpg_offset << 18) |
		      (rc[4].range_bpg_offset << 24));

	DPU_REG_WRITE(&hw_dsc->hw, hw_dsc->cap->sblk->enc.base + DSC_RC_MIN_QP_1,
		      (rc[5].range_min_qp << 0) |
		      (rc[6].range_min_qp << 5) |
		      (rc[7].range_min_qp << 10) |
		      (rc[8].range_min_qp << 15) |
		      (rc[9].range_min_qp << 20));
	DPU_REG_WRITE(&hw_dsc->hw, hw_dsc->cap->sblk->enc.base + DSC_RC_MAX_QP_1,
		      (rc[5].range_max_qp << 0) |
		      (rc[6].range_max_qp << 5) |
		      (rc[7].range_max_qp << 10) |
		      (rc[8].range_max_qp << 15) |
		      (rc[9].range_max_qp << 20));
	DPU_REG_WRITE(&hw_dsc->hw, hw_dsc->cap->sblk->enc.base + DSC_RC_RANGE_BPG_OFFSETS_1,
		      (rc[5].range_bpg_offset << 0) |
		      (rc[6].range_bpg_offset << 6) |
		      (rc[7].range_bpg_offset << 12) |
		      (rc[8].range_bpg_offset << 18) |
		      (rc[9].range_bpg_offset << 24));

	DPU_REG_WRITE(&hw_dsc->hw, hw_dsc->cap->sblk->enc.base + DSC_RC_MIN_QP_2,
		      (rc[10].range_min_qp << 0) |
		      (rc[11].range_min_qp << 5) |
		      (rc[12].range_min_qp << 10) |
		      (rc[13].range_min_qp << 15) |
		      (rc[14].range_min_qp << 20));
	DPU_REG_WRITE(&hw_dsc->hw, hw_dsc->cap->sblk->enc.base + DSC_RC_MAX_QP_2,
		      (rc[10].range_max_qp << 0) |
		      (rc[11].range_max_qp << 5) |
		      (rc[12].range_max_qp << 10) |
		      (rc[13].range_max_qp << 15) |
		      (rc[14].range_max_qp << 20));
	DPU_REG_WRITE(&hw_dsc->hw, hw_dsc->cap->sblk->enc.base + DSC_RC_RANGE_BPG_OFFSETS_2,
		      (rc[10].range_bpg_offset << 0) |
		      (rc[11].range_bpg_offset << 6) |
		      (rc[12].range_bpg_offset << 12) |
		      (rc[13].range_bpg_offset << 18) |
		      (rc[14].range_bpg_offset << 24));


	/* dpu_hw_dsc_bind_pingpong_blk_1_2() */
	mux_cfg = pp & 0x7;

	DPU_REG_WRITE(&hw_dsc->hw, hw_dsc->cap->sblk->ctl.base + DSC_CTL, mux_cfg);
}

static void dpu_hw_ctl_update_pending_flush_sspp(struct dpu_hw_ctl *ctx,
	enum dpu_sspp sspp)
{
	debug("%s: pending flush: SSPP_%s\n", ctx->cap->name, dpu_sspp_str(sspp));
	switch (sspp) {
	case SSPP_VIG0:
		ctx->pending_flush_mask |=  BIT(0);
		break;
	case SSPP_VIG1:
		ctx->pending_flush_mask |= BIT(1);
		break;
	case SSPP_VIG2:
		ctx->pending_flush_mask |= BIT(2);
		break;
	case SSPP_VIG3:
		ctx->pending_flush_mask |= BIT(18);
		break;
	case SSPP_RGB0:
		ctx->pending_flush_mask |= BIT(3);
		break;
	case SSPP_RGB1:
		ctx->pending_flush_mask |= BIT(4);
		break;
	case SSPP_RGB2:
		ctx->pending_flush_mask |= BIT(5);
		break;
	case SSPP_RGB3:
		ctx->pending_flush_mask |= BIT(19);
		break;
	case SSPP_DMA0:
		ctx->pending_flush_mask |= BIT(11);
		break;
	case SSPP_DMA1:
		ctx->pending_flush_mask |= BIT(12);
		break;
	case SSPP_DMA2:
		ctx->pending_flush_mask |= BIT(24);
		break;
	case SSPP_DMA3:
		ctx->pending_flush_mask |= BIT(25);
		break;
	case SSPP_DMA4:
		ctx->pending_flush_mask |= BIT(13);
		break;
	case SSPP_DMA5:
		ctx->pending_flush_mask |= BIT(14);
		break;
	case SSPP_CURSOR0:
		ctx->pending_flush_mask |= BIT(22);
		break;
	case SSPP_CURSOR1:
		ctx->pending_flush_mask |= BIT(23);
		break;
	default:
		break;
	}
}

static void dpu_hw_ctl_update_pending_flush_mixer(struct dpu_hw_ctl *ctx,
	enum dpu_lm lm)
{
	debug("%s: pending flush: LM_%d\n", ctx->cap->name, lm - LM_0);
	switch (lm) {
	case LM_0:
		ctx->pending_flush_mask |= BIT(6);
		break;
	case LM_1:
		ctx->pending_flush_mask |= BIT(7);
		break;
	case LM_2:
		ctx->pending_flush_mask |= BIT(8);
		break;
	case LM_3:
		ctx->pending_flush_mask |= BIT(9);
		break;
	case LM_4:
		ctx->pending_flush_mask |= BIT(10);
		break;
	case LM_5:
		ctx->pending_flush_mask |= BIT(20);
		break;
	case LM_6:
		ctx->pending_flush_mask |= BIT(21);
		break;
	case LM_7:
		ctx->pending_flush_mask |= BIT(27);
		break;
	default:
		break;
	}

	ctx->pending_flush_mask |= CTL_FLUSH_MASK_CTL;
}

static void dpu_hw_ctl_update_pending_flush_intf_v1(struct dpu_hw_ctl *ctx,
		enum dpu_intf intf)
{
	debug("%s: pending flush: INTF_%d\n", ctx->cap->name, intf - INTF_0);
	ctx->pending_intf_flush_mask |= BIT(intf - INTF_0);
	ctx->pending_flush_mask |= BIT(INTF_IDX);
}

static inline void dpu_hw_ctl_trigger_flush_v1(struct dpu_hw_ctl *ctx)
{
	debug("CTL_%d flush=%#x (intf_flush=%#x, dsc_flush=%#x)\n", ctx->idx - CTL_0, ctx->pending_flush_mask, ctx->pending_intf_flush_mask, ctx->pending_dsc_flush_mask);
	if (ctx->pending_flush_mask & BIT(INTF_IDX))
		DPU_REG_WRITE(&ctx->hw, CTL_INTF_FLUSH,
				ctx->pending_intf_flush_mask);

	if (ctx->pending_flush_mask & BIT(DSC_IDX))
		DPU_REG_WRITE(&ctx->hw, CTL_DSC_FLUSH,
			      ctx->pending_dsc_flush_mask);

	DPU_REG_WRITE(&ctx->hw, CTL_FLUSH, ctx->pending_flush_mask);
}

static const u32 fetch_tbl[SSPP_MAX] = {CTL_INVALID_BIT, 16, 17, 18, 19,
	CTL_INVALID_BIT, CTL_INVALID_BIT, CTL_INVALID_BIT, CTL_INVALID_BIT, 0,
	1, 2, 3, 4, 5};

static void dpu_hw_ctl_set_active_fetch_pipes(struct dpu_hw_ctl *ctx,
					      unsigned long *fetch_active)
{
	int i;
	u32 val = 0;

	if (fetch_active) {
		for (i = 0; i < SSPP_MAX; i++) {
			if (*fetch_active & BIT(i) &&
				fetch_tbl[i] != CTL_INVALID_BIT)
				val |= BIT(fetch_tbl[i]);
		}
	}

	DPU_REG_WRITE(&ctx->hw, CTL_FETCH_PIPE_ACTIVE, val);
}

struct ctl_blend_config {
	int idx, shift, ext_shift;
};

static const struct ctl_blend_config ctl_blend_config[][2] = {
	[SSPP_NONE] = { { -1 }, { -1 } },
	[SSPP_MAX] =  { { -1 }, { -1 } },
	[SSPP_VIG0] = { { 0, 0,  0  }, { 3, 0 } },
	[SSPP_VIG1] = { { 0, 3,  2  }, { 3, 4 } },
	[SSPP_VIG2] = { { 0, 6,  4  }, { 3, 8 } },
	[SSPP_VIG3] = { { 0, 26, 6  }, { 3, 12 } },
	[SSPP_RGB0] = { { 0, 9,  8  }, { -1 } },
	[SSPP_RGB1] = { { 0, 12, 10 }, { -1 } },
	[SSPP_RGB2] = { { 0, 15, 12 }, { -1 } },
	[SSPP_RGB3] = { { 0, 29, 14 }, { -1 } },
	[SSPP_DMA0] = { { 0, 18, 16 }, { 2, 8 } },
	[SSPP_DMA1] = { { 0, 21, 18 }, { 2, 12 } },
	[SSPP_DMA2] = { { 2, 0      }, { 2, 16 } },
	[SSPP_DMA3] = { { 2, 4      }, { 2, 20 } },
	[SSPP_DMA4] = { { 4, 0      }, { 4, 8 } },
	[SSPP_DMA5] = { { 4, 4      }, { 4, 12 } },
	[SSPP_CURSOR0] =  { { 1, 20 }, { -1 } },
	[SSPP_CURSOR1] =  { { 1, 26 }, { -1 } },
};

static void dpu_hw_ctl_setup_blendstage(struct dpu_hw_ctl *ctx,
	struct dpu_hw_mixer *mixer, struct dpu_hw_stage_cfg *stage_cfg)
{
	struct dpu_hw_blk_reg_map *c = &ctx->hw;
	enum dpu_lm lm = mixer->idx;
	u32 mix, ext, mix_ext;
	u32 mixercfg[5] = { 0 };
	int i, j;
	int stages;
	int pipes_per_stage;

	stages = mixer->cap->sblk->maxblendstages;
	if (stages < 0)
		return;

	if (test_bit(DPU_MIXER_SOURCESPLIT,
		&ctx->mixer_hw_caps->features))
		pipes_per_stage = PIPES_PER_STAGE;
	else
		pipes_per_stage = 1;

	mixercfg[0] = CTL_MIXER_BORDER_OUT; /* always set BORDER_OUT */

	// if (!stage_cfg)
	// 	goto exit;

	for (i = 0; i <= stages; i++) {
		/* overflow to ext register if 'i + 1 > 7' */
		mix = (i + 1) & 0x7;
		ext = i >= 7;
		mix_ext = (i + 1) & 0xf;

		for (j = 0 ; j < pipes_per_stage; j++) {
			enum dpu_sspp_multirect_index rect_index =
				stage_cfg->multirect_index[i][j];
			enum dpu_sspp pipe = stage_cfg->stage[i][j];
			const struct ctl_blend_config *cfg =
				&ctl_blend_config[pipe][rect_index == DPU_SSPP_RECT_1];

			/*
			 * CTL_LAYER has 3-bit field (and extra bits in EXT register),
			 * all EXT registers has 4-bit fields.
			 */
			if (cfg->idx == -1) {
				continue;
			} else if (cfg->idx == 0) {
				mixercfg[0] |= mix << cfg->shift;
				mixercfg[1] |= ext << cfg->ext_shift;
			} else {
				mixercfg[cfg->idx] |= mix_ext << cfg->shift;
			}
		}
	}

	debug("CTL_%d blending LM_%d [%#x %#x %#x %#x %#x]\n", ctx->idx - CTL_0, mixer->idx - LM_0,
		 mixercfg[0], mixercfg[1], mixercfg[2], mixercfg[3], mixercfg[4]);
	DPU_REG_WRITE(c, CTL_LAYER(lm), mixercfg[0]);
	DPU_REG_WRITE(c, CTL_LAYER_EXT(lm), mixercfg[1]);
	DPU_REG_WRITE(c, CTL_LAYER_EXT2(lm), mixercfg[2]);
	DPU_REG_WRITE(c, CTL_LAYER_EXT3(lm), mixercfg[3]);
	if (ctx->mdss_ver->core_major_ver >= 9)
		DPU_REG_WRITE(c, CTL_LAYER_EXT4(lm), mixercfg[4]);
}

/**
 * _stage_offset(): returns the relative offset of the blend registers
 * for the stage to be setup
 * @ctx:     mixer ctx contains the mixer to be programmed
 * @stage: stage index to setup
 */
static inline int _stage_offset(struct dpu_hw_mixer *ctx, enum dpu_stage stage)
{
	const struct dpu_lm_sub_blks *sblk = ctx->cap->sblk;
	if (stage != DPU_STAGE_BASE && stage <= sblk->maxblendstages)
		return sblk->blendstage_base[stage - DPU_STAGE_0];

	return -EINVAL;
}

static void dpu_hw_lm_setup_blend_config_combined_alpha(struct dpu_hw_mixer *ctx,
	u32 stage, u32 fg_alpha, u32 bg_alpha, u32 blend_op)
{
	struct dpu_hw_blk_reg_map *c = &ctx->hw;
	int stage_off;
	u32 const_alpha;

	if (stage == DPU_STAGE_BASE)
		return;

	stage_off = _stage_offset(ctx, stage);
	if (WARN_ON(stage_off < 0))
		return;

	const_alpha = (bg_alpha & 0xFF) | ((fg_alpha & 0xFF) << 16);
	DPU_REG_WRITE(c, LM_BLEND0_CONST_ALPHA + stage_off, const_alpha);
	DPU_REG_WRITE(c, LM_BLEND0_OP + stage_off, blend_op);
}

static void dpu_hw_ctl_intf_cfg_v1(struct dpu_hw_ctl *ctx,
		int intf, int intf_master, u32 dsc_mask)
{
	struct dpu_hw_blk_reg_map *c = &ctx->hw;
	u32 intf_active = 0;
	u32 dsc_active = 0;
	u32 wb_active = 0;
	u32 cwb_active = 0;
	u32 mode_sel = 0;
	u32 merge_3d_active = 0;

	/* CTL_TOP[31:28] carries group_id to collate CTL paths
	 * per VM. Explicitly disable it until VM support is
	 * added in SW. Power on reset value is not disable.
	 */
	if (ctx->mdss_ver->core_major_ver >= 7)
		mode_sel = CTL_DEFAULT_GROUP_ID  << 28;

	// if (cfg->intf_mode_sel == DPU_CTL_MODE_SEL_CMD)
	// 	mode_sel |= BIT(17);

	intf_active = DPU_REG_READ(c, CTL_INTF_ACTIVE);
	wb_active = DPU_REG_READ(c, CTL_WB_ACTIVE);
	cwb_active = DPU_REG_READ(c, CTL_CWB_ACTIVE);
	dsc_active = DPU_REG_READ(c, CTL_DSC_ACTIVE);
	merge_3d_active = DPU_REG_READ(c, CTL_MERGE_3D_ACTIVE);

	if (intf)
		intf_active |= BIT(intf - INTF_0);

	// if (cfg->wb)
	// 	wb_active |= BIT(cfg->wb - WB_0);

	// if (cfg->cwb)
	// 	cwb_active |= cfg->cwb;

	if (dsc_mask)
		dsc_active |= dsc_mask;

	// if (cfg->merge_3d)
	// 	merge_3d_active |= BIT(cfg->merge_3d - MERGE_3D_0);

	DPU_REG_WRITE(c, CTL_TOP, mode_sel);
	DPU_REG_WRITE(c, CTL_INTF_ACTIVE, intf_active);
	DPU_REG_WRITE(c, CTL_WB_ACTIVE, wb_active);
	DPU_REG_WRITE(c, CTL_CWB_ACTIVE, cwb_active);
	DPU_REG_WRITE(c, CTL_DSC_ACTIVE, dsc_active);
	DPU_REG_WRITE(c, CTL_MERGE_3D_ACTIVE, merge_3d_active);

	if (intf_master)
		DPU_REG_WRITE(c, CTL_INTF_MASTER, BIT(intf_master - INTF_0));

	// if (cfg->cdm)
	// 	DPU_REG_WRITE(c, CTL_CDM_ACTIVE, cfg->cdm);
}

// void mdp_intf_disable(void *dpu_base, const struct dpu_mdss_cfg *cat, unsigned intf_id)
// {
// 	unsigned intf = intf_id - INTF_0;
// 	void *base = dpu_base + cat->intf[intf].base;

// 	debug("%s:%d: %s\n", __FILE__, __LINE__, __func__);

// 	/* Disable fetch */
// 	u32 intf_cfg = DPU_REG_READ(base, INTF_CONFIG);
// 	DPU_REG_WRITE(base, INTF_CONFIG, intf_cfg & ~BIT(31));
// 	udelay(60 * 1000);

// 	writel(0, base + INTF_TIMING_ENGINE_EN);
// }

#define block_base(dpu_base, cat, block, index) ((ulong)dpu_base + (cat)->block[index].base)

bool regdump_decode(u64 addr, u32 val);
bool regdump_read_decode(u64 addr);

// bool regdump_decode(u64 addr, u32 val) {
// 	printf("%#llx: %#x\n", addr, val);
// 	return true;
// }

// bool regdump_read_decode(u64 addr) {
// 	return regdump_decode(addr, readl(addr));
// }

static void qcom_dpu_dump_reg(ulong addr)
{
	u32 val = readl(addr);
	if (val) {
		printf("%#lx: ",addr);
		regdump_decode(addr, val);
	}
	
	// if (!regdump_read_decode(addr)) {
	// 	printf("\n");
	// }
		// printf("%#010x\n", readl(bbase + j));
}

static void qcom_dpu_dump_sspp(struct dpu_hw_sspp *sspp)
{
	ulong bbase = (ulong)sspp->hw.blk_addr;
	printf("====================SSPP_%s================\n", dpu_sspp_str(sspp->idx));

	for (int j = 0; j < sspp->cap->len; j+= 4) {
		qcom_dpu_dump_reg(bbase + j);
	}

	printf("\n");
}

static void qcom_dpu_dump_ctl(struct dpu_hw_ctl *ctl)
{
	ulong bbase = (ulong)ctl->hw.blk_addr;
	printf("====================CTL_%d================\n", ctl->idx - CTL_0);

	for (int j = 0; j < ctl->cap->len; j+= 4) {
		qcom_dpu_dump_reg(bbase + j);
	}

	printf("\n");
}

/* Dump the encoder state, for non-dsc_merge the ctl and sspp are shared so don't dump them twice */
static void qcom_dpu_dump_encoder_state(struct dpu_phys_encoder *enc, bool shared_sspp)
{
	printf("\n\n=== %s Encoder ===\n", split_role_str(enc->split_role));

	if (!shared_sspp)
		qcom_dpu_dump_sspp(enc->sspp);


	for (int i = 0; i < enc->num_mixer; i++) {
		struct dpu_hw_mixer *lm = &enc->mixer[i];
		ulong bbase = (ulong)lm->hw.blk_addr;
		printf("====================LM_%d================\n", lm->idx - LM_0);

		for (int j = 0; j < lm->cap->len; j+= 4) {
			qcom_dpu_dump_reg(bbase + j);
		}

		printf("\n");
	}

	for (int i = 0; i < enc->num_dsc; i++) {
		struct dpu_hw_dsc *dsc = &enc->dsc[i];
		ulong bbase = (ulong)dsc->hw.blk_addr;
		printf("====================DSC_%d================\n", dsc->idx - DSC_0);

		for (int j = 0; j < dsc->cap->len; j+= 4) {
			qcom_dpu_dump_reg(bbase + j);
		}

		printf("\n  ==== CTL ====\n");

		ulong sbbase = bbase + dsc->cap->sblk->ctl.base;
		for (int j = 0; j < dsc->cap->sblk->ctl.len; j+= 4) {
			qcom_dpu_dump_reg(sbbase + j);
		}

		printf("\n  ==== ENC ====\n");

		sbbase = bbase + dsc->cap->sblk->enc.base;
		for (int j = 0; j < dsc->cap->sblk->enc.len; j+= 4) {
			qcom_dpu_dump_reg(sbbase + j);

			if (j > 0x3c)
				break;
		}

		printf("\n");
	}

	for (int i = 0; i < enc->num_pingpong; i++) {
		struct dpu_hw_pingpong *pp = &enc->pingpong[i];
		ulong bbase = (ulong)pp->hw.blk_addr;
		printf("====================PINGPONG_%d================\n", pp->idx - PINGPONG_0);

		for (int j = 0; j < pp->cap->len; j+= 4) {
			qcom_dpu_dump_reg(bbase + j);
		}

		printf("\n");
	}

	{
		struct dpu_hw_intf *intf = &enc->intf;
		ulong bbase = (ulong)intf->hw.blk_addr;
		printf("====================INTF_%d================\n", intf->idx - INTF_0);

		for (int j = 0; j < intf->cap->len; j+= 4) {
			qcom_dpu_dump_reg(bbase + j);
		}

		printf("\n");
	}
}

void qcom_dpu_dump_state(struct udevice *dev)
{
	struct qcom_dpu_priv *priv = dev_get_priv(dev);
	struct dpu_topology *topology = &priv->topology;

	bool shared_sspp = topology->num_sspp < topology->num_encs;
	// printf("VBIF 0x%p\n", priv->vbif_base);

	// regdump_read_decode((u64)priv->vbif_base + VBIF_OUT_AXI_AMEMTYPE_CONF0);
	// regdump_read_decode((u64)priv->vbif_base + VBIF_OUT_AXI_AMEMTYPE_CONF1);

	printf("\n\n=== Dumping DPU state ===\n");

	for (int i = 0; i < topology->num_encs; i++)
		qcom_dpu_dump_encoder_state(&topology->encs[i], shared_sspp);

	qcom_dpu_dump_ctl(&topology->ctl[0]);

	if (!shared_sspp)
		return;

	qcom_dpu_dump_sspp(&topology->sspp[0]);
}

/* Static msm format for easier Linux compat */
static void qcom_dpu_init_format(struct msm_format *fmt)
{
	/*
	INTERLEAVED_RGBA_FMT(ARGB8888, 4,
		BPC8A, BPC8, BPC8, BPC8,
		C1_B_Cb, C0_G_Y, C2_R_Cr, C3_ALPHA),
	*/
	fmt->pixel_format = 0; /* don't actually need this */
	fmt->bpc_g_y = BPC8;
	fmt->bpc_b_cb = BPC8;
	fmt->bpc_r_cr = BPC8;
	fmt->bpc_a = BPC8A;
	fmt->element[0] = C1_B_Cb;
	fmt->element[1] = C0_G_Y;
	fmt->element[2] = C2_R_Cr;
	fmt->element[3] = C3_ALPHA;
	fmt->fetch_type = MDP_PLANE_INTERLEAVED;
	fmt->chroma_sample = CHROMA_FULL;
	fmt->alpha_enable = false;
	fmt->unpack_count = 4;
	fmt->bpp = 4;
	fmt->flags = MSM_FORMAT_FLAG_UNPACK_TIGHT;
	fmt->num_planes = 1;
	fmt->fetch_mode = MDP_FETCH_LINEAR;
	fmt->tile_height = 1; /* MDP_TILE_HEIGHT_DEFAULT */
}

static int qcom_dpu_hw_setup_encoder(struct dpu_phys_encoder *enc,
				     bool dsc_merge,
				     struct dpu_hw_fmt_layout *layout,
				     struct drm_dsc_config *dsc,
				     struct msm_format *fmt,
				     const struct display_timing *timings)
{
	struct dpu_hw_intf *intf = &enc->intf;
	struct dpu_hw_ctl *ctl = enc->ctl;
	u32 dsc_mask = 0;
	int intf_master = 0;

	debug("\n=== Setting up %s encoder ===\n", split_role_str(enc->split_role));

	/* 1. Set up the INTF */
	/* 1.a. Program the display timings and DSC config */

	// dpu_encoder_helper_split_config -> dpu_hw_setup_split_pipe ??
	dpu_hw_intf_setup_timing_engine(intf, timings, fmt, dsc, !!enc->num_dsc);
	// ctl -> setup_intf_cfg
	for (int i = 0; i < enc->num_dsc; i++)
		dsc_mask |= BIT(enc->dsc[i].idx - DSC_0);
	if (enc_is_primary(enc))
		intf_master = intf->idx;

	dpu_hw_ctl_intf_cfg_v1(ctl, intf->idx, intf_master, dsc_mask);

	/* 1.b. Bind the primary pingpong block to the INTF and set up programmable fetch */

	// intf -> bind_pp
	dpu_hw_intf_bind_pingpong_blk(intf, enc->pingpong[0].idx);
	// intf -> programmable fetch
	dpu_hw_intf_setup_prg_fetch(intf, timings);

	/* 2.a. Stash the appropriate flush bits in the CTL for our configuration */

	// ctl -> update_pending_flush_intf
	dpu_hw_ctl_update_pending_flush_intf_v1(ctl, intf->idx);

	dpu_hw_ctl_update_pending_flush_sspp(ctl, enc->sspp->idx);

	/* 3.a. Create our static blending (zpos) config for the layer mixers */

	// For each LM in encoder ...
	// XXX: is it fine that num_mixer >1 implies quad pipe / dsc_merge ?
	// XXX: open-coding _dpu_crtc_blend_setup_mixer() and _dpu_crtc_blend_setup()
	struct dpu_hw_stage_cfg stage_cfg = { 0 };
	/* We only use a single stage */
	enum dpu_stage stage = DPU_STAGE_0;

	for (int i = 0; i < enc->num_mixer; i++) {
		/* Set up blending, we just have 1 stage :D */
		stage_cfg.stage[stage][i] = enc->sspp->idx;
		/* With DSC merge we have an SSPP
		 * per interface with 2 mixers, for normal dual DSI we have a single SSPP
		 * for both interfaces and we should be fine with just one DMA engine
		 */
		stage_cfg.multirect_index[stage][i] = enc->num_mixer > 1 ? DPU_SSPP_RECT_0 + i : DPU_SSPP_RECT_SOLO;
	}

	for (int i = 0; i < enc->num_mixer; i++) {
		struct dpu_hw_mixer *lm = &enc->mixer[i];

		dpu_hw_lm_setup_color3(lm, (1 << stage));

		dpu_hw_lm_setup_blend_config_combined_alpha(lm, stage,
			0xff, 0x00, DPU_BLEND_BG_ALPHA_BG_CONST);

		/* Update CTL state so the mixer gets flushed */
		dpu_hw_ctl_update_pending_flush_mixer(ctl, lm->idx);

		/* Program layer mixer size and left/right split */
		dpu_hw_lm_setup_out(lm, &lm->cfg);

		dpu_hw_ctl_setup_blendstage(enc->ctl, lm, &stage_cfg);

		/* 4.a. Program the SSPP DMA engines to fetch the frame */
		// dpu_hw_sspp_setup_rects()
		// SSPP pipe configuration
		struct dpu_sw_pipe pipe;
		struct dpu_sw_pipe_cfg pipe_cfg;

		pipe.sspp = enc->sspp;
		/* This value is only read when not in solo mode */
		pipe.multirect_mode = DPU_SSPP_MULTIRECT_PARALLEL;
		pipe.multirect_index = stage_cfg.multirect_index[DPU_STAGE_0][i];

		/* Only configure the SSPP once unless we're doing multirect */
		if (enc_is_primary(enc) || pipe.multirect_index != DPU_SSPP_RECT_SOLO) {
			// FIXME: put this math in a new function...
			/*
			* Configure the SSPP fetch to match the mixer, using both DMA
			* engines in the SSPP for quad pipe.
			*/
			if (dsc_merge) {
				u32 dst_x_off = lm->cfg.out_width * (lm->cfg.right_mixer ? 1 : 0);
				u32 src_x_off = dst_x_off;
				/* If only a single mixer then it will handle the entire encoder plane */
				if (enc->num_mixer == 1)
					src_x_off = 0;
				pipe_cfg.src_rect.x1 = enc->plane.x1 + src_x_off;
				pipe_cfg.src_rect.y1 = enc->plane.y1;
				pipe_cfg.src_rect.x2 = pipe_cfg.src_rect.x1 + lm->cfg.out_width;
				pipe_cfg.src_rect.y2 = enc->plane.y2;
				pipe_cfg.dst_rect.x1 = dst_x_off;
				pipe_cfg.dst_rect.y1 = 0;
				pipe_cfg.dst_rect.x2 = dst_x_off + lm->cfg.out_width;
				pipe_cfg.dst_rect.y2 = lm->cfg.out_height;
			} else {
				pipe_cfg.src_rect.x1 = 0;
				pipe_cfg.src_rect.y1 = 0;
				pipe_cfg.src_rect.x2 = layout->width;
				pipe_cfg.src_rect.y2 = layout->height;
				memcpy(&pipe_cfg.dst_rect, &pipe_cfg.src_rect, sizeof(pipe_cfg.dst_rect));
			}
			dpu_hw_sspp_setup_sourceaddress(&pipe, layout);
			dpu_hw_sspp_setup_rects(&pipe, &pipe_cfg);
			dpu_hw_sspp_setup_multirect(&pipe);

			/* 4.b. Program the framebuffer format into the SSPP */
			dpu_hw_sspp_setup_format(&pipe, fmt);

			dpu_hw_sspp_setup_cdp(&pipe, fmt, true);
		}
	}
	
	// dpu_encoder_prep_dsc()
	if (enc->num_dsc) {
		int this_frame_slices;
		int intf_ip_w, enc_ip_w;
		int dsc_common_mode;
		int pic_width;
		u32 initial_lines;

		pic_width = dsc->pic_width;

		dsc_common_mode = 0;
		if (enc->num_dsc > 1)
			dsc_common_mode |= DSC_MODE_SPLIT_PANEL;
		if (dsc_merge)
			dsc_common_mode |= DSC_MODE_MULTIPLEX;
		// FIXME: get panel mode!
		// if (enc_master->intf_mode == INTF_MODE_VIDEO)
		dsc_common_mode |= DSC_MODE_VIDEO;

		this_frame_slices = pic_width / dsc->slice_width;
		intf_ip_w = this_frame_slices * dsc->slice_width;

		enc_ip_w = intf_ip_w / enc->num_dsc;
		initial_lines = dpu_encoder_dsc_initial_line_calc(dsc, enc_ip_w);

		if (enc->num_dsc != enc->num_pingpong) {
			pr_err("Mismatching DSC to PINGPONG blocks! (%d dsc %d pp)\n", enc->num_dsc, enc->num_pingpong);
			return -EINVAL;
		}
		for (int i = 0; i < enc->num_dsc; i++) {
			struct dpu_hw_dsc *hw_dsc = &enc->dsc[i];
			struct dpu_hw_pingpong *hw_pp = &enc->pingpong[i];
			// dpu_encoder_dsc_pipe_cfg
			dpu_encoder_dsc_pipe_cfg(hw_dsc, hw_pp, dsc, dsc_common_mode, initial_lines);
			// dpu_hw_ctl_update_pending_flush_dsc_v1
			ctl->pending_dsc_flush_mask |= BIT(hw_dsc->idx - DSC_0);
			ctl->pending_flush_mask |= BIT(DSC_IDX);
		}
	}

	/* For dsc_merge each INTF has it's own CTL */
	// if (dsc_merge)
	// 	dpu_hw_ctl_trigger_flush_v1(ctl);

	return 0;
}

static int qcom_dpu_hw_init(struct udevice *dev, struct display_timing *timings)
{
	struct qcom_dpu_priv *priv = dev_get_priv(dev);
	struct video_uc_plat *uc_plat = dev_get_uclass_plat(dev);
	struct drm_dsc_config *dsc = &priv->dsc;
	struct msm_format fmt;
	struct dpu_topology *topology = &priv->topology;
	int ret;

	/* Determine the encoder topology */
	struct dpu_resource_request req = {
		.dsc = priv->dsc_en,
		.dpu_base = priv->dpu_base,
		.width = timings->hactive.typ,
		.height = timings->vactive.typ,
		.intf_type = INTF_DSI,
		.core_clk_max_rate = clk_get_rate(&priv->bulk.clks[3]),
		.cat = priv->cat,
	};

	ret = dpu_hw_get_encoder_topology(&req, priv->dual_dsi, topology);
	if (ret) {
		printf("Failed to get encoder topology: %d\n", ret);
		return ret;
	}

	/* Example topology: */

	// DPU: Primary encoder: DSI (QUAD PIPE) 2160x2160+0   +0
	//   [CTL_0: 0xae16000]
	//   |
	//   +-[SSPP_DMA0: 0xae25000]
	//   |
	//   +-[LM_0 : 0xae45000]---[LM_1 : 0xae46000]
	//   +-(1080x2160+0   +0)---(1080x2160+1080+0)
	//   +-[DSC_0: 0xae81000]---[DSC_1: 0xae81000]
	//   |
	//   +-[PP_0 : 0xae6a000]---[PP_1 : 0xae6b000]
	//   |
	//    ---[INTF_1: 0xae36000]

	// DPU: Secondary encoder: DSI (QUAD PIPE) 2160x2160+2160+0
	//   [CTL_1: 0xae17000]
	//   |
	//   +-[SSPP_DMA1: 0xae27000]
	//   |
	//   +-[LM_2 : 0xae47000]---[LM_3 : 0xae48000]
	//   +-(1080x2160+2160+0)---(1080x2160+3240+0)
	//   +-[DSC_2: 0xae82000]---[DSC_3: 0xae82000]
	//   |
	//   +-[PP_2 : 0xae6c000]---[PP_3 : 0xae6d000]
	//   |
	//    ---[INTF_2: 0xae37000]

	/* Setup format */
	qcom_dpu_init_format(&fmt);

	/* SSPP format layout */
	struct dpu_hw_fmt_layout layout = { 0 };
	layout.plane_addr[0] = uc_plat->base;
	/*
	 * Stride needs to account for the full size of the framebuffer
	 * regardless of if we're doing dual DSI / quad pipe
	 */
	layout.plane_pitch[0] = timings->hactive.typ * fmt.bpp;
	layout.width = timings->hactive.typ;
	layout.height = timings->vactive.typ;

	/* Adjust mode for dual-DSI */
	if (priv->dual_dsi) {
		timings->hactive.typ >>= 1;
		timings->hfront_porch.typ >>= 1;
		timings->hback_porch.typ >>= 1;
		timings->hsync_len.typ >>= 1;
	}

	mdss_vbif_setup(priv->vbif_base);

	struct dpu_phys_encoder *enc0 = &topology->encs[0];
	struct dpu_phys_encoder *enc1 = &topology->encs[1];

	/* 2.b. Program the CTL_FETCH_ATIVE register with the SSPP, this only needs to be done
	 * for the primary encoder if the SSPP is shared.
	 */
	unsigned long active_fetch = 0;

	for (int i = 0; i < topology->num_sspp; i++)
		active_fetch |= BIT(topology->sspp[i].idx);

	dpu_hw_ctl_set_active_fetch_pipes(&topology->ctl[0], &active_fetch);

	qcom_dpu_hw_setup_encoder(enc0, topology->dsc_merge, &layout, dsc, &fmt, timings);
	if (topology->num_encs > 1 && priv->bridge1)
		qcom_dpu_hw_setup_encoder(enc1, topology->dsc_merge, &layout, dsc, &fmt, timings);

	// Trigger flush for single DSI or dual-DSI with a shared CTL.
	if (topology->num_ctl == 1)
		dpu_hw_ctl_trigger_flush_v1(&topology->ctl[0]);

	// Finally:
	dpu_hw_intf_enable_timing_engine(&enc0->intf, 1);
	if (topology->num_encs > 1)
		dpu_hw_intf_enable_timing_engine(&enc1->intf, 1);

	return 0;
}

int qcom_dpu_probe(struct udevice *dev)
{
	struct qcom_dpu_priv *priv = dev_get_priv(dev);
	struct video_priv *uc_priv = dev_get_uclass_priv(dev);
	struct display_timing timings;
	u32 fb_stride;
	int ret;

	if (tried_probe) {
		pr_err("NOT RETRYING PROBE!\n");
		return -EIO;
	}

	tried_probe = true;

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
	ret = uclass_get_device_by_endpoint(UCLASS_VIDEO_BRIDGE, dev, 0, -1, &priv->bridge0);
	if (ret) {
		dev_err(dev, "Couldn't get DSI 0 via port graph! %d\n", ret);
		return ret;
	}

	priv->sync_dsi = dev_read_bool(priv->bridge0, "qcom,sync-dual-dsi");
	priv->dual_dsi = dev_read_bool(priv->bridge0, "qcom,dual-dsi-mode");
	if (priv->dual_dsi)
		pr_info("Setting up dual DSI!\n");

	// TOFIX follow port/endpoint instead
	ret = uclass_get_device_by_endpoint(UCLASS_VIDEO_BRIDGE, dev, 1, -1, &priv->bridge1);
	if (!ret) {
		ret = video_bridge_attach(priv->bridge1);
		if (ret) {
			dev_err(dev, "fail to attach bridge1\n");
			return ret;
		}
	} else if (ret != -ENODEV) {
		return ret;
	} else if (priv->dual_dsi) {
		dev_err(dev, "qcom,dual-dsi set but unable to find second bridge! %d\n", ret);
		return ret;
	}

	ret = video_bridge_attach(priv->bridge0);
	if (ret) {
		dev_err(dev, "fail to attach bridge\n");
		return ret;
	}

	// We assume either two identical panels or a dual-dsi panel
	video_bridge_get_display_timing(priv->bridge0, &timings);
	if (ret) {
		dev_err(dev, "fail to get display timings\n");
		return ret;
	}

	if (!priv->sync_dsi) {
		ret = video_bridge_set_backlight(priv->bridge0, 100);
		if (ret) {
			dev_err(dev, "bridge1 enable backlight error %d\n", ret);
			return ret;
		}
	}

	if (priv->bridge1) {
		ret = video_bridge_set_backlight(priv->bridge1, 100);
		if (ret) {
			dev_err(dev, "bridge enable backlight error %d\n", ret);
			return ret;
		}
	}

	priv->dsc_en = true;
	ret = video_bridge_get_dsc_config(priv->bridge0, &priv->dsc);
	if (ret)
		priv->dsc_en = false;
	// else
	// 	drm_dsc_dump_config(&priv->dsc);

	/* Configure & start DPU */

	uc_priv->bpix = VIDEO_BPP32;
	uc_priv->format = VIDEO_X8B8G8R8;
	uc_priv->xsize = timings.hactive.typ;
	uc_priv->ysize = timings.vactive.typ;
	// FIXME: read from DT! Also warn about vidconsole_rotate driver not being available!
	u32 rot_degrees = 0;
	struct udevice *panel;
	ret = uclass_get_device_by_endpoint(UCLASS_PANEL, priv->bridge0, 1, -1, &panel);
	if (!ret)
		rot_degrees = dev_read_u32_default(panel, "rotation", 0);
	uc_priv->rot = rot_degrees / 90;

	fb_stride = timings.hactive.typ * 4;

	ret = qcom_dpu_hw_init(dev, &timings);
	if (ret) {
		dev_err(dev, "hw init failed! %d\n", ret);
		return ret;
	}

	/*
	 * Display pipeline *
	 *
	 * Rather than support fancy dynamic resource management, since we only need a single buffer here we mostly
	 * hardcode the display pipeline.
	 * SSPP maximum size is ~4096x4096, similar for Layer Mixer (??)
	 *
	 * Single DSI usecase is simple:
	 *  - Single DMA through SSPP with no translation/scaling
	 *  - Goes through single Layer Mixer to the appropriate interface (through pingpong block)
	 *  - Single control is used to drive everything
	 *
	 * Dual DSI single panel (bonded) is more complex:
	 *  - Still just one SSPP (typically)
	 *  - Two layer mixers, one for each half of the SSPP
	 *  - Each interface/pingpong mapped to each layer mixer
	 *  - Single control still drives everything
	 *
	 * DSC: tied to pingpong block
	 *
	 * Avoid dual SSPP / dual DSC, shouldn't be needed?
	 */

	/* Panel 0 */
	// mdss_intf_setup(priv, &timings, INTF_1, PINGPONG_0);

	// if (priv->dsc_en)
	// 	mdp_dsc_setup(priv, &timings, DSC_0, PINGPONG_0);


	// mdp_setup_stage_rec1(priv, priv->cat, CTL_0, LM_0, SSPP_VIG0, timings.vactive.typ, timings.hactive.typ, false);
	// // TOFIX use companion LM id from catalog
	// // if (quad_pipe) // two LMs per SSPP!
	// // mdp_setup_stage_rec1(priv, priv->cat, CTL_0, LM_1, SSPP_VIG0, timings.vactive.typ, timings.hactive.typ, false);

	// // Since the panels are rotated, use the right side of the fb for panel0
	// mdss_sspp_config_rec1(priv, uc_plat->base,
	// 		      fb_stride, SSPP_VIG0, timings.vactive.typ, timings.hactive.typ, 0);

	// // mdss_intf_fetch_start_config(priv->dpu_base, priv->cat,
	// // 			     CTL_0, INTF_1, DSC_0, DSC_1);

	// /* Panel 1 */
	// if (priv->bridge1) {
	// 	mdss_intf_setup(priv, &timings, INTF_2, PINGPONG_1);

	// 	if (priv->dsc_en)
	// 		mdp_dsc_setup(priv, &timings, DSC_1, PINGPONG_1);

	// 	mdp_setup_stage_rec1(priv, priv->cat, CTL_0, LM_1, SSPP_VIG0, timings.vactive.typ, timings.hactive.typ, true);
	// 	// mdp_setup_stage_rec1(priv, priv->cat, CTL_0, LM_1, SSPP_VIG1, timings.vactive.typ, timings.hactive.typ, true);

	// 	// mdp_setup_stage_rec1(priv, priv->cat, CTL_1, LM_2, SSPP_VIG1, timings.vactive.typ, timings.hactive.typ, false);
	// 	// // TOFIX use companion LM id from catalog
	// 	// mdp_setup_stage_rec1(priv, priv->cat, CTL_1, LM_3, SSPP_VIG1, timings.vactive.typ, timings.hactive.typ, true);

	// 	// Since the panels are rotated, use the left side of the fb for panel0
	// 	// if (priv->dual_dsi)
	// 	// 	x_off /= 2;
	// 	// mdss_sspp_config_rec1(priv, uc_plat->base,
	// 	// 		fb_stride, SSPP_VIG1, timings.vactive.typ, timings.hactive.typ, x_off);

	// 	// mdss_intf_fetch_start_config(priv->dpu_base, priv->cat,
	// 	// 		CTL_0, INTF_2, DSC_2, DSC_3);

	// 	// TOFIX fill a flush bitfield from the config commands
	// 	// mdp_ctl_flush(priv->dpu_base, priv->cat, CTL_1, SSPP_VIG1, false,
	// 	// 		LM_2, LM_3);

	// 	// if (quad_pipe)
	// 	// 	sspp_ctl_active_mask |= BIT(17)
	// }

	// u32 sspp_ctl_active_mask = BIT(16); // VIG_0

	// u32 intf = INTF_1 - INTF_0;
	// u32 intf_active = BIT(intf);

	// writel(BIT(intf), ctl_base + CTL_INTF_MASTER);

	// if (priv->bridge1)
	// 	intf_active |= BIT(INTF_2 - INTF_0);

	// // TOFIX make dsc optional
	// if (priv->dsc_en) {
	// 	u32 dsc_bits = BIT(DSC_0 - DSC_0);
	// 	if (priv->bridge1)
	// 		dsc_bits |= BIT(DSC_1 - DSC_0);
	// 	writel(dsc_bits, ctl_base + CTL_DSC_ACTIVE);
	// 	writel(dsc_bits, ctl_base + CTL_DSC_FLUSH);
	// }

	// // FIXME: assuming intf_0 is written first...
	// writel(intf_active, ctl_base + CTL_INTF_ACTIVE);
	// writel(intf_active, ctl_base + CTL_INTF_FLUSH);

	// // TOFIX fill a flush bitfield from the config commands
	// // flush SSPP 0 and 1
	// mdp_ctl_flush(priv->dpu_base, priv->cat, CTL_0, sspp_ctl_active_mask, false,
	// 	      LM_0, LM_1);

	// // TOFIX should we use bonded dsi instead ?
	// //writel(FLD_INTF_2_SW_TRG_MUX, priv->dpu_base + SPLIT_DISPLAY_LOWER_PIPE_CTRL);

	// mdp_intf_enable(priv->dpu_base, priv->cat, INTF_1);
	// if (priv->bridge1)
	// 	mdp_intf_enable(priv->dpu_base, priv->cat, INTF_2);

	// qcom_dpu_dump_state(dev);

#if 0
	local_priv = priv;
	dpu_kms_mdp_snapshot();
#endif
	debug("probed!\n");
	return 0;
}

static int qcom_dpu_video_sync(struct udevice *dev)
{
	// struct qcom_dpu_priv *priv = dev_get_priv(dev);

	// if (priv->bridge0)
	// 	qcom_dsi_video_sync(priv->bridge0);
	// if (priv->bridge1)
	// 	qcom_dsi_video_sync(priv->bridge1);

	return 0;
}

static int qcom_dpu_remove(struct udevice *dev)
{
	struct qcom_dpu_priv *priv = dev_get_priv(dev);
	void *ctl_base = priv->dpu_base + priv->cat->ctl[CTL_0 - CTL_0].base;

	/* Disable INTFs to stop SMMU faults in Linux */
	writel(0, ctl_base + CTL_INTF_ACTIVE);

	// mdp_intf_disable(priv->dpu_base, priv->cat, INTF_1);
	// if (priv->bridge1)
	// 	mdp_intf_disable(priv->dpu_base, priv->cat, INTF_2);

	// clk_disable_bulk(&priv->bulk);

	return 0;
}

#define LCD_MAX_WIDTH 4320
#define LCD_MAX_HEIGHT 2160
static int qcom_dpu_bind(struct udevice *dev)
{
	struct video_uc_plat *uc_plat = dev_get_uclass_plat(dev);
	phys_addr_t base;
	int ret;

	uc_plat->size = (LCD_MAX_WIDTH * LCD_MAX_HEIGHT *
			((1 << VIDEO_BPP32) / 8));
	uc_plat->align = SZ_2M;
	dev_dbg(dev, "frame buffer max size %d bytes align %x\n",
		uc_plat->size, uc_plat->align);

	// FIXME: try to get reserved framebuffer region
	base = 0xd5100000;
	ret = lmb_alloc_mem(LMB_MEM_ALLOC_ADDR, SZ_4K, &base, uc_plat->size, LMB_NONE);
	if (ret) {
		base = UINT32_MAX;
		ret = lmb_alloc_mem(LMB_MEM_ALLOC_MAX, SZ_4K, &base, uc_plat->size, LMB_NONE);
		if (ret < 0) {
			dev_err(dev, "Couldn't allocate framebuffer! %d\n", ret);
			return ret;
		}
	}

	pr_info("Using framebuffer address: %llu\n", base);

	uc_plat->base = base;

	return 0;
}

static const struct udevice_id dpu_ids[] = {
	{ .compatible = "qcom,sm8650-dpu" },
	{ }
};

static const struct video_ops qcom_dpu_video_ops = {
	.video_sync = qcom_dpu_video_sync,
};

U_BOOT_DRIVER(qcom_dpu) = {
	.name	= "qcom_dpu",
	.id	= UCLASS_VIDEO,
	.of_match = dpu_ids,
	.ops	= &qcom_dpu_video_ops,
	.bind	= qcom_dpu_bind,
	.probe	= qcom_dpu_probe,
	.remove	= qcom_dpu_remove,
	.priv_auto	= sizeof(struct qcom_dpu_priv),
	.flags	= DM_FLAG_LEAVE_PD_ON,
};
