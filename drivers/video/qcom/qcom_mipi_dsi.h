/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef _SOC_DISPLAY_MIPI_DSI_H_
#define _SOC_DISPLAY_MIPI_DSI_H_

#include <linux/types.h>

#include "phy/dsi_phy.h"

/**********************************************************
  DSI register configuration options
 **********************************************************/
#define MIPI_DSI_MRPS			0x04	/* Maximum Return Packet Size */
#define MIPI_DSI_REG_LEN		16	/* 4 x 4 bytes register */

#define DSI_VIDEO_DST_FORMAT_RGB565		0
#define DSI_VIDEO_DST_FORMAT_RGB666		1
#define DSI_VIDEO_DST_FORMAT_RGB666_LOOSE	2
#define DSI_VIDEO_DST_FORMAT_RGB888		3

#define MSM_DSI_VER_MAJOR_V2	0x02
#define MSM_DSI_VER_MAJOR_6G	0x03
#define MSM_DSI_6G_VER_MINOR_V1_0	0x10000000
#define MSM_DSI_6G_VER_MINOR_V1_0_2	0x10000002
#define MSM_DSI_6G_VER_MINOR_V1_1	0x10010000
#define MSM_DSI_6G_VER_MINOR_V1_1_1	0x10010001
#define MSM_DSI_6G_VER_MINOR_V1_2	0x10020000
#define MSM_DSI_6G_VER_MINOR_V1_3	0x10030000
#define MSM_DSI_6G_VER_MINOR_V1_3_1	0x10030001
#define MSM_DSI_6G_VER_MINOR_V1_4_1	0x10040001
#define MSM_DSI_6G_VER_MINOR_V1_4_2	0x10040002
#define MSM_DSI_6G_VER_MINOR_V2_1_0	0x20010000
#define MSM_DSI_6G_VER_MINOR_V2_2_0	0x20000000
#define MSM_DSI_6G_VER_MINOR_V2_2_1	0x20020001
#define MSM_DSI_6G_VER_MINOR_V2_3_0	0x20030000
#define MSM_DSI_6G_VER_MINOR_V2_3_1	0x20030001
#define MSM_DSI_6G_VER_MINOR_V2_4_0	0x20040000
#define MSM_DSI_6G_VER_MINOR_V2_4_1	0x20040001
#define MSM_DSI_6G_VER_MINOR_V2_5_0	0x20050000
#define MSM_DSI_6G_VER_MINOR_V2_5_1	0x20050001
#define MSM_DSI_6G_VER_MINOR_V2_6_0	0x20060000
#define MSM_DSI_6G_VER_MINOR_V2_7_0	0x20070000
#define MSM_DSI_6G_VER_MINOR_V2_8_0	0x20080000
#define MSM_DSI_6G_VER_MINOR_V2_9_0	0x20090000

#define DSI_6G_REG_SHIFT	4

enum {
	DSI_VIDEO_MODE,
	DSI_CMD_MODE,
};

struct msm_dsi_cfg_handler {
	u32 major;
	u32 minor;
};

#define FIELD(val, name) (((val) & name ## __MASK) >> name ## __SHIFT)

//enum cb_err mdss_dsi_config(struct edid *edid, uint32_t num_of_lanes, uint32_t bpp);
void mdss_dsi_clock_config(void);
//void mdss_dsi_video_mode_config(struct edid *edid, uint32_t bpp);
enum cb_err mdss_dsi_panel_initialize(const u8 *init_cmds);

int msm_dsi_phy_enable(struct msm_dsi_phy *phy,
			struct msm_dsi_phy_clk_request *clk_req,
			struct msm_dsi_phy_shared_timings *shared_timings);
void msm_dsi_phy_disable(struct msm_dsi_phy *phy);
void msm_dsi_phy_set_usecase(struct msm_dsi_phy *phy,
			     enum msm_dsi_phy_usecase uc);
void msm_dsi_phy_pll_save_state(struct msm_dsi_phy *phy);
int msm_dsi_phy_pll_restore_state(struct msm_dsi_phy *phy);
// void msm_dsi_phy_snapshot(struct msm_disp_state *disp_state, struct msm_dsi_phy *phy);
bool msm_dsi_phy_set_continuous_clock(struct msm_dsi_phy *phy, bool enable);

/* Poll error interrupts in video_sync cb */
void qcom_dsi_video_sync(struct udevice *dsi_bridge);

#endif
