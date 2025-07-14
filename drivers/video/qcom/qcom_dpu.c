/* SPDX-License-Identifier: GPL-2.0-only */

/* this file handles configuration of VBIF/CTL/SSPP/INTF/LM/PP/DSC in DPU */

#define DEBUG 7
#include <asm/io.h>
#include <clk.h>
#include <dm/device.h>
#include <dm/device_compat.h>
#include <dm/read.h>
#include <panel.h>
#include <video.h>
#include <video_bridge.h>
#include <linux/sizes.h>

#include "dpu_hw_mdss.h"
//#include "mdssreg.h"
#include "catalog/dpu_hw_catalog.h"

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
#define LM_BLEND0_CONST_ALPHA            0x04
#define LM_FG_COLOR_FILL_COLOR_0         0x08
#define LM_FG_COLOR_FILL_COLOR_1         0x0C
#define LM_FG_COLOR_FILL_SIZE            0x10
#define LM_FG_COLOR_FILL_XY              0x14

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


/* 4 pipe usage case */
#define STAGES_PER_PLANE	2
#define PIPES_PER_STAGE		2
#define PIPES_PER_PLANE		(PIPES_PER_STAGE * STAGES_PER_PLANE)







#define dpu_write(reg, val) writel(val, reg)


/* traffic shaper clock in Hz */
#define TS_CLK			19200000

struct qcom_dpu_priv {
	void *regs_base;
	void *vbif_base;
/*
	struct mdp_sspp_regs *mdp_sspp[2];
	struct mdp_layer_mixer_regs *mdp_layer_mixer[4];
	struct mdp_dsc_regs *mdp_dsc[4];
	struct mdp_intf_regs *mdp_intf[2];
	struct mdp_ctl_regs *mdp_ctl;
	struct vbif_rt_regs *vbif_rt;
*/
};

#define MDSS_MDP_MAX_PREFILL_FETCH 24

