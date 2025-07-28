// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2025 Linaro.Ltd - All Rights Reserved
 *
 */

#define DEBUG
#define LOG_CATEGORY UCLASS_VIDEO_BRIDGE

#include <clk.h>
#include <dm.h>
#include <dsi_host.h>
#include <log.h>
#include <generic-phy.h>
#include <mipi_dsi.h>
#include <panel.h>
#include <phy-mipi-dphy.h>
#include <reset.h>
#include <video.h>
#include <video_bridge.h>
#include <asm/io.h>
#include <dm/device-internal.h>
#include <dm/device_compat.h>
#include <dm/lists.h>
#include <linux/bitops.h>
#include <linux/iopoll.h>
#include <linux/printk.h>
#include <power/regulator.h>

#include "mdssreg.h"
#include "qcom_mipi_dsi.h"
//#include "qcom_dsi_phy.h"

#define DSI_DMA_STREAM1 0x0
#define DSI_EMBED_MODE1 0x1
#define DSI_POWER_MODE2 0x1
#define DSI_PACK_TYPE1 0x0
#define DSI_VC1 0x0
#define DSI_DT1 0x0
#define DSI_WC1 0x0
#define DSI_EOF_BLLP_PWR 0x9
#define DSI_DMA_TRIGGER_SEL 0x4
#define TRAFFIC_MODE 0x1

#define DSI_EN 0x1
#define DSI_CLKLN_EN 0x1
#define DSI_VIDEO_EN 0x1

#define HS_TX_TO 0xEA60
#define TIMER_RESOLUTION 0x4
#define DSI_PAYLOAD_BYTE_BOUND 256
#define DSI_PAYLOAD_SIZE_ALIGN 4
#define DSI_CMD_DMA_TPG_EN  BIT(1)
#define DSI_TPG_DMA_FIFO_MODE BIT(2)
#define DSI_CMD_DMA_PATTERN_SEL (BIT(16) | BIT(17))

/* for bridge */
struct qcom_dsi_priv {
	struct mipi_dsi_device device;
	void __iomem *base;
	void __iomem *phy_base;
	struct udevice *panel;
	struct phy phy;
	struct phy_configure_opts_mipi_dphy phy_opts;
	u32 pllref_clk;
	u32 hw_version;
	int lane_min_kbps;
	int lane_max_kbps;
	unsigned int lane_mbps; /* per lane */
	struct udevice *vdd_reg;
	struct udevice *dsi_host;
};

/* for dsi host */
struct qcom_mipi_dsi {
	struct mipi_dsi_host dsi_host;
	struct mipi_dsi_device *device;
	void __iomem *base;
	unsigned int lane_mbps; /* per lane */
	u32 channel;
	unsigned int max_data_lanes;
	const struct mipi_dsi_phy_ops *phy_ops;
};

static struct qcom_dsi_priv_global {
	struct qcom_dsi_priv *priv0;
	struct qcom_dsi_priv *priv1;
	bool dual_dsi;
} qcom_dsi_global;

static inline struct qcom_mipi_dsi *host_to_dsi(struct mipi_dsi_host *host)
{
	return container_of(host, struct qcom_mipi_dsi, dsi_host);
}

static inline void dsi_write(void *addr, u32 val)
{
	writel(val, addr);
}

static inline u32 dsi_read(void *addr)
{
	return readl(addr);
}

#if 0
static inline void dsi_set(void *addr, u32 mask)
{
	dsi_write(addr, dsi_read(addr) | mask);
}

static void mdss_dsi_host_init(struct dsi_regs *dsi_reg, int num_of_lanes)
{
	uint8_t dlnx_en;
	uint32_t ctrl_mode = BIT(8) | BIT(0); /* Enable DSI and CLKlane. */

	switch (num_of_lanes) {
	default:
	case 1:
		dlnx_en = 1;
		break;

	case 2:
		dlnx_en = 3;
		break;

	case 3:
		dlnx_en = 7;
		break;

	case 4:
		dlnx_en = 0x0F;
		break;
	}

	/*
	 * Need to send pixel data before sending the ON commands
	 * so need to configure controller to VIDEO MODE.
	 */
	ctrl_mode |= BIT(1);

	mdss_dsi_clock_config(dsi_reg);
	dsi_write(dsi_reg->trig_ctrl, DSI_DMA_STREAM1 << 8 | DSI_DMA_TRIGGER_SEL);
	dsi_write(dsi_reg->ctrl, dlnx_en << 4 | ctrl_mode);
	dsi_write(dsi_reg->cmd_mode_dma_ctrl,
		DSI_EMBED_MODE1 << 28 | DSI_POWER_MODE2 << 26 |
		DSI_PACK_TYPE1 << 24 | DSI_VC1 << 22 | DSI_DT1 << 16 | DSI_WC1);
	dsi_write(dsi_reg->eot_packet_ctrl, 0x1);
}

