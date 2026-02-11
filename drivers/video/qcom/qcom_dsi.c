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
#include <generic-phy.h>
#include <lmb.h>
#include <log.h>
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

//#include "mdssreg.h"
#include "qcom_mipi_dsi.h"
#include "dsi.xml.h"
//#include "qcom_dsi_phy.h"
#include "phy/dsi_phy.h"

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

#define REG_DSI_VERSION						0x000001f0

#define DSI_RESET_TOGGLE_DELAY_MS 20

/* for bridge */
struct qcom_dsi_priv {
	struct mipi_dsi_device device;
	void __iomem *hw_base;
	void __iomem *base;
	void __iomem *phy_base;
	struct udevice *panel;
	struct phy phy;
	struct phy_configure_opts_mipi_dphy phy_opts;
	u32 pllref_clk;
	int lane_min_kbps;
	int lane_max_kbps;
	unsigned int lane_mbps; /* per lane */
	struct udevice *vdd_reg;
	struct udevice *dsi_host;
	struct clk_bulk bulk;
	struct display_timing timings;
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
	struct msm_dsi_cfg_handler cfg_hnd;
	phys_addr_t tx_buf;
	u8 *rx_buf;
};

static bool dsi_write_debug = false;

static inline struct qcom_mipi_dsi *host_to_dsi(struct mipi_dsi_host *host)
{
	return container_of(host, struct qcom_mipi_dsi, dsi_host);
}

static inline void dsi_write(void *addr, u32 val)
{
	if (dsi_write_debug)
		printf("%#010x: %#010x\n", (u32)(u64)addr, val);
	writel(val, addr);
}

static inline u32 dsi_read(void *addr)
{
	return readl(addr);
}

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

static void dsi_set_tx_power_mode(int mode, struct qcom_mipi_dsi *dsi)
{
	u32 data;

	data = dsi_read(dsi->base + REG_DSI_CMD_DMA_CTRL);

	if (mode == 0)
		data &= ~DSI_CMD_DMA_CTRL_LOW_POWER;
	else
		data |= DSI_CMD_DMA_CTRL_LOW_POWER;

	dsi_write(dsi->base + REG_DSI_CMD_DMA_CTRL, data);
}

static inline void dsi_flush_cache(uintptr_t addr, int length)
{
	uintptr_t start_addr = (uintptr_t)addr & ~(CONFIG_SYS_CACHELINE_SIZE - 1);
	uintptr_t end_addr = ALIGN((uintptr_t)addr + length, CONFIG_SYS_CACHELINE_SIZE);

	flush_dcache_range((unsigned long)start_addr, (unsigned long)end_addr);
}

#define REG_DSI_VBIF_CTRL	(0x01CC)
static int dsi_cmds2buf_tx(struct mipi_dsi_host *host,
			   const struct mipi_dsi_msg *msg)
{
	struct qcom_mipi_dsi *dsi = host_to_dsi(host);
	struct mipi_dsi_packet packet = { 0 };
	int ret, len, count = 0;
	void __iomem *base = dsi->base;
	u8 *data;
	u32 val = 0;

	while (count < 1000) {
		val = dsi_read(base + REG_DSI_STATUS0);

		if (!(val & (DSI_STATUS0_VIDEO_MODE_ENGINE_BUSY | DSI_STATUS0_CMD_MODE_ENGINE_BUSY)))
			break;

		++count;
	}

	if (val & DSI_STATUS0_VIDEO_MODE_ENGINE_BUSY) {
		dev_err(host->dev, "failed to wait for video engine busy (%x)\n", val);
		return -EIO;
	}
	if (val & DSI_STATUS0_CMD_MODE_ENGINE_BUSY) {
		dev_err(host->dev, "failed to wait for cmd mode engine busy (%x)\n", val);
		return -EIO;
	}

	ret = mipi_dsi_create_packet(&packet, msg);
	if (ret) {
		dev_err(host->dev, "failed to create packet: %d\n", ret);
		return ret;
	}

	len = (packet.size + 3) & (~0x3);
	data = (u8 *)dsi->tx_buf;

	/* MSM specific command format in memory */
	data[0] = packet.header[1];
	data[1] = packet.header[2];
	data[2] = packet.header[0];
	data[3] = BIT(7); /* Last packet */
	if (mipi_dsi_packet_format_is_long(msg->type))
		data[3] |= BIT(6);
	if (msg->rx_buf && msg->rx_len)
		data[3] |= BIT(5);

	/* Long packet */
	if (packet.payload && packet.payload_length)
		memcpy(data + 4, packet.payload, packet.payload_length);

	/* Append 0xff to the end */
	if (packet.size < len)
		memset(data + packet.size, 0xff, len - packet.size);

	dsi_flush_cache(dsi->tx_buf, len);

	// dsi_write(base + REG_DSI_TEST_PATTERN_GEN_CTRL, 0);

	dsi_write(base + REG_DSI_DMA_BASE, dsi->tx_buf);
	dsi_write(base + REG_DSI_DMA_LEN, len);
	// dsi_write(base + REG_DSI_VBIF_CTRL, 0x7);

	// Unmask
	// dsi_write(base + REG_DSI_INTR_CTRL, DSI_IRQ_MASK_CMD_DMA_DONE);
	// Clear
	// dsi_write(base + REG_DSI_INTR_CTRL, DSI_IRQ_MASK_CMD_DMA_DONE | DSI_IRQ_CMD_DMA_DONE);


	dsi_write(base + REG_DSI_TRIG_DMA, 0x1);

	count = 0;
	ret = 0;
	while (count < 1000) {
		val = dsi_read(base + REG_DSI_STATUS0) ;

		if (!(val & DSI_STATUS0_CMD_MODE_ENGINE_BUSY))
			break;

		++count;
	}
	if (val & DSI_STATUS0_CMD_MODE_ENGINE_BUSY) {
		dev_err(host->dev, "DMA timeout, cmd mode engine busy (%x)\n", val);
		return -EIO;
	}

	// Clear
	// dsi_write(base + REG_DSI_INTR_CTRL, DSI_IRQ_MASK_CMD_DMA_DONE | DSI_IRQ_CMD_DMA_DONE);
	// Mask
	// dsi_write(base + REG_DSI_INTR_CTRL, 0);

	return ret < 0 ? ret : packet.size;
}