/* call 4 times to configure rect0/rect1 of DMA0/1 on 8650 */
static void mdss_sspp_config(void *base, bool rect0, int x_off)
{
#if 0
	uint32_t img_size, out_size, stride;
	uint32_t fb_off = 0;
	uint32_t flip_bits = 0;
	uint32_t src_xy = 0;
	uint32_t dst_xy = 0;

	/* write active region size*/
	img_size = (edid->mode.va << 16) | edid->mode.ha;
	out_size = img_size;
	stride = (edid->mode.ha * edid->framebuffer_bits_per_pixel/8);

	dst_xy = (edid->mode.vborder << 16) | edid->mode.hborder;
	src_xy = dst_xy;

	printk(BIOS_INFO, "%s: src=%x fb_off=%x src_xy=%x dst_xy=%x\n",
		   __func__, out_size, fb_off, src_xy, dst_xy);

	/* gd->fb_base is from reserve_video() */
	write32(mdp_sspp->sspp_src0, gd->fb_base);
	write32(mdp_sspp->sspp_src_ystride0, stride);
	write32(mdp_sspp->sspp_src_size, out_size);
	write32(mdp_sspp->sspp_out_size, out_size);
	write32(mdp_sspp->sspp_src_xy, src_xy);
	write32(mdp_sspp->sspp_out_xy, dst_xy);

	/* Tight Packing 4bpp Alpha 8-bit A R B G */
	write32(mdp_sspp->sspp_src_format, 0x000236ff);
	write32(mdp_sspp->sspp_src_unpack_pattern, 0x03020001);

	flip_bits |= SW_PIX_EXT_OVERRIDE;
	write32(mdp_sspp->sspp_sw_pic_ext_c0_req_pixels, out_size);
	write32(mdp_sspp->sspp_sw_pic_ext_c1c2_req_pixels, out_size);
	write32(mdp_sspp->sspp_sw_pic_ext_c3_req_pixels, out_size);
	write32(mdp_sspp->sspp_src_op_mode, flip_bits);
#endif

	if (rect0) {
		/* gd->fb_base is from reserve_video() */
		writel(gd->video_bottom, base + SSPP_SRC0_ADDR);
		writel(x_off, base + SSPP_SRC_XY); /* 0x438 * N */
		writel(0x8700438, base + SSPP_OUT_SIZE);
		writel(0, base + SSPP_OUT_XY);
		writel(0x236FF, base + SSPP_SRC_FORMAT);
		writel(0x3020001, base + SSPP_SRC_UNPACK_PATTERN);
		writel(0x80000000, base + SSPP_SRC_OP_MODE);
		writel(0x9, base + SSPP_CDP_CNTL);
	} else {
		writel(gd->video_bottom, base + SSPP_SRC1_ADDR);
		writel(x_off, base + SSPP_SRC_XY_REC1); /* 0x438 * N */
		writel(0x8700438, base + SSPP_OUT_SIZE_REC1);
		writel(0x438, base + SSPP_OUT_XY_REC1);
		writel(0x236FF, base + SSPP_SRC_FORMAT_REC1);
		writel(0x3020001, base + SSPP_SRC_UNPACK_PATTERN_REC1);
		writel(0x80000000, base + SSPP_SRC_OP_MODE_REC1);
		writel(0x9, base + SSPP_CDP_CNTL_REC1);
	}
	writel(0x43804380, base + SSPP_SRC_YSTRIDE0);
	writel(0, base + SSPP_SRC_YSTRIDE1);
	writel(0x8700438, base + SSPP_SRC_SIZE);
	writel(0, base + SSPP_SW_PIX_EXT_C0_LR);
	writel(0, base + SSPP_SW_PIX_EXT_C0_TB);
	writel(0x8700438, base + SSPP_SW_PIX_EXT_C0_REQ_PIXELS);
	writel(0, base + SSPP_SW_PIX_EXT_C1C2_LR);
	writel(0, base + SSPP_SW_PIX_EXT_C1C2_TB);
	writel(0x8700438, base + SSPP_SW_PIX_EXT_C1C2_REQ_PIXELS);
	writel(0, base + SSPP_SW_PIX_EXT_C3_LR);
	writel(0, base + SSPP_SW_PIX_EXT_C3_TB);
	writel(0x8700438, base + SSPP_SW_PIX_EXT_C3_REQ_PIXELS);
	writel(0x3, base + SSPP_MULTIRECT_OPMODE);
	writel(0x80000000, base + SSPP_UBWC_ERROR_STATUS);
	writel(0x3FFFF, base + SSPP_DANGER_LUT);
	writel(0xFE00, base + SSPP_SAFE_LUT);
	writel(0x22335777, base + SSPP_CREQ_LUT_0);
	writel(0x112222, base + SSPP_CREQ_LUT_1);
	writel(0x1, base + SSPP_QOS_CTRL);
	writel(0x5, base + SSPP_CLK_CTRL);
}

static void mdss_vbif_setup(void *regs_vbif)
{
#if 0
	write32(vbif_rt->vbif_out_axi_amemtype_conf0, 0x33333333);
	write32(vbif_rt->vbif_out_axi_amemtype_conf1, 0x00333333);

	/*
	 * VBIF remapper registers are used for translating internal display hardware
	 * priority level (from 0 to 7) into system fabric priority level.
	 * These remapper settings are defined for all the clients which corresponds
	 * to the xin clients connected to SSPP on VBIF.
	 */
	write32(vbif_rt->qos_rp_remap[0].vbif_xinl_qos_rp_remap, 0x00000003);
	write32(vbif_rt->qos_rp_remap[1].vbif_xinl_qos_rp_remap, 0x11111113);
	write32(vbif_rt->qos_rp_remap[2].vbif_xinl_qos_rp_remap, 0x22222224);
	write32(vbif_rt->qos_rp_remap[3].vbif_xinl_qos_rp_remap, 0x33333334);
	write32(vbif_rt->qos_rp_remap[4].vbif_xinl_qos_rp_remap, 0x44444445);
	write32(vbif_rt->qos_rp_remap[7].vbif_xinl_qos_rp_remap, 0x77777776);
	write32(vbif_rt->qos_lvl_remap[0].vbif_xinl_qos_lvl_remap, 0x00000003);
	write32(vbif_rt->qos_lvl_remap[1].vbif_xinl_qos_lvl_remap, 0x11111113);
	write32(vbif_rt->qos_lvl_remap[2].vbif_xinl_qos_lvl_remap, 0x22222224);
	write32(vbif_rt->qos_lvl_remap[3].vbif_xinl_qos_lvl_remap, 0x33333334);
	write32(vbif_rt->qos_lvl_remap[4].vbif_xinl_qos_lvl_remap, 0x44444445);
	write32(vbif_rt->qos_lvl_remap[5].vbif_xinl_qos_lvl_remap, 0x77777776);
#endif
	u32 reg_lvl = 0x40;

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
}