static void mdss_dsi_reset(void)
{
	/*
	 * Disable DSI Controller, DSI lane states,
	 * DSI command-mode and DSI video-mode engines
	 */
	dsi_write(dsi_reg->ctrl, 0x0);

	/* DSI soft reset */
	dsi_write(dsi_reg->soft_reset, 0x1);
	dsi_write(dsi_reg->soft_reset, 0x0);

	/* set hs timer count speed */
	dsi_write(dsi_reg->hs_timer_ctrl, HS_TX_TO | TIMER_RESOLUTION << 16);

	/* dma fifo reset */
	dsi_write(dsi_reg->tpg_dma_fifo_reset, 0x1);
	dsi_write(dsi_reg->tpg_dma_fifo_reset, 0x0);
}

void mdss_dsi_video_mode_config(struct display_timing *timings, uint32_t bpp)
{
	u32 hs_start = 0, vs_start = 0; /* take sync start as 0 */
	u32 h_total = timing->hback_porch.typ + timing->hsync_len.typ +
		timing->hactive.typ + timing->hfront_porch.typ;
	u32 v_total = timing->vback_porch.typ + timing->vsync_len.typ +
		timing->vactive.typ + timing->vfront_porch.typ;
	u32 hs_end = timing->hsync_len.typ;
	u32 vs_end = timing->vsync_len.typ;
	u32 ha_start = timing->hback_porch.typ + timing->hsync_len.typ;
	u32 ha_end = ha_start + timing->hactive.typ;
	u32 va_start = timing->vback_porch.typ + timing->vsync_len.typ;
	u32 va_end = va_start + timing->vactive.typ;
	u32 hdisplay = timing->hactive.typ;
	uint16_t dst_format;
	uint8_t lane_en = 15; /* Enable 4 lanes by default */

	switch (bpp) {
	case 16:
		dst_format = DSI_VIDEO_DST_FORMAT_RGB565;
		break;
	case 18:
		dst_format = DSI_VIDEO_DST_FORMAT_RGB666;
		break;
	case 24:
	default:
		dst_format = DSI_VIDEO_DST_FORMAT_RGB888;
		break;
	}

/* TODO:
	if (is_bonded_dsi) {
		h_total /= 2;
		hs_end /= 2;
		ha_start /= 2;
		ha_end /= 2;
		hdisplay /= 2;
	}
*/

	dsi_write(dsi_reg->video_mode_active_h, (ha_end << 16) | ha_start);
	dsi_write(dsi_reg->video_mode_active_v, (va_end << 16) | va_start);
	dsi_write(dsi_reg->video_mode_active_total,
		((v_total  - 1) << 16) | (h_total - 1));

	dsi_write(dsi_reg->video_mode_active_hsync, hs_start | (hs_end << 16));
	dsi_write(dsi_reg->video_mode_active_vsync, 0x0);
	dsi_write(dsi_reg->video_mode_active_vsync_vpos, vs_start | (vs_end << 16));

	dsi_write(dsi_reg->video_mode_ctrl,
			DSI_EOF_BLLP_PWR << 12 | dst_format << 4 | TRAFFIC_MODE << 8);

/*
	dsi_write(dsi_reg->hs_timer_ctrl, HS_TX_TO | TIMER_RESOLUTION << 16);
*/

	dsi_write(dsi_reg->ctrl, lane_en << 4 | DSI_VIDEO_EN << 1 | DSI_EN | DSI_CLKLN_EN << 8);
}

int mdss_dsi_config(struct display_timing *timings, uint32_t num_of_lanes, uint32_t bpp)
{
	mdss_dsi_reset();
	if ((mdss_dsi_phy_10nm_init(timings, num_of_lanes, bpp)) != 0) {
		printk(BIOS_ERR, "dsi phy setup returned error\n");
		return CB_ERR;
	}

	mdss_dsi_host_init(num_of_lanes);

	return CB_SUCCESS;
}