static int dsi_cmd_dma_rx(struct qcom_mipi_dsi *dsi,
			u8 *buf, int rx_byte, int pkt_size)
{
	u32 *temp, data;
	int i, j = 0, cnt;
	u32 read_cnt;
	u8 reg[16];
	int repeated_bytes = 0;
	int buf_offset = buf - dsi->rx_buf;

	temp = (u32 *)reg;
	cnt = (rx_byte + 3) >> 2;
	if (cnt > 4)
		cnt = 4; /* 4 x 32 bits registers only */

	if (rx_byte == 4)
		read_cnt = 4;
	else
		read_cnt = pkt_size + 6;

	/*
	 * In case of multiple reads from the panel, after the first read, there
	 * is possibility that there are some bytes in the payload repeating in
	 * the RDBK_DATA registers. Since we read all the parameters from the
	 * panel right from the first byte for every pass. We need to skip the
	 * repeating bytes and then append the new parameters to the rx buffer.
	 */
	if (read_cnt > 16) {
		int bytes_shifted;
		/* Any data more than 16 bytes will be shifted out.
		 * The temp read buffer should already contain these bytes.
		 * The remaining bytes in read buffer are the repeated bytes.
		 */
		bytes_shifted = read_cnt - 16;
		repeated_bytes = buf_offset - bytes_shifted;
	}

	for (i = cnt - 1; i >= 0; i--) {
		data = dsi_read(dsi->base + REG_DSI_RDBK_DATA(i));
		*temp++ = ntohl(data); /* to host byte order */
		debug("data = 0x%x and ntohl(data) = 0x%x\n", data, ntohl(data));
	}

	for (i = repeated_bytes; i < 16; i++)
		buf[j++] = reg[i];

	return j;
}

/*
 * dsi_short_read1_resp: 1 parameter
 */
static int dsi_short_read1_resp(u8 *buf, const struct mipi_dsi_msg *msg)
{
	u8 *data = msg->rx_buf;

	if (data && (msg->rx_len >= 1)) {
		*data = buf[1]; /* strip out dcs type */
		return 1;
	}

	pr_err("%s: read data does not match with rx_buf len %zu\n",
		__func__, msg->rx_len);
	return -EINVAL;
}

/*
 * dsi_short_read2_resp: 2 parameter
 */
static int dsi_short_read2_resp(u8 *buf, const struct mipi_dsi_msg *msg)
{
	u8 *data = msg->rx_buf;

	if (data && (msg->rx_len >= 2)) {
		data[0] = buf[1]; /* strip out dcs type */
		data[1] = buf[2];
		return 2;
	}

	pr_err("%s: read data does not match with rx_buf len %zu\n",
		__func__, msg->rx_len);
	return -EINVAL;
}

static int dsi_long_read_resp(u8 *buf, const struct mipi_dsi_msg *msg)
{
	/* strip out 4 byte dcs header */
	if (msg->rx_buf && msg->rx_len)
		memcpy(msg->rx_buf, buf + 4, msg->rx_len);

	return msg->rx_len;
}

int msm_dsi_host_cmd_rx(struct mipi_dsi_host *host,
				const struct mipi_dsi_msg *msg)
{
	struct qcom_mipi_dsi *dsi = host_to_dsi(host);
	//const struct msm_dsi_cfg_handler *cfg_hnd = msm_host->cfg_hnd;
	int data_byte, rx_byte, dlen, end;
	int short_response, diff, pkt_size, ret = 0;
	char cmd;
	int rlen = msg->rx_len;
	u8 *buf;

	debug("Dsi RX\n");
	if (rlen <= 2) {
		short_response = 1;
		pkt_size = rlen;
		rx_byte = 4;
	} else {
		short_response = 0;
		data_byte = 10;	/* first read */
		if (rlen < data_byte)
			pkt_size = rlen;
		else
			pkt_size = data_byte;
		rx_byte = data_byte + 6; /* 4 header + 2 crc */
	}

	buf = dsi->rx_buf;
	end = 0;
	while (!end) {
		u8 tx[2] = {pkt_size & 0xff, pkt_size >> 8};
		struct mipi_dsi_msg max_pkt_size_msg = {
			.channel = msg->channel,
			.type = MIPI_DSI_SET_MAXIMUM_RETURN_PACKET_SIZE,
			.tx_len = 2,
			.tx_buf = tx,
		};

		debug("rlen=%d pkt_size=%d rx_byte=%d\n",
			rlen, pkt_size, rx_byte);

		ret = dsi_cmds2buf_tx(host, &max_pkt_size_msg);
		if (ret < 2) {
			pr_err("%s: Set max pkt size failed, %d\n",
				__func__, ret);
			return -EINVAL;
		}

		/* 8650 is 6g_v2 MSM_DSI_VER_MAJOR_6G, MSM_DSI_6G_VER_MINOR_V2_8_0
		if ((cfg_hnd->major == MSM_DSI_VER_MAJOR_6G) &&
			(cfg_hnd->minor >= MSM_DSI_6G_VER_MINOR_V1_1)) {
		}
		*/
		if (1) {
			/* Clear the RDBK_DATA registers */
			dsi_write(dsi->base + REG_DSI_RDBK_DATA_CTRL,
					DSI_RDBK_DATA_CTRL_CLR);
			wmb(); /* make sure the RDBK registers are cleared */
			dsi_write(dsi->base + REG_DSI_RDBK_DATA_CTRL, 0);
			wmb(); /* release cleared status before transfer */
		}

		ret = dsi_cmds2buf_tx(host, msg);
		if (ret < 0) {
			pr_err("%s: Read cmd Tx failed, %d\n", __func__, ret);
			return ret;
		} else if (ret < msg->tx_len) {
			pr_err("%s: Read cmd Tx failed, too short: %d\n", __func__, ret);
			return -EIO;
		}

		/*
		 * once cmd_dma_done interrupt received,
		 * return data from client is ready and stored
		 * at RDBK_DATA register already
		 * since rx fifo is 16 bytes, dcs header is kept at first loop,
		 * after that dcs header lost during shift into registers
		 */
		dlen = dsi_cmd_dma_rx(dsi, buf, rx_byte, pkt_size);

		if (dlen <= 0)
			return 0;

		if (short_response)
			break;

		if (rlen <= data_byte) {
			diff = data_byte - rlen;
			end = 1;
		} else {
			diff = 0;
			rlen -= data_byte;
		}

		if (!end) {
			dlen -= 2; /* 2 crc */
			dlen -= diff;
			buf += dlen;	/* next start position */
			data_byte = 14;	/* NOT first read */
			if (rlen < data_byte)
				pkt_size += rlen;
			else
				pkt_size += data_byte;
			debug("buf=%p dlen=%d diff=%d\n", buf, dlen, diff);
		}
	}

	/*
	 * For single Long read, if the requested rlen < 10,
	 * we need to shift the start position of rx
	 * data buffer to skip the bytes which are not
	 * updated.
	 */
	if (pkt_size < 10 && !short_response)
		buf = dsi->rx_buf + (10 - rlen);
	else
		buf = dsi->rx_buf;

	cmd = buf[0];
	switch (cmd) {
	case MIPI_DSI_RX_ACKNOWLEDGE_AND_ERROR_REPORT:
		pr_err("%s: rx ACK_ERR_PACLAGE\n", __func__);
		ret = 0;
		break;
	case MIPI_DSI_RX_GENERIC_SHORT_READ_RESPONSE_1BYTE:
	case MIPI_DSI_RX_DCS_SHORT_READ_RESPONSE_1BYTE:
		ret = dsi_short_read1_resp(buf, msg);
		break;
	case MIPI_DSI_RX_GENERIC_SHORT_READ_RESPONSE_2BYTE:
	case MIPI_DSI_RX_DCS_SHORT_READ_RESPONSE_2BYTE:
		ret = dsi_short_read2_resp(buf, msg);
		break;
	case MIPI_DSI_RX_GENERIC_LONG_READ_RESPONSE:
	case MIPI_DSI_RX_DCS_LONG_READ_RESPONSE:
		ret = dsi_long_read_resp(buf, msg);
		break;
	default:
		printf("%s:Invalid response cmd 0x%x\n", __func__, cmd);
		ret = 0;
	}

	return ret;
}