static void mdss_intf_fetch_start_config(void *regs_base, struct dpu_mdss_cfg *cat)
{
#if 0
	uint32_t v_total, h_total, fetch_start, vfp_start;
	uint32_t prefetch_avail, prefetch_needed;
	uint32_t fetch_enable = PROG_FETCH_START_EN;

	/*
	 * MDP programmable fetch is for MDP with rev >= 1.05.
	 * Programmable fetch is not needed if vertical back porch
	 * plus vertical pulse width plus extra line for the extra h_total
	 * added during fetch start is >= 24.
	 */
	if ((edid->mode.vbl - edid->mode.vso + 1) >= MDSS_MDP_MAX_PREFILL_FETCH)
		return;

	/*
	 * Fetch should always be outside the active lines. If the fetching
	 * is programmed within active region, hardware behavior is unknown.
	 */
	v_total = edid->mode.va + edid->mode.vbl;
	h_total = edid->mode.ha + edid->mode.hbl;
	vfp_start = edid->mode.va + edid->mode.vbl - edid->mode.vso;
	prefetch_avail = v_total - vfp_start;
	prefetch_needed = MDSS_MDP_MAX_PREFILL_FETCH - edid->mode.vbl + edid->mode.vso;

	/*
	 * In some cases, vertical front porch is too high. In such cases limit
	 * the mdp fetch lines  as the last (25 - vbp - vpw) lines of
	 * vertical front porch.
	 */
	if (prefetch_avail > prefetch_needed)
		prefetch_avail = prefetch_needed;

	fetch_start = (v_total - prefetch_avail) * h_total + h_total + 1;
	write32(mdp_intf->intf_prof_fetch_start, fetch_start);
	write32(mdp_intf->intf_config, fetch_enable);
#endif

	void *base;
	/* assume the 1st ctl in active case of sm8650 */
	void *ctl_base = regs_base + cat->ctl->base;
	int i;

	/* config 2 DSI intf that's on 2nd and 3rd intf */
	for (i = 2; i < 4; i++) {
		base = regs_base + cat->intf[i].base;

		writel(0x3100014, base + INTF_HSYNC_CTL);
		writel(0x293960, base + INTF_VSYNC_PERIOD_F0);
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
		//writel(0x80800000, base + INTF_CONFIG);
		writel(0x213F, base + INTF_PANEL_FORMAT);
		writel(0x1011, base + INTF_CONFIG2);
		writel(0x18F0028, base + INTF_DISPLAY_DATA_HCTL);
		writel(0, base + INTF_ACTIVE_DATA_HCTL);
		writel(0x293031, base + INTF_PROG_FETCH_START);
		writel(0x80800000, base + INTF_CONFIG);
		writel(0x1, base + INTF_TIMING_ENGINE_EN);

		if (i == 2)
			writel(0xF0000, base + INTF_MUX);
		else /* intf 3 */
			writel(0xF0002, base + INTF_MUX);
	}

	writel(0xF0000000, ctl_base + CTL_TOP);
	/* enable 2 DSI interface */
	writel(0x6, ctl_base + CTL_INTF_ACTIVE);
	writel(0, ctl_base + CTL_WB_ACTIVE);
	writel(0xF, ctl_base + CTL_DSC_ACTIVE);
	writel(2, ctl_base + CTL_INTF_MASTER);
	writel(0x3, ctl_base + CTL_MERGE_3D_ACTIVE);
}

/* configure the mixer blending in ctl */
void mdp_setup_stage(struct dpu_mdss_cfg *cat, void *regs_base)
{
/*
	uint32_t ctl0_reg_val;

	ctl0_reg_val = VIG_0 | LAYER_MIXER_0 | CTL | INTF;
	write32(mdp_ctl->ctl_intf_flush, INTF_FLUSH);
	write32(mdp_ctl->ctl_flush, ctl0_reg_val);
*/
	void *base = regs_base + cat->ctl[0].base;

	for (int lm = 0; lm < PIPES_PER_PLANE; lm++) {
		if (lm < PIPES_PER_STAGE) {
			writel(0x1080000, base + CTL_LAYER(lm));
			writel(0x200, base + CTL_LAYER_EXT2(lm));
		} else {
			writel(0x1400000, base + CTL_LAYER(lm));
			writel(0x2000, base + CTL_LAYER_EXT2(lm));
		}
		writel(0, base + CTL_LAYER_EXT(lm));
		writel(0, base + CTL_LAYER_EXT3(lm));
		writel(0, base + CTL_LAYER_EXT4(lm));
	}
}