void mdss_dsi_clock_config(struct dsi_regs *dsi_reg)
{
	/* Clock for AHI Bus Master, for DMA out from memory */
	dsi_write(dsi_reg->clk_ctrl, 0);
	dsi_set(dsi_reg->clk_ctrl, DSI_AHBM_SCLK_ON | DSI_FORCE_ON_DYN_AHBM_HCLK);

	/* Clock for MDP/DSI, for DMA out from MDP */
	dsi_set(dsi_reg->clk_ctrl, DSI_PCLK_ON);

	/* Clock for rest of DSI */
	dsi_set(dsi_reg->clk_ctrl, DSI_AHBS_HCLK_ON | DSI_DSICLK_ON |
		  DSI_BYTECLK_ON | DSI_ESCCLK_ON);
}

static void mdss_dsi_set_intr(void)
{
	dsi_write(dsi_reg->int_ctrl, 0x0);

	/* Enable all HW interrupts. */
	dsi_set(dsi_reg->int_ctrl, DSI_CMD_MODE_DMA_DONE_MASK | DSI_CMD_MODE_MDP_DONE_MASK |
			DSI_VIDEO_MODE_DONE_MASK | DSI_ERROR_MASK | DSI_BTA_DONE_MASK);
}

static int mdss_dsi_cmd_dma_trigger_for_panel(struct dsi_regs *dsi_reg)
{
	uint32_t read_value;
	uint32_t count = 0;
	int status = 0;

	mdss_dsi_set_intr(dsi_reg);
	dsi_write(dsi_reg->cmd_mode_dma_sw_trigger, 0x1);
	dsb();

	read_value = dsi_read(dsi_reg->int_ctrl) & 0x1;

	while (read_value != 0x1) {
		read_value = dsi_read(dsi_reg->int_ctrl) & 0x1;
		count++;
		if (count > 0xffff) {
			status = -1;
			printk(BIOS_ERR,
				"Panel CMD: count :%d command mode dma test failed\n", count);
			printk(BIOS_ERR,
			       "Panel CMD: read value = %x, addr=%p\n",
			       read_value, (dsi_reg->int_ctrl));
			return status;
		}
	}

	dsi_write(dsi_reg->int_ctrl, (dsi_read(dsi_reg->int_ctrl) | 0x01000001));
	return status;
}

static int mdss_dsi_send_init_cmd(enum mipi_dsi_transaction type, const u8 *body,
					  u8 len)
{
	uint8_t *pload = _dma_coherent;
	uint32_t size;
	int ret = CB_SUCCESS;
	int data = 0;
	uint32_t *bp = NULL;

	if (len > 2) {
		pload[0] = len;
		pload[1] = 0;
		pload[2] = type;
		pload[3] = BIT(7) | BIT(6);

		/* The payload size has to be a multiple of 4 */
		memcpy(pload + 4, body, len);
		size = ALIGN_UP(len + 4, DSI_PAYLOAD_SIZE_ALIGN);
		memset(pload + 4 + len, 0, size - 4 - len);
		assert(size < DSI_PAYLOAD_BYTE_BOUND);
	} else {
		pload[0] = body[0];
		pload[1] = len > 1 ? body[1] : 0;
		pload[2] = type;
		pload[3] = BIT(7);
		size = 4;
	}

	bp = (uint32_t *)pload;

	/* Enable custom pattern stored in TPG DMA FIFO */
	data = DSI_CMD_DMA_PATTERN_SEL;

	/* select CMD_DMA_FIFO_MODE to 1 */
	data |= DSI_TPG_DMA_FIFO_MODE;
	data |= DSI_CMD_DMA_TPG_EN;

	dsi_write(dsi_reg->test_pattern_gen_ctrl, data);
	for (int j = 0; j < size; j += 4) {
		dsi_write(dsi_reg->test_pattern_gen_cmd_dma_init_val, *bp);
		bp++;
	}

	if ((size % 8) != 0)
		dsi_write(dsi_reg->test_pattern_gen_cmd_dma_init_val, 0x0);

	dsi_write(dsi_reg->dma_cmd_length, size);
	dsi_write(dsi_reg->cmd_mode_dma_sw_trigger, 0x1);
	if (mdss_dsi_cmd_dma_trigger_for_panel())
		ret = CB_ERR;

	/* Reset the DMA TPG FIFO */
	dsi_write(dsi_reg->tpg_dma_fifo_reset, 0x1);
	dsi_write(dsi_reg->tpg_dma_fifo_reset, 0x0);

	/* Disable CMD_DMA_TPG */
	dsi_write(dsi_reg->test_pattern_gen_ctrl, 0x0);

	udelay(80);

	return ret;
}