static ssize_t qcom_mipi_dsi_host_transfer(struct mipi_dsi_host *host,
					 const struct mipi_dsi_msg *msg)
{
	struct qcom_mipi_dsi *dsi = host_to_dsi(host);
	int ret;
	u32 save_ctrl;

	/* from msm_dsi_host_xfer_prepare() */
	if (!(msg->flags & MIPI_DSI_MSG_USE_LPM))
		dsi_set_tx_power_mode(0, dsi);

	save_ctrl = dsi_read(dsi->base + REG_DSI_CTRL);
	dsi_write(dsi->base + REG_DSI_CTRL, save_ctrl | 0x5);

	if (msg->rx_buf && msg->rx_len) {
		ret = msm_dsi_host_cmd_rx(host, msg);
		if (ret)
			goto out;
		ret = msg->rx_len;
	} else {
		ret = dsi_cmds2buf_tx(host, msg);
	}

out:
	dsi_write(dsi->base + REG_DSI_CTRL, save_ctrl);

	/* from msm_dsi_host_xfer_restore() */
	if (!(msg->flags & MIPI_DSI_MSG_USE_LPM))
		dsi_set_tx_power_mode(1, dsi);

	return ret;
}

static const struct mipi_dsi_host_ops qcom_mipi_dsi_host_ops = {
	.attach = qcom_mipi_dsi_host_attach,
	.transfer = qcom_mipi_dsi_host_transfer,
};

#define DSI_CLK_CTRL_ENABLE_CLKS	\
		(DSI_CLK_CTRL_AHBS_HCLK_ON | DSI_CLK_CTRL_AHBM_SCLK_ON | \
		DSI_CLK_CTRL_PCLK_ON | DSI_CLK_CTRL_DSICLK_ON | \
		DSI_CLK_CTRL_BYTECLK_ON | DSI_CLK_CTRL_ESCCLK_ON | \
		DSI_CLK_CTRL_FORCE_ON_DYN_AHBM_HCLK)

static void qcom_mipi_dsi_init_ctrl(void __iomem *base)
{
	debug("DSI host controller init\n");

	/* DSC only */
	dsi_write(base + REG_DSI_VIDEO_COMPRESSION_MODE_CTRL, 0x8700B01);
	dsi_write(base + REG_DSI_ACTIVE_H, 0x02f80028);
	dsi_write(base + REG_DSI_ACTIVE_V, 0x08850015);
	dsi_write(base + REG_DSI_TOTAL, 0x0a18030f);
	dsi_write(base + REG_DSI_ACTIVE_HSYNC, 0x00140000);
	dsi_write(base + REG_DSI_ACTIVE_VSYNC_HPOS, 0x00000000);
	dsi_write(base + REG_DSI_ACTIVE_VSYNC_VPOS, 0x00010000);

	dsi_write(base + REG_DSI_CLK_CTRL, 0x23F);
	dsi_write(base + REG_DSI_RESET, 0x1);
	mdelay(20);
	dsi_write(base + REG_DSI_RESET, 0x0);

	dsi_write(base + REG_DSI_VID_CFG0, 0x2009130);
	dsi_write(base + REG_DSI_VID_CFG1, 0x0);

	dsi_write(base + REG_DSI_CMD_DMA_CTRL, 0x14000000);

	dsi_write(base + REG_DSI_TRIG_CTRL, 0x80001004);
	dsi_write(base + REG_DSI_CLKOUT_TIMING_CTRL, 0x1A26);
	dsi_write(base + REG_DSI_EOT_PACKET_CTRL, 0x1);
	dsi_write(base + REG_DSI_INTR_CTRL, 0);
	dsi_write(base + REG_DSI_CLK_CTRL, 0x23F);
	dsi_write(base + REG_DSI_LANE_SWAP_CTRL, 0x0);
	dsi_write(base + REG_DSI_LANE_CTRL, DSI_LANE_CTRL_HS_REQ_SEL_PHY);

	dsi_write(base + REG_DSI_CTRL, 0x173);

	dsi_write(base + REG_DSI_CPHY_MODE_CTRL, 0x1);
}