/**
 * _stage_offset(): returns the relative offset of the blend registers
 * for the stage to be setup
 * @ctx:     mixer ctx contains the mixer to be programmed
 * @stage: stage index to setup
 */
static inline int _stage_offset(struct dpu_lm_sub_blks *sblk, enum dpu_stage stage)
{
	if (stage != DPU_STAGE_BASE && stage <= sblk->maxblendstages)
		return sblk->blendstage_base[stage - DPU_STAGE_0];

	return -EINVAL;
}

static void mdss_layer_mixer_setup(void *regs_base, struct dpu_mdss_cfg *cat)
{
	uint32_t op_mode = 0x2;
#if 0
	uint32_t mdp_rgb_size;
	uint32_t left_staging_level;

	/* write active region size*/
	mdp_rgb_size = (edid->mode.va << 16) | edid->mode.ha;

	write32(mdp_layer_mixer->layer_out_size, mdp_rgb_size);
	write32(mdp_layer_mixer->layer_op_mode, 0x0);
	for (int i = 0; i < 6; i++) {
		write32(mdp_layer_mixer->layer_blend[i].layer_blend_op, 0x100);
		write32(mdp_layer_mixer->layer_blend[i].layer_blend_const_alpha, 0x00ff0000);
	}

	/* Enable border fill */
	left_staging_level = BORDER_OUT;
	left_staging_level |= VIG_0_OUT;

	/* Base layer for layer mixer 0 */
	write32(mdp_ctl->ctl_layer0, left_staging_level);
#endif
	//for (int i = 0; i < cat->mixer_count; i++) {
	for (int i = 0; i < PIPES_PER_PLANE; i++) {
		void *base = regs_base + cat->mixer[i].base;
		//struct dpu_lm_sub_blks *sblk = lm->sblk;
		//for (int stage = 0; i < sblk->maxblendstages; stage++) {
		for (int stage = 0; i < 1; stage++) {
			u32 stage_off = _stage_offset(cat->mixer[i].sblk, stage);
			if (WARN_ON(stage_off < 0))
				return;
			writel(0xFF0000, base + LM_BLEND0_CONST_ALPHA + stage_off);
			writel(0x100, base + LM_BLEND0_OP + stage_off);
		}
		writel(0x8700438, base + LM_OUT_SIZE);
		if (i & 1)
			op_mode |= BIT(31); /* split right half in stage */
		writel(op_mode, base + LM_OP_MODE);
	}
	mdp_setup_stage(cat, regs_base);
}

#if 0
void mdp_dsi_video_config(struct edid *edid)
{
	mdss_intf_tg_setup(edid);
	mdss_intf_fetch_start_config(edid);
	mdss_vbif_setup();
	mdss_vbif_qos_remapper_setup();
	mdss_sspp_config(edid);
	mdss_layer_mixer_setup(regs_base, );
	mdss_ctrl_config();
	write32(mdp_intf->intf_mux, 0x0F0000);
}
#endif

void mdp_dsc_setup(struct dpu_mdss_cfg *cat, void *regs_base)
{
	void *base;
	struct dpu_dsc_sub_blks *sblk;
	int i;

	for (i = 0; i < 4; i++) {
		base = regs_base + cat->dsc[i].base;
		sblk = cat->dsc[i].sblk;

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
		dpu_write(base + sblk->ctl.base + DSC_CTL, i);
	}
}

void mdp_pingpong_setup(struct dpu_mdss_cfg *cat, void *regs_base)
{
	void *base;
	int i;

	/* 2 pp map to 4 pipe in quad-pipe case */
	for (i = 0; i < 2; i++) {
		base = regs_base + cat->pingpong[i].sblk->dither.base;
		dpu_write(base + PP_DITHER_EN, 0);
	}
}