static void mdss_dsi_clear_intr(struct dsi_regs *dsi_reg)
{
	dsi_write(dsi_reg->int_ctrl, 0x0);

	/* Clear all the hardware interrupts */
	setbits32(dsi_reg->int_ctrl, DSI_CMD_MODE_DMA_DONE_AK | DSI_CMD_MODE_MDP_DONE_AK |
			DSI_VIDEO_MODE_DONE_AK |  DSI_BTA_DONE_AK | DSI_ERROR_AK);
	dsi_write(dsi_reg->err_int_mask0, 0x13FF3BFF);
}

int mdss_dsi_panel_initialize(struct dsi_regs *dsi_reg, const u8 *init_cmds)
{
	uint32_t ctrl_mode = 0;

	assert(init_cmds != NULL);
	ctrl_mode = dsi_read(dsi_reg->ctrl);

	/* Enable command mode before sending the commands */
	dsi_write(dsi_reg->ctrl, ctrl_mode | 0x04);

/* TODO: send cmds in panel driver
	int ret = mipi_panel_parse_init_commands(init_cmds, mdss_dsi_send_init_cmd);
*/
	dsi_write(dsi_reg->ctrl, ctrl_mode);
	mdss_dsi_clear_intr(dsi_reg);

	return ret;
}


/////////////////////////////////////////////////////////////



static enum dsi_color dsi_color_from_mipi(u32 fmt)
{
	switch (fmt) {
	case MIPI_DSI_FMT_RGB888:
		return DSI_RGB888;
	case MIPI_DSI_FMT_RGB666:
		return DSI_RGB666_CONF2;
	case MIPI_DSI_FMT_RGB666_PACKED:
		return DSI_RGB666_CONF1;
	case MIPI_DSI_FMT_RGB565:
		return DSI_RGB565_CONF1;
	default:
		log_err("MIPI color invalid, so we use rgb888\n");
	}
	return DSI_RGB888;
}

static int dsi_pll_get_clkout_khz(int clkin_khz, int idf, int ndiv, int odf)
{
	int divisor = idf * odf;

	/* prevent from division by 0 */
	if (!divisor)
		return 0;

	return DIV_ROUND_CLOSEST(clkin_khz * ndiv, divisor);
}

static int dsi_pll_get_params(struct qcom_dsi_priv *dsi,
			      int clkin_khz, int clkout_khz,
			      int *idf, int *ndiv, int *odf)
{
	int i, o, n, n_min, n_max;
	int fvco_min, fvco_max, delta, best_delta; /* all in khz */

	/* Early checks preventing division by 0 & odd results */
	if (clkin_khz <= 0 || clkout_khz <= 0)
		return -EINVAL;

	fvco_min = dsi->lane_min_kbps * 2 * ODF_MAX;
	fvco_max = dsi->lane_max_kbps * 2 * ODF_MIN;

	best_delta = 1000000; /* big started value (1000000khz) */

	for (i = IDF_MIN; i <= IDF_MAX; i++) {
		/* Compute ndiv range according to Fvco */
		n_min = ((fvco_min * i) / (2 * clkin_khz)) + 1;
		n_max = (fvco_max * i) / (2 * clkin_khz);

		/* No need to continue idf loop if we reach ndiv max */
		if (n_min >= NDIV_MAX)
			break;

		/* Clamp ndiv to valid values */
		if (n_min < NDIV_MIN)
			n_min = NDIV_MIN;
		if (n_max > NDIV_MAX)
			n_max = NDIV_MAX;

		for (o = ODF_MIN; o <= ODF_MAX; o *= 2) {
			n = DIV_ROUND_CLOSEST(i * o * clkout_khz, clkin_khz);
			/* Check ndiv according to vco range */
			if (n < n_min || n > n_max)
				continue;
			/* Check if new delta is better & saves parameters */
			delta = dsi_pll_get_clkout_khz(clkin_khz, i, n, o) -
				clkout_khz;
			if (delta < 0)
				delta = -delta;
			if (delta < best_delta) {
				*idf = i;
				*ndiv = n;
				*odf = o;
				best_delta = delta;
			}
			/* fast return in case of "perfect result" */
			if (!delta)
				return 0;
		}
	}

	return 0;
}
#endif