#define DSI_DUMP_REG(base, reg) \
	printf(#reg ": %#llx: %#x\n", (u64)(base) + reg, dsi_read(base + reg))

static void dsi_dump_regs(void __iomem *base)
{
	DSI_DUMP_REG(base, REG_DSI_ACTIVE_H);
	DSI_DUMP_REG(base, REG_DSI_ACTIVE_HSYNC);
	DSI_DUMP_REG(base, REG_DSI_ACTIVE_V);
	DSI_DUMP_REG(base, REG_DSI_ACTIVE_VSYNC_HPOS);
	DSI_DUMP_REG(base, REG_DSI_ACTIVE_VSYNC_VPOS);
	DSI_DUMP_REG(base, REG_DSI_CLK_CTRL);
	DSI_DUMP_REG(base, REG_DSI_CLKOUT_TIMING_CTRL);
	DSI_DUMP_REG(base, REG_DSI_CMD_CFG0);
	DSI_DUMP_REG(base, REG_DSI_CMD_CFG1);
	DSI_DUMP_REG(base, REG_DSI_CMD_DMA_CTRL);
	DSI_DUMP_REG(base, REG_DSI_CMD_MDP_STREAM0_CTRL);
	DSI_DUMP_REG(base, REG_DSI_CMD_MDP_STREAM0_TOTAL);
	DSI_DUMP_REG(base, REG_DSI_CMD_MODE_MDP_CTRL2);
	DSI_DUMP_REG(base, REG_DSI_CPHY_MODE_CTRL);
	DSI_DUMP_REG(base, REG_DSI_CTRL);
	DSI_DUMP_REG(base, REG_DSI_DMA_BASE);
	DSI_DUMP_REG(base, REG_DSI_DMA_LEN);
	DSI_DUMP_REG(base, REG_DSI_EOT_PACKET_CTRL);
	DSI_DUMP_REG(base, REG_DSI_ERR_INT_MASK0);
	DSI_DUMP_REG(base, REG_DSI_INTR_CTRL);
	DSI_DUMP_REG(base, REG_DSI_LANE_CTRL);
	DSI_DUMP_REG(base, REG_DSI_LANE_SWAP_CTRL);
	DSI_DUMP_REG(base, REG_DSI_PHY_RESET);
	DSI_DUMP_REG(base, REG_DSI_RDBK_DATA_CTRL);
	DSI_DUMP_REG(base, REG_DSI_RESET);
	DSI_DUMP_REG(base, REG_DSI_STATUS0);
	DSI_DUMP_REG(base, REG_DSI_T_CLK_PRE_EXTEND);
	DSI_DUMP_REG(base, REG_DSI_TEST_PATTERN_GEN_CTRL);
	DSI_DUMP_REG(base, REG_DSI_TOTAL);
	DSI_DUMP_REG(base, REG_DSI_TRIG_CTRL);
	DSI_DUMP_REG(base, REG_DSI_TRIG_DMA);
	DSI_DUMP_REG(base, REG_DSI_VBIF_CTRL);
	DSI_DUMP_REG(base, REG_DSI_VERSION);
	DSI_DUMP_REG(base, REG_DSI_VID_CFG0);
	DSI_DUMP_REG(base, REG_DSI_VID_CFG1);
	DSI_DUMP_REG(base, REG_DSI_VIDEO_COMPRESSION_MODE_CTRL);
}

static inline enum dsi_traffic_mode dsi_get_traffic_mode(const u32 mode_flags)
{
	if (mode_flags & MIPI_DSI_MODE_VIDEO_BURST)
		return BURST_MODE;
	else if (mode_flags & MIPI_DSI_MODE_VIDEO_SYNC_PULSE)
		return NON_BURST_SYNCH_PULSE;

	return NON_BURST_SYNCH_EVENT;
}

static inline enum dsi_vid_dst_format
dsi_get_vid_fmt(const enum mipi_dsi_pixel_format mipi_fmt)
{
	switch (mipi_fmt) {
	case MIPI_DSI_FMT_RGB888:	return VID_DST_FORMAT_RGB888;
	case MIPI_DSI_FMT_RGB666:	return VID_DST_FORMAT_RGB666_LOOSE;
	case MIPI_DSI_FMT_RGB666_PACKED:	return VID_DST_FORMAT_RGB666;
	case MIPI_DSI_FMT_RGB565:	return VID_DST_FORMAT_RGB565;
	default:			return VID_DST_FORMAT_RGB888;
	}
}

static inline enum dsi_cmd_dst_format
dsi_get_cmd_fmt(const enum mipi_dsi_pixel_format mipi_fmt)
{
	switch (mipi_fmt) {
	case MIPI_DSI_FMT_RGB888:	return CMD_DST_FORMAT_RGB888;
	case MIPI_DSI_FMT_RGB666_PACKED:
	case MIPI_DSI_FMT_RGB666:	return CMD_DST_FORMAT_RGB666;
	case MIPI_DSI_FMT_RGB565:	return CMD_DST_FORMAT_RGB565;
	default:			return CMD_DST_FORMAT_RGB888;
	}
}

bool msm_dsi_host_is_wide_bus_enabled(struct qcom_mipi_dsi *priv)
{
	#define DSC_ENABLED false
	return DSC_ENABLED &&
		(priv->cfg_hnd.major == MSM_DSI_VER_MAJOR_6G &&
		 priv->cfg_hnd.minor >= MSM_DSI_6G_VER_MINOR_V2_5_0);
}

/**** PHY_OPS *****/

bool msm_dsi_phy_set_continuous_clock(struct msm_dsi_phy *phy, bool enable)
{
	if (!phy || !phy->plat->cfg->ops.set_continuous_clock)
		return false;

	dev_info(phy->dev, "set_continuous_clock: %d\n", enable);
	return phy->plat->cfg->ops.set_continuous_clock(phy, enable);
}

/**** PHY_OPS_END *****/

static void dsi_ctrl_enable(struct qcom_mipi_dsi *priv,
			struct msm_dsi_phy_shared_timings *phy_shared_timings, struct msm_dsi_phy *phy)
{
	struct mipi_dsi_device *device = priv->device;
	u32 flags = device->mode_flags;
	enum mipi_dsi_pixel_format mipi_fmt = device->format;
	const struct msm_dsi_cfg_handler *cfg_hnd = &priv->cfg_hnd;
	u32 data = 0, lane_ctrl = 0;

	if (flags & MIPI_DSI_MODE_VIDEO) {
		if (flags & MIPI_DSI_MODE_VIDEO_HSE)
			data |= DSI_VID_CFG0_PULSE_MODE_HSA_HE;
		if (flags & MIPI_DSI_MODE_VIDEO_HFP) // VIDEO_NO_x in Linux
			data |= DSI_VID_CFG0_HFP_POWER_STOP;
		if (flags & MIPI_DSI_MODE_VIDEO_HBP) // VIDEO_NO_x in Linux
			data |= DSI_VID_CFG0_HBP_POWER_STOP;
		if (flags & MIPI_DSI_MODE_VIDEO_HSA) // VIDEO_NO_x in Linux
			data |= DSI_VID_CFG0_HSA_POWER_STOP;
		/* Always set low power stop mode for BLLP
		 * to let command engine send packets
		 */
		data |= DSI_VID_CFG0_EOF_BLLP_POWER_STOP |
			DSI_VID_CFG0_BLLP_POWER_STOP;
		data |= DSI_VID_CFG0_TRAFFIC_MODE(dsi_get_traffic_mode(flags));
		data |= DSI_VID_CFG0_DST_FORMAT(dsi_get_vid_fmt(mipi_fmt));
		data |= DSI_VID_CFG0_VIRT_CHANNEL(device->channel);
		if (msm_dsi_host_is_wide_bus_enabled(priv))
			data |= DSI_VID_CFG0_DATABUS_WIDEN;
		dsi_write(priv->base + REG_DSI_VID_CFG0, data);

		/* Do not swap RGB colors */
		data = DSI_VID_CFG1_RGB_SWAP(SWAP_RGB);
		dsi_write(priv->base + REG_DSI_VID_CFG1, 0);
	} else {
		/* Do not swap RGB colors */
		data = DSI_CMD_CFG0_RGB_SWAP(SWAP_RGB);
		data |= DSI_CMD_CFG0_DST_FORMAT(dsi_get_cmd_fmt(mipi_fmt));
		dsi_write(priv->base + REG_DSI_CMD_CFG0, data);

		data = DSI_CMD_CFG1_WR_MEM_START(MIPI_DCS_WRITE_MEMORY_START) |
			DSI_CMD_CFG1_WR_MEM_CONTINUE(
					MIPI_DCS_WRITE_MEMORY_CONTINUE);
		/* Always insert DCS command */
		data |= DSI_CMD_CFG1_INSERT_DCS_COMMAND;
		dsi_write(priv->base + REG_DSI_CMD_CFG1, data);

		if (cfg_hnd->major == MSM_DSI_VER_MAJOR_6G) {
			data = dsi_read(priv->base + REG_DSI_CMD_MODE_MDP_CTRL2);

			if (cfg_hnd->minor >= MSM_DSI_6G_VER_MINOR_V1_3)
				data |= DSI_CMD_MODE_MDP_CTRL2_BURST_MODE;

			if (msm_dsi_host_is_wide_bus_enabled(priv))
				data |= DSI_CMD_MODE_MDP_CTRL2_DATABUS_WIDEN;

			dsi_write(priv->base + REG_DSI_CMD_MODE_MDP_CTRL2, data);
		}
	}

	dsi_write(priv->base + REG_DSI_CMD_DMA_CTRL,
			DSI_CMD_DMA_CTRL_FROM_FRAME_BUFFER |
			DSI_CMD_DMA_CTRL_LOW_POWER);

	data = 0;
	/* Always assume dedicated TE pin */
	data |= DSI_TRIG_CTRL_TE;
	data |= DSI_TRIG_CTRL_MDP_TRIGGER(TRIGGER_NONE);
	data |= DSI_TRIG_CTRL_DMA_TRIGGER(TRIGGER_SW);
	data |= DSI_TRIG_CTRL_STREAM(device->channel);
	if ((cfg_hnd->major == MSM_DSI_VER_MAJOR_6G) &&
		(cfg_hnd->minor >= MSM_DSI_6G_VER_MINOR_V1_2))
		data |= DSI_TRIG_CTRL_BLOCK_DMA_WITHIN_FRAME;
	dsi_write(priv->base + REG_DSI_TRIG_CTRL, data);

	data = DSI_CLKOUT_TIMING_CTRL_T_CLK_POST(phy_shared_timings->clk_post) |
		DSI_CLKOUT_TIMING_CTRL_T_CLK_PRE(phy_shared_timings->clk_pre);
	dev_info(device->dev, "Writing DSI_CLKOUT_TIMING_CTRL: %#x (post %u, pre %u)\n", data, phy_shared_timings->clk_post, phy_shared_timings->clk_pre);
	dsi_write(priv->base + REG_DSI_CLKOUT_TIMING_CTRL, data);

	if ((cfg_hnd->major == MSM_DSI_VER_MAJOR_6G) &&
	    (cfg_hnd->minor > MSM_DSI_6G_VER_MINOR_V1_0) &&
	    phy_shared_timings->clk_pre_inc_by_2)
		dsi_write(priv->base + REG_DSI_T_CLK_PRE_EXTEND,
			  DSI_T_CLK_PRE_EXTEND_INC_BY_2_BYTECLK);

	data = 0;
	if (!(flags & MIPI_DSI_MODE_EOT_PACKET)) // NO_EOT_PACKET in Linux
		data |= DSI_EOT_PACKET_CTRL_TX_EOT_APPEND;
	dsi_write(priv->base + REG_DSI_EOT_PACKET_CTRL, data);

	/* allow only ack-err-status to generate interrupt */
	dsi_write(priv->base + REG_DSI_ERR_INT_MASK0, 0x13ff3fe0);

	/* U-Boot: disable all interrupts! */
	dsi_write(priv->base + REG_DSI_INTR_CTRL, 0);

	dsi_write(priv->base + REG_DSI_CLK_CTRL, DSI_CLK_CTRL_ENABLE_CLKS);

	data = DSI_CTRL_CLK_EN;

	dev_info(device->dev, "lane number=%d\n", device->lanes);
	data |= ((DSI_CTRL_LANE0 << device->lanes) - DSI_CTRL_LANE0);

	if (device->mode_flags & MIPI_DSI_MODE_VIDEO)
		data |= DSI_CTRL_VID_MODE_EN;
	else
	 	data |= DSI_CTRL_CMD_MODE_EN;

	/* FIXME: lane swap not supported yet */
	u32 dlane_swap = 0;
	dsi_write(priv->base + REG_DSI_LANE_SWAP_CTRL,
		  DSI_LANE_SWAP_CTRL_DLN_SWAP_SEL(dlane_swap));

	if (!(flags & MIPI_DSI_CLOCK_NON_CONTINUOUS)) {
		lane_ctrl = dsi_read(priv->base + REG_DSI_LANE_CTRL);

		if (msm_dsi_phy_set_continuous_clock(phy, true))
			lane_ctrl &= ~DSI_LANE_CTRL_HS_REQ_SEL_PHY;

		dsi_write(priv->base + REG_DSI_LANE_CTRL,
			lane_ctrl | DSI_LANE_CTRL_CLKLN_HS_FORCE_REQUEST);
	} else {
		dsi_write(priv->base + REG_DSI_LANE_CTRL, DSI_LANE_CTRL_HS_REQ_SEL_PHY);
	}

	data |= DSI_CTRL_ENABLE;

	dsi_write(priv->base + REG_DSI_CTRL, data);

	if (phy->plat->cphy_mode)
		dsi_write(priv->base + REG_DSI_CPHY_MODE_CTRL, BIT(0));


	printf("Enabling DSI0 CMD_DMA_DEBUG_MODE!\n");
	dsi_write(priv->base + 0x2c4, BIT(16));
}

/*
static const struct display_timing default_timing = {
	.pixelclock.typ		= (1080 + 20 + 2 + 20) * (2400 + 20 + 2 + 18) * 120, // 144
	.hactive.typ		= 1080,

	.hfront_porch.typ	= 20,
	.hback_porch.typ	= 20,
	.hsync_len.typ		= 2,

	.vactive.typ		= 2400,
	.vfront_porch.typ	= 20,
	.vback_porch.typ	= 18,
	.vsync_len.typ		= 2,
};
*/

static void dsi_timing_setup(struct qcom_mipi_dsi *dsi, struct display_timing *timings)
{
	void __iomem *base = dsi->base;
	u32 hs_start = 0, vs_start = 0; /* take sync start as 0 */

	// 1080 + 20 + 20 + 2
	u32 h_total = timings->hactive.typ + timings->hfront_porch.typ + timings->hback_porch.typ + timings->hsync_len.typ;
	u32 v_total = timings->vactive.typ + timings->vfront_porch.typ + timings->vback_porch.typ + timings->vsync_len.typ;

	// (1080 + 20 + 2) - (1080 + 20) = 2 ~= hs_start + hs_len = 0 + 2
	u32 hs_end = timings->hsync_len.typ;
	u32 vs_end = timings->vsync_len.typ;

	// (1080 + 20 + 2 + 20) - (1080 + 18) = 20 ~= hs_start + hs_len + hback_porch = 0 + 2 + 18 = 20
	u32 ha_start = timings->hsync_len.typ + timings->hback_porch.typ;
	u32 ha_end = ha_start + timings->hactive.typ;
	u32 va_start = timings->vsync_len.typ + timings->vback_porch.typ;
	u32 va_end = va_start + timings->vactive.typ;
	u32 hdisplay = timings->hactive.typ;
	u32 wc;

	dsi_write_debug = true;

	if (dsi->device->mode_flags & MIPI_DSI_MODE_VIDEO) {
		dsi_write(base + REG_DSI_ACTIVE_H,
			DSI_ACTIVE_H_START(ha_start) |
			DSI_ACTIVE_H_END(ha_end));
		dsi_write(base + REG_DSI_ACTIVE_V,
			DSI_ACTIVE_V_START(va_start) |
			DSI_ACTIVE_V_END(va_end));
		dsi_write(base + REG_DSI_TOTAL,
			DSI_TOTAL_H_TOTAL(h_total - 1) |
			DSI_TOTAL_V_TOTAL(v_total - 1));

		dsi_write(base + REG_DSI_ACTIVE_HSYNC,
			DSI_ACTIVE_HSYNC_START(hs_start) |
			DSI_ACTIVE_HSYNC_END(hs_end));
		dsi_write(base + REG_DSI_ACTIVE_VSYNC_HPOS, 0);
		dsi_write(base + REG_DSI_ACTIVE_VSYNC_VPOS,
			DSI_ACTIVE_VSYNC_VPOS_START(vs_start) |
			DSI_ACTIVE_VSYNC_VPOS_END(vs_end));
	} else {		/* command mode */
		/* image data and 1 byte write_memory_start cmd */
		wc = hdisplay * mipi_dsi_pixel_format_to_bpp(dsi->device->format) / 8 + 1;

		dsi_write(base + REG_DSI_CMD_MDP_STREAM0_CTRL,
			DSI_CMD_MDP_STREAM0_CTRL_WORD_COUNT(wc) |
			DSI_CMD_MDP_STREAM0_CTRL_VIRTUAL_CHANNEL(
					dsi->device->channel) |
			DSI_CMD_MDP_STREAM0_CTRL_DATA_TYPE(
					MIPI_DSI_DCS_LONG_WRITE));

		dsi_write(base + REG_DSI_CMD_MDP_STREAM0_TOTAL,
			DSI_CMD_MDP_STREAM0_TOTAL_H_TOTAL(hdisplay) |
			DSI_CMD_MDP_STREAM0_TOTAL_V_TOTAL(timings->vactive.typ));
	}

	dsi_write_debug = false;
}

static void dsi_sw_reset(struct qcom_mipi_dsi *dsi)
{
	u32 ctrl;

	ctrl = dsi_read(dsi->base + REG_DSI_CTRL);

	if (ctrl & DSI_CTRL_ENABLE) {
		dsi_write(dsi->base + REG_DSI_CTRL, ctrl & ~DSI_CTRL_ENABLE);
		/*
		 * dsi controller need to be disabled before
		 * clocks turned on
		 */
		wmb();
	}

	dsi_write(dsi->base + REG_DSI_CLK_CTRL, DSI_CLK_CTRL_ENABLE_CLKS);
	wmb(); /* clocks need to be enabled before reset */

	/* dsi controller can only be reset while clocks are running */
	dsi_write(dsi->base + REG_DSI_RESET, 1);
	udelay(DSI_RESET_TOGGLE_DELAY_MS * 1000); /* make sure reset happen */
	dsi_write(dsi->base + REG_DSI_RESET, 0);
	wmb(); /* controller out of reset */

	if (ctrl & DSI_CTRL_ENABLE) {
		dsi_write(dsi->base + REG_DSI_CTRL, ctrl);
		wmb();	/* make sure dsi controller enabled again */
	}
}


static int dsi_get_version(const void __iomem *base, u32 *major, u32 *minor)
{
	u32 ver;

	if (!major || !minor)
		return -EINVAL;

	/*
	 * From DSI6G(v3), addition of a 6G_HW_VERSION register at offset 0
	 * makes all other registers 4-byte shifted down.
	 *
	 * In order to identify between DSI6G(v3) and beyond, and DSIv2 and
	 * older, we read the DSI_VERSION register without any shift(offset
	 * 0x1f0). In the case of DSIv2, this hast to be a non-zero value. In
	 * the case of DSI6G, this has to be zero (the offset points to a
	 * scratch register which we never touch)
	 */

	ver = readl(base + REG_DSI_VERSION);
	if (ver) {
		/* older dsi host, there is no register shift */
		ver = FIELD(ver, DSI_VERSION_MAJOR);
		if (ver <= MSM_DSI_VER_MAJOR_V2) {
			/* old versions */
			*major = ver;
			*minor = 0;
			return 0;
		} else {
			return -EINVAL;
		}
	} else {
		/*
		 * newer host, offset 0 has 6G_HW_VERSION, the rest of the
		 * registers are shifted down, read DSI_VERSION again with
		 * the shifted offset
		 */
		ver = readl(base + DSI_6G_REG_SHIFT + REG_DSI_VERSION);
		ver = FIELD(ver, DSI_VERSION_MAJOR);
		if (ver == MSM_DSI_VER_MAJOR_6G) {
			/* 6G version */
			*major = ver;
			*minor = readl(base + REG_DSI_6G_HW_VERSION);
			return 0;
		} else {
			return -EINVAL;
		}
	}
}


static int qcom_mipi_dsi_init(struct udevice *dev,
			    struct mipi_dsi_device *device,
			    struct display_timing *timings,
			    unsigned int max_data_lanes,
			    const struct mipi_dsi_phy_ops *phy_ops)
{
	struct qcom_mipi_dsi *dsi = dev_get_priv(dev);
	int ret;

	dev_dbg(dev, "Initialize DSI host\n");
	if (!phy_ops->init) {
		dev_err(device->dev, "Phy not properly configured\n");
		return -ENODEV;
	}

	dsi->phy_ops = phy_ops;
	dsi->max_data_lanes = max_data_lanes;
	dsi->device = device;
	dsi->dsi_host.dev = (struct device *)dev;
	dsi->dsi_host.ops = &qcom_mipi_dsi_host_ops;
	device->host = &dsi->dsi_host;

	dsi->tx_buf = 0xf0000000;

	ret = lmb_alloc_mem(LMB_MEM_ALLOC_MAX, 0x1000, &dsi->tx_buf, 0x1000, LMB_NONE);
	if (ret) {
		dev_err(device->dev, "Failed to allocate tx_buf @ %#llx!\n", dsi->tx_buf);
	}
	memset((void *)dsi->tx_buf, 0xff, 0x1000);
	dsi->rx_buf = malloc(0x1000);

	dsi_write(dsi->base + REG_DSI_PHY_RESET, 0x1);
	mdelay(1);
	dsi_write(dsi->base + REG_DSI_PHY_RESET, 0x0);
	mdelay(1);

	ret = phy_ops->init(device);

	/* Ugly poke into through the DSI bridge device to the phy */
	struct qcom_dsi_priv *priv = dev_get_priv(device->dev);
	struct msm_dsi_phy *phy_priv = dev_get_priv(priv->phy.dev);

	// qcom_mipi_dsi_init_ctrl(dsi->base);
	dsi_timing_setup(dsi, timings);
	dsi_sw_reset(dsi);
	dsi_ctrl_enable(dsi, &phy_priv->timing.shared_timings, phy_priv);

	dsi_dump_regs(dsi->base);

	return 0;
}

static int qcom_mipi_dsi_enable(struct udevice *dev)
{
	/* NOP */

	return 0;
}

struct dsi_host_ops qcom_mipi_dsi_ops = {
	.init = qcom_mipi_dsi_init,
	.enable = qcom_mipi_dsi_enable,
};

static int qcom_mipi_dsi_probe(struct udevice *dev)
{
	struct qcom_mipi_dsi *dsi = dev_get_priv(dev);

	dsi->base = dev_read_addr_ptr(dev);
	if (!dsi->base) {
		dev_err(dev, "dsi dt register address error\n");
		return -EINVAL;
	}

	/* check hardware version */
	dsi_get_version(dsi->base, &dsi->cfg_hnd.major, &dsi->cfg_hnd.minor);
	dev_info(dev, "Got hw_version: %#x.%#x\n", dsi->cfg_hnd.major, dsi->cfg_hnd.minor);
	if (dsi->cfg_hnd.minor != MSM_DSI_6G_VER_MINOR_V2_8_0) { // sm8650
		dev_err(dev, "DSI version %#x.%#x not supported\n", dsi->cfg_hnd.major, dsi->cfg_hnd.minor);
		dev_dbg(dev, "remove and unbind all DSI child\n");
		device_chld_remove(dev, NULL, DM_REMOVE_NORMAL);
		device_chld_unbind(dev, NULL);
		return -ENODEV;
	}

	dsi->base += 4;

	dev_dbg(dev, "DSI host probe\n");

	return 0;
}

static int qcom_mipi_dsi_remove(struct udevice *dev)
{
	struct qcom_mipi_dsi *dsi = dev_get_priv(dev);
	
	dev_dbg(dev, "DSI host remove??\n");

	dsi_write(dsi->base + REG_DSI_PHY_RESET, 0x1);
	mdelay(1);
	dsi_write(dsi->base + REG_DSI_PHY_RESET, 0x0);
	mdelay(1);

	dsi_write(dsi->base + REG_DSI_CLK_CTRL, 0x23F);
	dsi_write(dsi->base + REG_DSI_RESET, 0x1);
	mdelay(20);
	dsi_write(dsi->base + REG_DSI_RESET, 0x0);

	return 0;
}

U_BOOT_DRIVER(qcom_mipi_dsi) = {
	.name = "qcom_mipi_dsi",
	.id = UCLASS_DSI_HOST,
	.probe = qcom_mipi_dsi_probe,
	.remove	= qcom_mipi_dsi_remove,
	.ops = &qcom_mipi_dsi_ops,
	.priv_auto = sizeof(struct qcom_mipi_dsi),
	.flags = DM_FLAG_LEAVE_PD_ON,
};

static int dsi_phy_init(void *priv_data)
{
	struct mipi_dsi_device *device = priv_data;
	struct udevice *dev = device->dev;
	struct qcom_dsi_priv *dsi = dev_get_priv(dev);
	int ret;

	dev_dbg(dev, "Initialize DSI physical layer\n");

	if (!generic_phy_valid(&dsi->phy)) {
		dev_dbg(dev, "fail to validate phy, phy %p phy->dev %p\n", &dsi->phy, dsi->phy.dev);
	}

	ret = generic_phy_power_on(&dsi->phy);
	if (ret) {
		dev_dbg(dev, "fail to power on phy\n");
	}

	ret = clk_enable_bulk(&dsi->bulk);
	if (ret) {
		dev_err(dev, "peripheral clock enable error %d\n", ret);
		return ret;
	}

	return 0;
}

static const struct mipi_dsi_phy_ops mdss_dsi_phy_ops = {
	.init = dsi_phy_init,
};

static int qcom_dsi_attach(struct udevice *dev)
{
	struct qcom_dsi_priv *priv = dev_get_priv(dev);
	struct mipi_dsi_device *device = &priv->device;
	struct mipi_dsi_panel_plat *mplat;
	int ret;

	dev_info(dev, "%s\n", __func__);

	ret = uclass_get_device(UCLASS_PANEL, dev_seq(dev), &priv->panel);
	if (ret) {
		dev_err(dev, "panel device error %d\n", ret);
		return ret;
	}

	mplat = dev_get_plat(priv->panel);
	mplat->device = device;
	device->lanes = mplat->lanes;
	device->format = mplat->format;
	device->mode_flags = mplat->mode_flags;

	ret = panel_get_display_timing(priv->panel, &priv->timings);
	if (ret) {
		dev_err(dev, "get panel timing error %d\n", ret);
		return ret;
	}

	ret = uclass_get_device(UCLASS_DSI_HOST, dev_seq(dev), &priv->dsi_host);
	if (ret) {
		dev_err(dev, "No video dsi host detected %d\n", ret);
		return ret;
	}

	ret = dsi_host_init(priv->dsi_host, device, &priv->timings, 4,
			    &mdss_dsi_phy_ops);
	if (ret) {
		dev_err(dev, "failed to initialize mipi dsi host\n");
		return ret;
	}

	return 0;
}

static int qcom_dsi_get_display_timing(struct udevice *dev, struct display_timing *timing)
{
	struct qcom_dsi_priv *priv = dev_get_priv(dev);

	memcpy(timing, &priv->timings, sizeof(struct display_timing));

	return 0;
}

static int qcom_dsi_set_backlight(struct udevice *dev, int percent)
{
	struct qcom_dsi_priv *priv = dev_get_priv(dev);
	int ret;

	if (percent) {
		ret = panel_enable_backlight(priv->panel);
		if (ret)
			return ret;
	}

	return panel_set_backlight(priv->panel, percent);
}

static int qcom_dsi_bind(struct udevice *dev)
{
	int ret;

	ret = device_bind_driver_to_node(dev, "qcom_mipi_dsi", "mdss_dsi",
					 dev_ofnode(dev), NULL);
	if (ret)
		return ret;

	return dm_scan_fdt_dev(dev);
}

static int qcom_dsi_probe(struct udevice *dev)
{
	struct qcom_dsi_priv *priv = dev_get_priv(dev);
	struct mipi_dsi_device *device = &priv->device;
	int ret;

	device->dev = dev;

	priv->hw_base = dev_read_addr_ptr(dev);
	if (!priv->hw_base) {
		dev_err(dev, "dsi dt register address error\n");
		return -EINVAL;
	}
	priv->base = priv->hw_base + 4;

	ret = generic_phy_get_by_name(dev, "dsi", &priv->phy);
	if (ret && ret != -ENODATA) {
		dev_err(dev, "failed to get mipi dphy: %d\n", ret);
		return ret;
	}

	if (!generic_phy_valid(&priv->phy)) {
		dev_dbg(dev, "fail to validate phy, phy %p phy->dev %p\n", &priv->phy, priv->phy.dev);
	}

	ret =  device_get_supply_regulator(dev, "vdda-supply",
					   &priv->vdd_reg);
	if (ret) {
		dev_err(dev, "Warning: cannot get dsi vdda supply\n");
		return -ENODEV;
	}

	ret = regulator_set_enable(priv->vdd_reg, true);
	if (ret)
		return ret;

	ret = clk_get_bulk(device->dev, &priv->bulk);
	if (ret) {
		dev_err(dev, "peripheral clock get error %d\n", ret);
		goto err_reg;
	}

	printf("DSI bridge probe done!\n");

	return 0;
err_reg:
	regulator_set_enable(priv->vdd_reg, false);

	return ret;
}

static int qcom_dsi_remove(struct udevice *dev)
{
	struct qcom_dsi_priv *priv = dev_get_priv(dev);
	printf("DSI remove\n");

	clk_disable_bulk(&priv->bulk);

	generic_phy_power_off(&priv->phy);

	regulator_set_enable(priv->vdd_reg, false);

	return 0;
}

struct video_bridge_ops qcom_dsi_ops = {
	.attach = qcom_dsi_attach,
	.get_display_timing = qcom_dsi_get_display_timing,
	.set_backlight = qcom_dsi_set_backlight,
};

static const struct udevice_id qcom_dsi_ids[] = {
	{ .compatible = "qcom,mdss-dsi-ctrl"},
	{ }
};

U_BOOT_DRIVER(qcom_dsi) = {
	.name = "qcom-dsi",
	.id = UCLASS_VIDEO_BRIDGE,
	.of_match = qcom_dsi_ids,
	.bind = qcom_dsi_bind,
	.probe = qcom_dsi_probe,
	.remove	= qcom_dsi_remove,
	.ops = &qcom_dsi_ops,
	.priv_auto = sizeof(struct qcom_dsi_priv),
	.flags = DM_FLAG_LEAVE_PD_ON,
};