void mdp_dsi_video_on(struct dpu_mdss_cfg *cat, void *regs_base)
{
/*
	uint32_t ctl0_reg_val;

	ctl0_reg_val = VIG_0 | LAYER_MIXER_0 | CTL | INTF;
	write32(mdp_ctl->ctl_intf_flush, INTF_FLUSH);
	write32(mdp_ctl->ctl_flush, ctl0_reg_val);
*/
	void *ctl_base = regs_base + cat->ctl[0].base;

	writel(0x3, ctl_base + CTL_MERGE_3D_FLUSH);
	writel(0x6, ctl_base + CTL_INTF_FLUSH);
	writel(0xF, ctl_base + CTL_DSC_FLUSH);
	writel(0x80C21BC0, ctl_base + CTL_FLUSH);
	/* enable INTF_TIMING_ENGINE_EN of 2 dsi here in timing */
	writel(0x3, ctl_base + CTL_FETCH_PIPE_ACTIVE);
	writel(0x421BC0, ctl_base + CTL_FLUSH);
}

/* copy from rk_vop.c and stm32_ltdc.c
We can parse all sub-nodes here, to make parsing nodes and topology easier
with referencing bootdev_get_sibling_blk() to get sub-nodes device */
int qcom_dpu_probe(struct udevice *dev)
{
	struct video_uc_plat *uc_plat = dev_get_uclass_plat(dev);
	struct qcom_dpu_priv *priv = dev_get_priv(dev);
	struct video_priv *uc_priv = dev_get_uclass_priv(dev);
	struct clk_bulk bulk;
	struct udevice *bridge = NULL;
	struct udevice *panel = NULL;
	struct display_timing timings;
	const struct device_node *np;
	ulong rate;
	int ret, len;

printf("=========%s enter, FLG_RELOC? %d\n", __func__, gd->flags & GD_FLG_RELOC);
	/* Before relocation we don't need to do anything */
	if (!(gd->flags & GD_FLG_RELOC))
		return 0;

	priv->regs_base = dev_remap_addr_name(dev, "mdp");
	priv->vbif_base = dev_remap_addr_name(dev, "vbif");
	uc_plat->base = 0x9bb00000;

printf("=========%s line %d\n", __func__, __LINE__);
	ret = clk_get_bulk(dev, &bulk);
	if (ret) {
		dev_err(dev, "clk bulk get error %d\n", ret);
		return ret;
	}

	ret = clk_enable_bulk(&bulk);
	if (ret) {
		dev_err(dev, "clk bulk enable error %d\n", ret);
		return ret;
	}

/*
	rate = clk_set_rate(&pclk, timings.pixelclock.typ);
	if (IS_ERR_VALUE(rate))
		dev_warn(dev, "fail to set pixel clock %d hz, ret=%ld\n",
			 timings.pixelclock.typ, rate);
*/

	if (IS_ENABLED(CONFIG_VIDEO_BRIDGE)) {
		ret = uclass_get_device(UCLASS_VIDEO_BRIDGE, 0, &bridge);
		if (ret) {
			dev_dbg(dev,
				"No video bridge, err %d\n", ret);
			return ret;
		}

		if (bridge) {
			ret = video_bridge_attach(bridge);
			if (ret) {
				dev_err(bridge, "fail to attach bridge\n");
				return ret;
			}
		}
		np = ofnode_to_np(dev_ofnode(bridge));
		if (of_find_property(np, "qcom,dual-dsi-mode", &len)) {
			dev_dbg(bridge, "Dual DSI is found. To attach it\n");
			ret = uclass_get_device(UCLASS_VIDEO_BRIDGE, 1, &bridge);
			if (ret) {
				dev_dbg(dev,
					"No video bridge, err %d\n", ret);
				return ret;
			}

			if (bridge) {
				ret = video_bridge_attach(bridge);
				if (ret) {
					dev_err(bridge, "fail to attach bridge\n");
					return ret;
				}
			}
		}
	} else {
		dev_dbg(dev,
			"ERROR: Please enable video bridge in defconfig\n");
	}

printf("=========%s line %d\n", __func__, __LINE__);
	//ret = uclass_first_device_err(UCLASS_PANEL, &panel);
	//ret = uclass_find_first_device(UCLASS_PANEL, &panel);
	ret = uclass_get_device(UCLASS_PANEL, 0, &panel);
	if (ret) {
		dev_err(dev, "panel device error %d\n", ret);
		return ret;
	}

printf("=========%s line %d\n", __func__, __LINE__);
	ret = panel_get_display_timing(panel, &timings);
printf("=========%s line %d get panel timing ret %d\n", __func__, __LINE__, ret);
	if (ret) {
		ret = ofnode_decode_display_timing(dev_ofnode(panel),
						   0, &timings);
		if (ret) {
			dev_err(dev, "decode display timing error %d\n", ret);
			return ret;
		}
	}

printf("=========%s line %d\n", __func__, __LINE__);
	dev_dbg(dev, "Set pixel clock req %d hz get %ld hz\n",
		timings.pixelclock.typ, rate);


printf("=========%s line %d\n", __func__, __LINE__);
	dev_dbg(dev, "%dx%d %dbpp frame buffer at 0x%lx\n",
		timings.hactive.typ, timings.vactive.typ,
		VNBITS(VIDEO_BPP32), uc_plat->base);

	/* Configure & start DPU */

	uc_priv->bpix = VIDEO_BPP32;
	uc_priv->xsize = timings.hactive.typ;
	uc_priv->ysize = timings.vactive.typ;
	uc_priv->rot = 0;
//printf("=========%s uc_priv->bpix %d\n", __func__, VIDEO_BPP32);
printf("=========%s uc_priv->bpix %d, uc_priv 0x%x\n", __func__, uc_priv->bpix, uc_priv);


	/* Call panel api directly instead of via bridge, as we have 2 bridges */
	ret = panel_enable_backlight(panel);
	if (ret) {
		dev_err(dev, "panel %s enable backlight error %d\n",
			panel->name, ret);
		return ret;
	}

	video_set_flush_dcache(dev, true);
printf("=========%s exit ok\n", __func__);

#if 0
	struct rkvop_driverdata *ops =
		(struct rkvop_driverdata *)dev_get_driver_data(dev);
	ofnode port, node;

	priv->regs = dev_read_addr_ptr(dev);
	priv->win_offset = ops->win_offset;
	priv->dsp_offset = ops->dsp_offset;

	/*
	 * Try all the ports until we find one that works. In practice this
	 * tries EDP first if available, then HDMI.
	 *
	 * Note that rockchip_vop_set_clk() always uses NPLL as the source
	 * clock so it is currently not possible to use more than one display
	 * device simultaneously.
	 */
	port = dev_read_subnode(dev, "port");
	if (!ofnode_valid(port)) {
		debug("%s(%s): 'port' subnode not found\n",
		      __func__, dev_read_name(dev));
		return -EINVAL;
	}

	for (node = ofnode_first_subnode(port);
	     ofnode_valid(node);
	     node = dev_read_next_subnode(node)) {
		ret = rk_display_init(dev, plat->base, node);
		if (ret)
			debug("Device failed: ret=%d\n", ret);
		if (!ret)
			break;
	}
	video_set_flush_dcache(dev, 1);

	return ret;
#endif

	return 0;
}