static int qcom_mipi_dsi_host_attach(struct mipi_dsi_host *host,
				   struct mipi_dsi_device *device)
{
	struct qcom_mipi_dsi *dsi = host_to_dsi(host);

	if (device->lanes > dsi->max_data_lanes) {
		dev_err(device->dev,
			"the number of data lanes(%u) is too many\n",
			device->lanes);
		return -EINVAL;
	}

	dsi->channel = device->channel;

	return 0;
}

static ssize_t qcom_mipi_dsi_host_transfer(struct mipi_dsi_host *host,
					 const struct mipi_dsi_msg *msg)
{
	struct qcom_mipi_dsi *dsi = host_to_dsi(host);
	struct mipi_dsi_packet packet;
	int ret, nb_bytes;

printf("======%s tx %d rx %d with base %p\n", __func__, msg->tx_len, msg->rx_len, dsi->base);
#if 0
	ret = mipi_dsi_create_packet(&packet, msg);
	if (ret) {
		dev_err(host->dev, "failed to create packet: %d\n", ret);
		return ret;
	}

	qcom_mipi_message_config(dsi, msg);

	ret = qcom_mipi_dsi_write(dsi, &packet);
	if (ret)
		return ret;

	if (msg->rx_buf && msg->rx_len) {
		ret = qcom_mipi_dsi_read(dsi, msg);
		if (ret)
			return ret;
		nb_bytes = msg->rx_len;
	} else {
		nb_bytes = packet.size;
	}

#endif
	return nb_bytes;
}

static const struct mipi_dsi_host_ops qcom_mipi_dsi_host_ops = {
	.attach = qcom_mipi_dsi_host_attach,
	.transfer = qcom_mipi_dsi_host_transfer,
};


static int qcom_mipi_dsi_init(struct udevice *dev,
			    struct mipi_dsi_device *device,
			    struct display_timing *timings,
			    unsigned int max_data_lanes,
			    const struct mipi_dsi_phy_ops *phy_ops)
{
	struct qcom_mipi_dsi *dsi = dev_get_priv(dev);
	//const struct mipi_dsi_phy_ops *phy_ops = dsi->phy_ops;
	struct clk clk;
	int ret;

	dev_dbg(dev, "Initialize DSI host\n");
	if (!phy_ops->init || !phy_ops->get_lane_mbps) {
		dev_err(device->dev, "Phy not properly configured\n");
		return -ENODEV;
	}

	dsi->phy_ops = phy_ops;
	dsi->max_data_lanes = max_data_lanes;
	dsi->device = device;
	dsi->dsi_host.dev = (struct device *)dev;
	dsi->dsi_host.ops = &qcom_mipi_dsi_host_ops;
	device->host = &dsi->dsi_host;

	dsi->base = dev_read_addr_ptr(device->dev);
	if (!dsi->base) {
		dev_err(device->dev, "dsi dt register address error\n");
		return -EINVAL;
	}

#if 0
	ret = clk_get_by_name(device->dev, "px_clk", &clk);
	if (ret) {
		dev_err(device->dev, "peripheral clock get error %d\n", ret);
		return ret;
	}

	/*  get the pixel clock set by the clock framework */
	timings->pixelclock.typ = clk_get_rate(&clk);

	qcom_mipi_dsi_bridge_set(dsi, timings);
#endif

/*
	ret = phy_ops->get_lane_mbps(device, timings, device->lanes,
				     device->format, &dsi->lane_mbps);
	if (ret)
		dev_warn(dsi->dsi_host.dev, "Phy get_lane_mbps() failed\n");
*/
	ret = phy_ops->init(device);

	return 0;
}

static int qcom_mipi_dsi_enable(struct udevice *dev)
{
	struct qcom_mipi_dsi *dsi = dev_get_priv(dev);

	dev_dbg(dev, "Enable DSI host\n");
	/* Switch to video mode for panel-bridge enable & panel enable */
	//qcom_mipi_dsi_set_mode(dsi, MIPI_DSI_MODE_VIDEO);

	return 0;
}

struct dsi_host_ops qcom_mipi_dsi_ops = {
	.init = qcom_mipi_dsi_init,
	.enable = qcom_mipi_dsi_enable,
};

static int qcom_mipi_dsi_probe(struct udevice *dev)
{
	dev_dbg(dev, "DSI host probe\n");
	return 0;
}

U_BOOT_DRIVER(qcom_mipi_dsi) = {
	.name			= "qcom_mipi_dsi",
	.id			= UCLASS_DSI_HOST,
	.probe			= qcom_mipi_dsi_probe,
	.ops			= &qcom_mipi_dsi_ops,
	.priv_auto		= sizeof(struct qcom_mipi_dsi),
};





static int dsi_phy_init(void *priv_data)
{
	struct mipi_dsi_device *device = priv_data;
	struct udevice *dev = device->dev;
	struct qcom_dsi_priv *dsi = dev_get_priv(dev);
	u32 val;
	int ret;

	dev_dbg(dev, "Initialize DSI physical layer\n");

	if (!generic_phy_valid(&dsi->phy)) {
		dev_dbg(dev, "fail to validate phy, phy %p phy->dev %p\n", &dsi->phy, dsi->phy.dev);
	}

	ret = generic_phy_power_on(&dsi->phy);
	if (ret) {
		dev_dbg(dev, "fail to power on phy\n");
	}

#if 0
	/* Enable the regulator */
	dsi_set(dsi, DSI_WRPCR, WRPCR_REGEN | WRPCR_BGREN);
	ret = readl_poll_timeout(dsi->base + DSI_WISR, val, val & WISR_RRS,
				 TIMEOUT_US);
	if (ret) {
		dev_dbg(dev, "!TIMEOUT! waiting REGU\n");
		return ret;
	}

	/* Enable the DSI PLL & wait for its lock */
	dsi_set(dsi, DSI_WRPCR, WRPCR_PLLEN);
	ret = readl_poll_timeout(dsi->base + DSI_WISR, val, val & WISR_PLLLS,
				 TIMEOUT_US);
	if (ret) {
		dev_dbg(dev, "!TIMEOUT! waiting PLL\n");
		return ret;
	}
#endif

	return 0;
}

static void dsi_phy_post_set_mode(void *priv_data, unsigned long mode_flags)
{
	struct mipi_dsi_device *device = priv_data;
	struct udevice *dev = device->dev;
	struct qcom_dsi_priv *dsi = dev_get_priv(dev);

	dev_dbg(dev, "Set mode %p enable %ld\n", dsi,
		mode_flags & MIPI_DSI_MODE_VIDEO);

	if (!dsi)
		return;

	/*
	 * DSI wrapper must be enabled in video mode & disabled in command mode.
	 * If wrapper is enabled in command mode, the display controller
	 * register access will hang.
	 */

/*
	if (mode_flags & MIPI_DSI_MODE_VIDEO)
		dsi_set(dsi, DSI_WCR, WCR_DSIEN);
	else
		dsi_clear(dsi, DSI_WCR, WCR_DSIEN);
*/
}

static int dsi_get_lane_mbps(void *priv_data, struct display_timing *timings,
			     u32 lanes, u32 format, unsigned int *lane_mbps)
{
	struct mipi_dsi_device *device = priv_data;
	struct udevice *dev = device->dev;
	struct qcom_dsi_priv *dsi = dev_get_priv(dev);
	unsigned long mpclk, tmp;
	int idf, ndiv, odf, pll_in_khz, pll_out_khz;
	int ret, bpp;
	unsigned int max_mbps, target_mbps = 1000;

	*lane_mbps = pll_out_khz / 1000;

	dev_dbg(dev, "pll_in %ukHz pll_out %ukHz lane_mbps %uMHz\n",
		pll_in_khz, pll_out_khz, *lane_mbps);

	bpp = mipi_dsi_pixel_format_to_bpp(format);
	if (bpp < 0) {
		dev_err(dsi->dsi_host,
			"failed to get bpp for pixel format %d\n",
			format);
		return bpp;
	}

	mpclk = DIV_ROUND_UP(timings->pixelclock.typ, 1000);
	if (mpclk) {
		/* take 1 / 0.8, since mbps must big than bandwidth of RGB */
		tmp = (mpclk * (bpp / lanes) * 10 / 8) / 1000;
		if (tmp < max_mbps)
			target_mbps = tmp;
		else
			dev_err(dsi->dsi_host,
				"DPHY clock frequency is out of range\n");
	}

	/* for external phy only the mipi_dphy_config is necessary */
	if (generic_phy_valid(&dsi->phy)) {
		/* need to switch a cphy version ?? required clk rate is calculated
		 * here, then the clk is enabled in generic phy power on */
/*
		phy_mipi_dphy_get_default_config(timings->pixelclock.typ  * 10 / 8,
						 bpp, lanes,
						 &dsi->phy_opts);
*/
		dsi->lane_mbps = target_mbps;
		*lane_mbps = dsi->lane_mbps;

		return 0;
	}

	return 0;
}