#define LCD_MAX_WIDTH 4320
#define LCD_MAX_HEIGHT 2160
static int qcom_dpu_bind(struct udevice *dev)
{
	struct video_uc_plat *uc_plat = dev_get_uclass_plat(dev);

printf("=========%s called\n", __func__);
	uc_plat->size = (LCD_MAX_WIDTH * LCD_MAX_HEIGHT *
			((1 << VIDEO_BPP32) / 8));
	uc_plat->align = SZ_2M;
	dev_dbg(dev, "frame buffer max size %d bytes align %x\n",
		uc_plat->size, uc_plat->align);
printf("frame buffer max size %d bytes align %x\n", uc_plat->size, uc_plat->align);

	return 0;
}

static const struct udevice_id dpu_ids[] = {
	{ .compatible = "qcom,sm8650-dpu" },
	{ }
};

static const struct video_ops dpu_ops = {
};

U_BOOT_DRIVER(qcom_dpu) = {
	.name	= "qcom_dpu",
	.id	= UCLASS_VIDEO,
	.of_match = dpu_ids,
//	.ops	= &dpu_ops,
	.bind	= qcom_dpu_bind,
	.probe	= qcom_dpu_probe,
//        .remove = rk_vop_remove,
	.priv_auto	= sizeof(struct qcom_dpu_priv),
	.flags  = DM_FLAG_PRE_RELOC | DM_FLAG_LEAVE_PD_ON,
};