static const struct mipi_dsi_phy_ops mdss_dsi_phy_ops = {
	.init = dsi_phy_init,
	.get_lane_mbps = dsi_get_lane_mbps,
	.post_set_mode = dsi_phy_post_set_mode,
};

static int qcom_dsi_attach(struct udevice *dev)
{
	struct qcom_dsi_priv *priv = dev_get_priv(dev);
	struct mipi_dsi_device *device = &priv->device;
	struct mipi_dsi_panel_plat *mplat;
	struct display_timing timings;
	int ret, host_index;

printf("------------%s enter\n", __func__);
	ret = uclass_first_device_err(UCLASS_PANEL, &priv->panel);
	if (ret) {
		dev_err(dev, "panel device error %d\n", ret);
		return ret;
	}

	mplat = dev_get_plat(priv->panel);
	device->lanes = mplat->lanes;
	device->format = mplat->format;
	device->mode_flags = mplat->mode_flags;

	if (device->index) {
printf("------------%s dsi device 2: %p\n", __func__, &priv->device);
		mplat->device2 = &priv->device;
		qcom_dsi_global.priv1 = priv;
		qcom_dsi_global.dual_dsi = true;
		host_index = 1;
	} else {
printf("------------%s dsi device 0: %p\n", __func__, &priv->device);
		mplat->device = &priv->device;
		qcom_dsi_global.priv0 = priv;
		host_index = 0;
	}

	ret = panel_get_display_timing(priv->panel, &timings);
	if (ret) {
		ret = ofnode_decode_display_timing(dev_ofnode(priv->panel),
						   0, &timings);
		if (ret) {
			dev_err(dev, "decode display timing error %d\n", ret);
			return ret;
		}
	}

printf("------------%s line %d\n", __func__, __LINE__);
	ret = uclass_get_device(UCLASS_DSI_HOST, host_index, &priv->dsi_host);
	if (ret) {
		dev_err(dev, "No video dsi host detected %d\n", ret);
		return ret;
	}

printf("------------%s line %d call dsi_host_init, while qcom_mipi_dsi_init is %p\n", __func__, __LINE__, qcom_mipi_dsi_init);
	ret = dsi_host_init(priv->dsi_host, device, &timings, 3,
			    &mdss_dsi_phy_ops);
	if (ret) {
		dev_err(dev, "failed to initialize mipi dsi host\n");
		return ret;
	}

printf("------------%s exit ok\n", __func__);
	return 0;
}

static int qcom_dsi_set_backlight(struct udevice *dev, int percent)
{
	struct qcom_dsi_priv *priv = dev_get_priv(dev);
	struct mipi_dsi_host *dsi_host;
	int ret;

	dev_err(dev, "panel %s backlight enabling skipped to avoid panic for temp\n", priv->panel->name);
//	return 0;

	ret = panel_enable_backlight(priv->panel);
	if (ret) {
		dev_err(dev, "panel %s enable backlight error %d\n",
			priv->panel->name, ret);
		return ret;
	}

	/// FIXME: CASEY: dsi_host_enable() expects struct udevice not mipi_dsi_host?
	// dsi_host = &qcom_dsi_global.priv0->dsi_host;
	// ret = dsi_host_enable(dsi_host->dev);
	// if (ret) {
	// 	dev_err(dev, "failed to enable mipi dsi host\n");
	// 	return ret;
	// }

	// if(!qcom_dsi_global.priv1)
	// 	return 0;
	// dsi_host = &qcom_dsi_global.priv1->dsi_host;
	// ret = dsi_host_enable(dsi_host->dev);
	// if (ret) {
	// 	dev_err(dev, "failed to enable mipi dsi host\n");
	// 	return ret;
	// }

	return 0;
}

static int qcom_dsi_bind(struct udevice *dev)
{
	int ret;

printf("=============%s enter\n", __func__);
	ret = device_bind_driver_to_node(dev, "qcom_mipi_dsi", "mdss_dsi",
					 dev_ofnode(dev), NULL);
printf("=============%s result %d, will scan panel next\n", __func__, ret);
	if (ret)
		return ret;

	return dm_scan_fdt_dev(dev);
}

static int qcom_dsi_probe(struct udevice *dev)
{
	struct qcom_dsi_priv *priv = dev_get_priv(dev);
	struct mipi_dsi_device *device = &priv->device;
	struct reset_ctl rst;
	struct clk_bulk bulk;
	struct dsi_regs *dsi_reg;
	int ret;

	device->dev = dev;

printf("=============%s enter\n", __func__);
	priv->base = dev_read_addr_ptr(dev);
	dsi_reg = priv->base;
	if (!priv->base) {
		dev_err(dev, "dsi dt register address error\n");
		return -EINVAL;
	}

	ret = ofnode_read_u32_default(dev_ofnode(dev), "dual-dsi-index", -1);
	if (ret == 0 || ret == 1)
		device->index = ret;
	else
		return ret;
printf("=============%s line %d device->index %d\n", __func__, __LINE__, device->index);

	ret = generic_phy_get_by_name(dev, "dsi", &priv->phy);
	if (ret && ret != -ENODATA) {
		dev_err(dev, "failed to get mipi dphy: %d\n", ret);
		return ret;
	}

	if (!generic_phy_valid(&priv->phy)) {
		dev_dbg(dev, "fail to validate phy, phy %p phy->dev %p\n", &priv->phy, priv->phy.dev);
	}

/*
	ret =  device_get_supply_regulator(dev, "phy-dsi-supply",
					   &priv->vdd_reg);
	if (ret && ret != -ENOENT) {
		dev_err(dev, "Warning: cannot get phy dsi supply\n");
		return -ENODEV;
	}

	if (ret != -ENOENT) {
		ret = regulator_set_enable(priv->vdd_reg, true);
		if (ret)
			return ret;
	}

	ret = clk_get_bulk(device->dev, &bulk);
	if (ret) {
		dev_err(dev, "peripheral clock get error %d\n", ret);
		goto err_reg;
	}

	ret = clk_enable_bulk(&bulk);
	if (ret) {
		dev_err(dev, "peripheral clock enable error %d\n", ret);
		goto err_reg;
	}
*/

#if 0
	ret = reset_get_by_index(device->dev, 0, &rst);
	if (ret) {
		dev_err(dev, "missing dsi hardware reset\n");
		goto err_clk;
	}

	/* Reset */
	reset_deassert(&rst);
#endif

	/* check hardware version */
	priv->hw_version = dsi_read(dsi_reg);
	if (priv->hw_version != 0x20080000) {
		dev_err(dev, "DSI version 0x%x not supported\n", priv->hw_version);
		dev_dbg(dev, "remove and unbind all DSI child\n");
		device_chld_remove(dev, NULL, DM_REMOVE_NORMAL);
		device_chld_unbind(dev, NULL);
		ret = -ENODEV;
		goto err_clk;
	}

printf("=============%s ret 0\n", __func__);
	return 0;
err_clk:
	clk_disable_bulk(&bulk);
err_reg:
//	regulator_set_enable(priv->vdd_reg, false);

printf("=============%s ret %d\n", __func__, ret);
	return ret;
}

struct video_bridge_ops qcom_dsi_ops = {
	.attach = qcom_dsi_attach,
	.set_backlight = qcom_dsi_set_backlight,
};

static const struct udevice_id qcom_dsi_ids[] = {
	{ .compatible = "qcom,mdss-dsi-ctrl"},
	{ }
};

U_BOOT_DRIVER(qcom_dsi) = {
	.name				= "qcom-dsi",
	.id				= UCLASS_VIDEO_BRIDGE,
	.of_match			= qcom_dsi_ids,
	.bind				= qcom_dsi_bind,
	.probe				= qcom_dsi_probe,
	.ops				= &qcom_dsi_ops,
	.priv_auto		= sizeof(struct qcom_dsi_priv),
};
