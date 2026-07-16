// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2025 Linaro.Ltd - All Rights Reserved
 *
 */

#define pr_fmt(fmt) "[DSI] "fmt
#define LOG_CATEGORY UCLASS_VIDEO_BRIDGE
// #define LOG_DEBUG

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
#include <drm/display/drm_dsc_helper.h>

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

#define DSI_ERR_STATE_ACK			0x0000
#define DSI_ERR_STATE_TIMEOUT			0x0001
#define DSI_ERR_STATE_DLN0_PHY			0x0002
#define DSI_ERR_STATE_FIFO			0x0004
#define DSI_ERR_STATE_MDP_FIFO_UNDERFLOW	0x0008
#define DSI_ERR_STATE_INTERLEAVE_OP_CONTENTION	0x0010
#define DSI_ERR_STATE_PLL_UNLOCKED		0x0020

#define DSI_REG_FRAME_COUNT 0x398

/* for bridge */
struct qcom_dsi_priv {
	struct mipi_dsi_device device;
	void __iomem *hw_base;
	void __iomem *base;
	void __iomem *phy_base;
	struct udevice *panel;
	struct phy phy;
	struct phy_configure_opts_mipi_dphy phy_opts;
	u64 pixel_clk_rate;
	u64 byte_clk_rate;
	unsigned long byte_intf_clk_rate;
	int lane_min_kbps;
	int lane_max_kbps;
	unsigned int lane_mbps; /* per lane */
	struct udevice *vdd_reg;
	struct udevice *dsi_host;
	// struct clk_bulk bulk;
	struct clk byte_clk;
	struct clk byte_intf_clk;
	struct clk pixel_clk;
	struct clk esc_clk;
	struct clk iface_clk;
	struct clk bus_clk;
	struct display_timing timings;
	int id;
};

/* for dsi host */
struct qcom_mipi_dsi {
	struct mipi_dsi_host dsi_host;
	struct mipi_dsi_device *device;
	struct drm_dsc_config *dsc;
	struct cyclic_info cyclic;
	unsigned int dsc_slice_per_pkt;
	u32 err_work_state;
	u32 fifo_err;
	void __iomem *base;
	int id;
	unsigned int lane_mbps; /* per lane */
	u32 channel;
	unsigned int max_data_lanes;
	const struct mipi_dsi_phy_ops *phy_ops;
	struct msm_dsi_cfg_handler cfg_hnd;
	phys_addr_t tx_buf;
	u8 *rx_buf;
};

struct {
	struct mipi_dsi_host *dsi[2];
	bool bonded_dsi; // dual-DSI mode with a single panel
	bool sync_needed; // Send DCS commands to both DSI links in-sync
} qcom_dsi_globals;

#define DSI_0 0
#define DSI_1 1

#define DSI_CLOCK_MASTER	DSI_0
#define DSI_CLOCK_SLAVE		DSI_1

#define DSI_ID(dsi) (dsi->id)
#define IS_SYNC_NEEDED() (qcom_dsi_globals.sync_needed)
#define IS_BONDED_DSI() (qcom_dsi_globals.bonded_dsi)
#define GET_DSI(id) (qcom_dsi_globals.dsi[id])

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

static void dsi_intr_ctrl(struct qcom_mipi_dsi *msm_host, u32 mask, int enable)
{
	u32 intr;
	unsigned long flags;

	spin_lock_irqsave(&msm_host->intr_lock, flags);
	intr = dsi_read(msm_host->base + REG_DSI_INTR_CTRL);

	if (enable)
		intr |= mask;
	else
		intr &= ~mask;

	// dev_dbg(msm_host->dsi_host.dev, "intr=%x enable=%d\n", intr, enable);

	dsi_write(msm_host->base + REG_DSI_INTR_CTRL, intr);
	spin_unlock_irqrestore(&msm_host->intr_lock, flags);
}

#define REG_DSI_VBIF_CTRL	(0x01C8)
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

		if (!(val & (DSI_STATUS0_CMD_MODE_ENGINE_BUSY)))
			break;

		++count;
	}

	// if (val & DSI_STATUS0_VIDEO_MODE_ENGINE_BUSY) {
	// 	dev_err(host->dev, "failed to wait for video engine busy (%x)\n", val);
	// 	return -EIO;
	// }
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
	dsi_write(base + REG_DSI_VBIF_CTRL, 0x7);

	// Unmask
	dsi_write(base + REG_DSI_INTR_CTRL, DSI_IRQ_MASK_CMD_DMA_DONE);
	// Clear
	dsi_write(base + REG_DSI_INTR_CTRL, DSI_IRQ_MASK_CMD_DMA_DONE | DSI_IRQ_CMD_DMA_DONE);


	dsi_write(base + REG_DSI_TRIG_DMA, 0x1);

	pr_debug("%s: %#x (ret=%d)\n", __func__, data[0], ret);
	return ret < 0 ? ret : packet.size;
}

static int dsi_tx_wait_done(struct mipi_dsi_host *host)
{
	struct qcom_mipi_dsi *dsi = host_to_dsi(host);
	void __iomem *base = dsi->base;
	int ret, count;
	u32 val;

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
	dsi_write(base + REG_DSI_INTR_CTRL, DSI_IRQ_MASK_CMD_DMA_DONE | DSI_IRQ_CMD_DMA_DONE);
	// Mask
	dsi_write(base + REG_DSI_INTR_CTRL, 0);

	return 0;
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

	// debug("Dsi RX\n");
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

		ret = dsi_tx_wait_done(host);
		if (ret < 0) {
			pr_err("%s: Set max pkt size failed tx_wait_done, %d\n",
				__func__, ret);
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

		ret = dsi_tx_wait_done(host);
		if (ret < 0) {
			pr_err("%s: Read cmd Tx failed tx_wait_done, %d\n",
				__func__, ret);
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
		debug("%s:Invalid response cmd 0x%x\n", __func__, cmd);
		ret = 0;
	}

	return ret;
}

static ssize_t qcom_mipi_dsi_host_transfer(struct mipi_dsi_host *host,
					 const struct mipi_dsi_msg *msg)
{
	struct qcom_mipi_dsi *dsi = host_to_dsi(host);
	struct mipi_dsi_host *host0 = GET_DSI(DSI_0);
	struct qcom_mipi_dsi *dsi0 = host_to_dsi(host0);
	int ret;
	u32 save_ctrl, save_ctrl0;

	/*
	 * In the synchronous case we send the command over both DSI
	 * links when it is sent over DSI 1 and and ignore DSI 0.
	 */
	if (IS_SYNC_NEEDED() && DSI_ID(dsi) == 0)
		return msg->rx_len ?: msg->tx_len;

	/* from msm_dsi_host_xfer_prepare() */
	if (!(msg->flags & MIPI_DSI_MSG_USE_LPM)) {
		dsi_set_tx_power_mode(0, dsi);
		if (IS_SYNC_NEEDED() && host0)
			dsi_set_tx_power_mode(0, dsi0);
	}

	save_ctrl = dsi_read(dsi->base + REG_DSI_CTRL);
	dsi_write(dsi->base + REG_DSI_CTRL, save_ctrl | DSI_CTRL_CMD_MODE_EN);

	if (IS_SYNC_NEEDED() && host0) {
		save_ctrl0 = dsi_read(dsi0->base + REG_DSI_CTRL);
		dsi_write(dsi0->base + REG_DSI_CTRL, save_ctrl0 | DSI_CTRL_CMD_MODE_EN);
	}

	if (msg->rx_buf && msg->rx_len) {
		ret = msm_dsi_host_cmd_rx(host, msg);
		if (ret)
			goto out;
		ret = msg->rx_len;
	} else {
		ret = dsi_cmds2buf_tx(host, msg);
		if (IS_SYNC_NEEDED() && host0 && ret >= 0)
			ret = dsi_cmds2buf_tx(host0, msg);

		ret = dsi_tx_wait_done(host);
		if (ret)
			dev_err(host0->dev, "DSI %d: Failed waiting for TX done: %d\n", DSI_ID(dsi), ret);

		if (IS_SYNC_NEEDED() && host0) {
			ret = dsi_tx_wait_done(host0);
			if (ret)
				dev_err(host0->dev, "DSI 0: Failed waiting for TX done: %d\n", ret);
		}
	}

out:
	dsi_write(dsi->base + REG_DSI_CTRL, save_ctrl);
	if (IS_SYNC_NEEDED() && host0)
		dsi_write(dsi0->base + REG_DSI_CTRL, save_ctrl0);

	/* from msm_dsi_host_xfer_restore() */
	if (!(msg->flags & MIPI_DSI_MSG_USE_LPM)) {
		dsi_set_tx_power_mode(1, dsi);
		if (IS_SYNC_NEEDED() && host0)
			dsi_set_tx_power_mode(1, dsi0);
	}

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

bool regdump_read_decode(u64 addr);

static void qcom_dsi_dump_regs(struct qcom_mipi_dsi *host)
{
	printf("\n====== DSI%d ======\n", host->id);
	regdump_read_decode((u64)host->base + REG_DSI_STATUS0);
	regdump_read_decode((u64)host->base + REG_DSI_FIFO_STATUS);
	printf("\n");
	regdump_read_decode((u64)host->base + REG_DSI_ACK_ERR_STATUS);
	regdump_read_decode((u64)host->base + REG_DSI_ACTIVE_H);
	regdump_read_decode((u64)host->base + REG_DSI_ACTIVE_HSYNC);
	regdump_read_decode((u64)host->base + REG_DSI_ACTIVE_V);
	regdump_read_decode((u64)host->base + REG_DSI_ACTIVE_VSYNC_HPOS);
	regdump_read_decode((u64)host->base + REG_DSI_ACTIVE_VSYNC_VPOS);
	regdump_read_decode((u64)host->base + REG_DSI_CLK_CTRL);
	regdump_read_decode((u64)host->base + REG_DSI_CLKOUT_TIMING_CTRL);
	regdump_read_decode((u64)host->base + REG_DSI_CLK_STATUS);
	regdump_read_decode((u64)host->base + REG_DSI_CMD_CFG0);
	regdump_read_decode((u64)host->base + REG_DSI_CMD_CFG1);
	regdump_read_decode((u64)host->base + REG_DSI_CMD_DMA_CTRL);
	regdump_read_decode((u64)host->base + REG_DSI_CMD_MDP_STREAM0_CTRL);
	regdump_read_decode((u64)host->base + REG_DSI_CMD_MDP_STREAM0_TOTAL);
	regdump_read_decode((u64)host->base + REG_DSI_CMD_MODE_MDP_CTRL2);
	regdump_read_decode((u64)host->base + REG_DSI_COMMAND_COMPRESSION_MODE_CTRL);
	regdump_read_decode((u64)host->base + REG_DSI_COMMAND_COMPRESSION_MODE_CTRL2);
	regdump_read_decode((u64)host->base + REG_DSI_CPHY_MODE_CTRL);
	regdump_read_decode((u64)host->base + REG_DSI_CTRL);
	regdump_read_decode((u64)host->base + REG_DSI_DLN0_PHY_ERR);
	regdump_read_decode((u64)host->base + REG_DSI_LANE_STATUS);
	regdump_read_decode((u64)host->base + REG_DSI_DMA_BASE);
	regdump_read_decode((u64)host->base + REG_DSI_DMA_LEN);
	regdump_read_decode((u64)host->base + REG_DSI_EOT_PACKET_CTRL);
	regdump_read_decode((u64)host->base + REG_DSI_ERR_INT_MASK0);
	regdump_read_decode((u64)host->base + REG_DSI_INTR_CTRL);
	regdump_read_decode((u64)host->base + REG_DSI_LANE_CTRL);
	regdump_read_decode((u64)host->base + REG_DSI_LANE_SWAP_CTRL);
	regdump_read_decode((u64)host->base + REG_DSI_T_CLK_PRE_EXTEND);
	regdump_read_decode((u64)host->base + REG_DSI_TEST_PATTERN_GEN_CTRL);
	regdump_read_decode((u64)host->base + REG_DSI_TIMEOUT_STATUS);
	regdump_read_decode((u64)host->base + REG_DSI_TOTAL);
	regdump_read_decode((u64)host->base + REG_DSI_TRIG_CTRL);
	regdump_read_decode((u64)host->base + REG_DSI_TRIG_DMA);
	regdump_read_decode((u64)host->base + REG_DSI_VBIF_CTRL);
	regdump_read_decode((u64)host->base + REG_DSI_VID_CFG0);
	regdump_read_decode((u64)host->base + REG_DSI_VID_CFG1);
	regdump_read_decode((u64)host->base + REG_DSI_VIDEO_COMPRESSION_MODE_CTRL);

	printf("\n");
}

void qcom_dsi_do_dump(struct udevice *dev)
{
	struct qcom_mipi_dsi *host = dev_get_priv(dev);

	printf("\n====== DSI%d ======\n", host->id);
	printf("Frame count: %u\n", dsi_read(host->base + DSI_REG_FRAME_COUNT));
	printf("C-PHY mode: %d\n", dsi_read(host->base + REG_DSI_CPHY_MODE_CTRL));
	qcom_dsi_dump_regs(host);
	// regdump_read_decode((u64)host->base + REG_DSI_ERR_INT_MASK0);
	// regdump_read_decode((u64)host->base + REG_DSI_FIFO_STATUS);
	// regdump_read_decode((u64)host->base + REG_DSI_STATUS0);
	// regdump_read_decode((u64)host->base + REG_DSI_DLN0_PHY_ERR);
}

bool msm_dsi_host_is_wide_bus_enabled(struct qcom_mipi_dsi *priv)
{
	return priv->dsc &&
		(priv->cfg_hnd.major == MSM_DSI_VER_MAJOR_6G &&
		 priv->cfg_hnd.minor >= MSM_DSI_6G_VER_MINOR_V2_5_0);
}

/**** PHY_OPS *****/

// bool msm_dsi_phy_set_continuous_clock(struct msm_dsi_phy *phy, bool enable)
// {
// 	if (!phy || !phy->cfg->ops.set_continuous_clock)
// 		return false;

// 	return phy->cfg->ops.set_continuous_clock(phy, enable);
// }

/**** PHY_OPS_END *****/

static void dsi_update_dsc_timing(struct qcom_mipi_dsi *msm_host, bool is_cmd_mode)
{
	struct drm_dsc_config *dsc = msm_host->dsc;
	u32 reg, reg_ctrl, reg_ctrl2;
	u32 slice_per_intf, total_bytes_per_intf;
	u32 pkt_per_line;
	u32 eol_byte_num;
	u32 bytes_per_pkt;

	dev_dbg(msm_host->device->dev, "%s\n", __func__);

	/* first calculate dsc parameters and then program
	 * compress mode registers
	 */
	slice_per_intf = dsc->slice_count;

	total_bytes_per_intf = dsc->slice_chunk_size * slice_per_intf;
	bytes_per_pkt = dsc->slice_chunk_size * msm_host->dsc_slice_per_pkt;

	eol_byte_num = total_bytes_per_intf % 3;
	pkt_per_line = slice_per_intf / msm_host->dsc_slice_per_pkt;

	if (is_cmd_mode) /* packet data type */
		reg = DSI_COMMAND_COMPRESSION_MODE_CTRL_STREAM0_DATATYPE(MIPI_DSI_DCS_LONG_WRITE);
	else
		reg = DSI_VIDEO_COMPRESSION_MODE_CTRL_DATATYPE(MIPI_DSI_COMPRESSED_PIXEL_STREAM);

	/* DSI_VIDEO_COMPRESSION_MODE & DSI_COMMAND_COMPRESSION_MODE
	 * registers have similar offsets, so for below common code use
	 * DSI_VIDEO_COMPRESSION_MODE_XXXX for setting bits
	 *
	 * pkt_per_line is log2 encoded, >>1 works for supported values (1,2,4)
	 */
	if (pkt_per_line > 4)
		dev_warn(msm_host->device->dev, "pkt_per_line too big");
	reg |= DSI_VIDEO_COMPRESSION_MODE_CTRL_PKT_PER_LINE(pkt_per_line >> 1);
	reg |= DSI_VIDEO_COMPRESSION_MODE_CTRL_EOL_BYTE_NUM(eol_byte_num);
	reg |= DSI_VIDEO_COMPRESSION_MODE_CTRL_EN;

	if (is_cmd_mode) {
		reg_ctrl = dsi_read(msm_host->base + REG_DSI_COMMAND_COMPRESSION_MODE_CTRL);
		reg_ctrl2 = dsi_read(msm_host->base + REG_DSI_COMMAND_COMPRESSION_MODE_CTRL2);

		reg_ctrl &= ~0xffff;
		reg_ctrl |= reg;

		reg_ctrl2 &= ~DSI_COMMAND_COMPRESSION_MODE_CTRL2_STREAM0_SLICE_WIDTH__MASK;
		reg_ctrl2 |= DSI_COMMAND_COMPRESSION_MODE_CTRL2_STREAM0_SLICE_WIDTH(dsc->slice_chunk_size);

		dsi_write(msm_host->base + REG_DSI_COMMAND_COMPRESSION_MODE_CTRL, reg_ctrl);
		dsi_write(msm_host->base + REG_DSI_COMMAND_COMPRESSION_MODE_CTRL2, reg_ctrl2);
	} else {
		reg |= DSI_VIDEO_COMPRESSION_MODE_CTRL_WC(bytes_per_pkt);
		dsi_write(msm_host->base + REG_DSI_VIDEO_COMPRESSION_MODE_CTRL, reg);
	}
}

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
		dsi_write(priv->base + REG_DSI_VID_CFG1, data);
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

	// FIXME: ????
	if ((cfg_hnd->major == MSM_DSI_VER_MAJOR_6G) &&
		(cfg_hnd->minor >= MSM_DSI_6G_VER_MINOR_V1_2))
		data |= DSI_TRIG_CTRL_BLOCK_DMA_WITHIN_FRAME;
	dsi_write(priv->base + REG_DSI_TRIG_CTRL, data);

	data = DSI_CLKOUT_TIMING_CTRL_T_CLK_POST(phy_shared_timings->clk_post) |
		DSI_CLKOUT_TIMING_CTRL_T_CLK_PRE(phy_shared_timings->clk_pre);
	dev_dbg(device->dev, "Writing phy clout timings: (post %u, pre %u)\n", phy_shared_timings->clk_post, phy_shared_timings->clk_pre);
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

	/* U-Boot: disable all interrupts except error */
	dsi_write(priv->base + REG_DSI_INTR_CTRL, DSI_IRQ_MASK_ERROR);
	// dsi_intr_ctrl(priv, DSI_IRQ_MASK_ERROR, 1);
	// dsi_intr_ctrl(priv, UINT32_MAX, 0);

	dsi_write(priv->base + REG_DSI_CLK_CTRL, DSI_CLK_CTRL_ENABLE_CLKS);

	data = DSI_CTRL_CLK_EN;

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

	if (phy->cphy_mode)
		dsi_write(priv->base + REG_DSI_CPHY_MODE_CTRL, BIT(0));


	// qcom_dsi_dump_regs(priv);
}

static int dsi_populate_dsc_params(struct qcom_mipi_dsi *msm_host, struct drm_dsc_config *dsc)
{
	int ret;

	if (dsc->bits_per_pixel & 0xf) {
		dev_err(msm_host->device->dev, "DSI does not support fractional bits_per_pixel\n");
		return -EINVAL;
	}

	switch (dsc->bits_per_component) {
	case 8:
	case 10:
	case 12:
		/*
		 * Only 8, 10, and 12 bpc are supported for DSC 1.1 block.
		 * If additional bpc values need to be supported, update
		 * this quard with the appropriate DSC version verification.
		 */
		break;
	default:
		dev_err(msm_host->device->dev,
			      "Unsupported bits_per_component value: %d\n",
			      dsc->bits_per_component);
		return -EOPNOTSUPP;
	}

	dsc->simple_422 = 0;
	dsc->convert_rgb = 1;
	dsc->vbr_enable = 0;

	drm_dsc_set_const_params(dsc);
	drm_dsc_set_rc_buf_thresh(dsc);

	/* DPU supports only pre-SCR panels */
	ret = drm_dsc_setup_rc_params(dsc, DRM_DSC_1_1_PRE_SCR);
	if (ret) {
		dev_err(msm_host->device->dev, "could not find DSC RC parameters\n");
		return ret;
	}

	dsc->initial_scale_value = drm_dsc_initial_scale_value(dsc);
	dsc->line_buf_depth = dsc->bits_per_component + 1;

	return drm_dsc_compute_rc_parameters(dsc);
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

#define dsi_timing_htotal(t, hactive) (hactive + t->hfront_porch.typ + t->hback_porch.typ + t->hsync_len.typ)

static void dsi_timing_setup(struct qcom_mipi_dsi *dsi, struct display_timing *timings)
{
	void __iomem *base = dsi->base;
	u32 hs_start = 0, vs_start = 0; /* take sync start as 0 */
	u32 hactive = timings->hactive.typ;
	bool wide_bus_enabled = msm_dsi_host_is_wide_bus_enabled(dsi);
	int ret;

	// 1080 + 20 + 20 + 2
	u32 h_total = dsi_timing_htotal(timings, hactive);
	u32 v_total = timings->vactive.typ + timings->vfront_porch.typ + timings->vback_porch.typ + timings->vsync_len.typ;

	// (1080 + 20 + 2) - (1080 + 20) = 2 ~= hs_start + hs_len = 0 + 2
	u32 hs_end = timings->hsync_len.typ;
	u32 vs_end = timings->vsync_len.typ;

	// (1080 + 20 + 2 + 20) - (1080 + 18) = 20 ~= hs_start + hs_len + hback_porch = 0 + 2 + 18 = 20
	u32 ha_start = timings->hsync_len.typ + timings->hback_porch.typ;
	u32 ha_end = ha_start + hactive;
	u32 va_start = timings->vsync_len.typ + timings->vback_porch.typ;
	u32 va_end = va_start + timings->vactive.typ;
	u32 hdisplay = hactive;
	u32 wc;

	if (IS_BONDED_DSI()) {
		h_total /= 2;
		hs_end /= 2;
		ha_start /= 2;
		ha_end /= 2;
		hdisplay /= 2;
	}

	if (dsi->dsc) {
		struct drm_dsc_config *dsc = dsi->dsc;
		u32 bytes_per_pclk;

		/* update dsc params with timing params */
		if (!dsc || !timings->hactive.typ || !timings->vactive.typ) {
			pr_err("DSI: invalid input: pic_width: %d pic_height: %d\n",
			       timings->hactive.typ, timings->vactive.typ);
			return;
		}

		dsc->pic_width = hdisplay;
		dsc->pic_height = timings->vactive.typ;
		dev_dbg(dsi->device->dev, "%s: DSC Mode %dx%d\n", __func__, dsc->pic_width, dsc->pic_height);

		/* we do the calculations for dsc parameters here so that
		 * panel can use these parameters
		 */
		ret = dsi_populate_dsc_params(dsi, dsc);
		if (ret) {
			dev_err(dsi->dsi_host.dev, "Couldn't populate DSC params! %d\n", ret);
			return;
		}

		/*
		 * DPU sends 3 bytes per pclk cycle to DSI. If widebus is
		 * enabled, bus width is extended to 6 bytes.
		 *
		 * Calculate the number of pclks needed to transmit one line of
		 * the compressed data.

		 * The back/font porch and pulse width are kept intact. For
		 * VIDEO mode they represent timing parameters rather than
		 * actual data transfer, see the documentation for
		 * dsi_adjust_pclk_for_compression(). For CMD mode they are
		 * unused anyway.
		 */
		h_total -= hdisplay;
		if (wide_bus_enabled && !(dsi->device->mode_flags & MIPI_DSI_MODE_VIDEO))
			bytes_per_pclk = 6;
		else
			bytes_per_pclk = 3;

		hdisplay = DIV_ROUND_UP(msm_dsc_get_bytes_per_line(dsi->dsc), bytes_per_pclk);

		h_total += hdisplay;
		ha_end = ha_start + hdisplay;
	}

	if (dsi->device->mode_flags & MIPI_DSI_MODE_VIDEO) {
		if (dsi->dsc)
			dsi_update_dsc_timing(dsi, false);

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
		if (dsi->dsc)
			dsi_update_dsc_timing(dsi, true);

		/* image data and 1 byte write_memory_start cmd */
		if (!dsi->dsc)
			wc = hdisplay * mipi_dsi_pixel_format_to_bpp(dsi->device->format) / 8 + 1;
		else
			/*
			 * When DSC is enabled, WC = slice_chunk_size * slice_per_pkt + 1.
			 */
			wc = dsi->dsc->slice_chunk_size * dsi->dsc_slice_per_pkt + 1;

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
	dsi_write(dsi->base + REG_DSI_RESET, 0); // reset during FIFO oflow, video start on vsync
	wmb(); /* controller out of reset */

	if (ctrl & DSI_CTRL_ENABLE) {
		dsi_write(dsi->base + REG_DSI_CTRL, ctrl);
		wmb();	/* make sure dsi controller enabled again */
	}
}

/* error polling */
static void dsi_err_worker(struct qcom_mipi_dsi *msm_host)
{
	u32 status = msm_host->err_work_state;

	// dev_dbg(msm_host->dsi_host.dev, "err status=%#x (fifo status=%#x)\n", status, msm_host->fifo_err);
	if (status & DSI_ERR_STATE_MDP_FIFO_UNDERFLOW) {
		dev_warn(msm_host->dsi_host.dev, "Doing soft reset\n");
		dsi_sw_reset(msm_host);
	}

	/* It is safe to clear here because error irq is disabled. */
	msm_host->err_work_state = 0;

	/* enable dsi error interrupt */
	dsi_intr_ctrl(msm_host, DSI_IRQ_MASK_ERROR, 1);
}

static void dsi_ack_err_status(struct qcom_mipi_dsi *msm_host)
{
	u32 status;

	status = dsi_read(msm_host->base + REG_DSI_ACK_ERR_STATUS);

	if (status) {
		dsi_write(msm_host->base + REG_DSI_ACK_ERR_STATUS, status);
		/* Writing of an extra 0 needed to clear error bits */
		dsi_write(msm_host->base + REG_DSI_ACK_ERR_STATUS, 0);
		msm_host->err_work_state |= DSI_ERR_STATE_ACK;
	}
}

static void dsi_timeout_status(struct qcom_mipi_dsi *msm_host)
{
	u32 status;

	status = dsi_read(msm_host->base + REG_DSI_TIMEOUT_STATUS);

	if (status) {
		dsi_write(msm_host->base + REG_DSI_TIMEOUT_STATUS, status);
		msm_host->err_work_state |= DSI_ERR_STATE_TIMEOUT;
	}
}

static void dsi_dln0_phy_err(struct qcom_mipi_dsi *msm_host)
{
	u32 status;

	status = dsi_read(msm_host->base + REG_DSI_DLN0_PHY_ERR);

	if (status & (DSI_DLN0_PHY_ERR_DLN0_ERR_ESC |
			DSI_DLN0_PHY_ERR_DLN0_ERR_SYNC_ESC |
			DSI_DLN0_PHY_ERR_DLN0_ERR_CONTROL |
			DSI_DLN0_PHY_ERR_DLN0_ERR_CONTENTION_LP0 |
			DSI_DLN0_PHY_ERR_DLN0_ERR_CONTENTION_LP1)) {
		dsi_write(msm_host->base + REG_DSI_DLN0_PHY_ERR, status);
		msm_host->err_work_state |= DSI_ERR_STATE_DLN0_PHY;
	}
}

static void dsi_fifo_status(struct qcom_mipi_dsi *msm_host)
{
	u32 status;

	status = dsi_read(msm_host->base + REG_DSI_FIFO_STATUS);

	/* fifo underflow, overflow */
	if (status) {
		dsi_write(msm_host->base + REG_DSI_FIFO_STATUS, status);
		msm_host->err_work_state |= DSI_ERR_STATE_FIFO;
		msm_host->fifo_err = status;
		if (status & DSI_FIFO_STATUS_CMD_MDP_FIFO_UNDERFLOW)
			msm_host->err_work_state |=
					DSI_ERR_STATE_MDP_FIFO_UNDERFLOW;
	} else {
		msm_host->fifo_err = 0;
	}
}

static void dsi_status(struct qcom_mipi_dsi *msm_host)
{
	u32 status;

	status = dsi_read(msm_host->base + REG_DSI_STATUS0);

	if (status & DSI_STATUS0_INTERLEAVE_OP_CONTENTION) {
		dsi_write(msm_host->base + REG_DSI_STATUS0, status);
		msm_host->err_work_state |=
			DSI_ERR_STATE_INTERLEAVE_OP_CONTENTION;
	}
}

static void dsi_clk_status(struct qcom_mipi_dsi *msm_host)
{
	u32 status;

	status = dsi_read(msm_host->base + REG_DSI_CLK_STATUS);

	if (status & DSI_CLK_STATUS_PLL_UNLOCKED) {
		dsi_write(msm_host->base + REG_DSI_CLK_STATUS, status);
		msm_host->err_work_state |= DSI_ERR_STATE_PLL_UNLOCKED;
	}
}

/* Some error states can just be cleared and the hw will recover */
void qcom_dsi_video_sync(struct udevice *dsi_bridge)
{
	struct qcom_dsi_priv *bpriv = dev_get_priv(dsi_bridge);
	struct qcom_mipi_dsi *msm_host;

	if (!bpriv->dsi_host)
		return;

	msm_host = dev_get_priv(bpriv->dsi_host);

	// if (msm_host->fifo_err) {
	// 	dev_err(msm_host->dsi_host.dev, "Stopping err poll\n");
	// 	cyclic_unregister(cyclic);
	// }

	/* disable dsi error interrupt */
	dsi_intr_ctrl(msm_host, DSI_IRQ_MASK_ERROR, 0);

	dsi_clk_status(msm_host);
	dsi_fifo_status(msm_host);
	dsi_ack_err_status(msm_host);
	dsi_timeout_status(msm_host);
	dsi_status(msm_host);
	dsi_dln0_phy_err(msm_host);

	dsi_err_worker(msm_host);
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
	dsi->dsc = device->dsc;
	dsi->dsc_slice_per_pkt = device->dsc_slice_per_pkt;
	dsi->dsi_host.dev = dev;
	dsi->dsi_host.ops = &qcom_mipi_dsi_host_ops;
	device->host = &dsi->dsi_host;

	dsi->tx_buf = 0xf0000000;

	ret = lmb_alloc_mem(LMB_MEM_ALLOC_MAX, 0x1000, &dsi->tx_buf, 0x1000, LMB_NONE);
	if (ret) {
		dev_err(device->dev, "Failed to allocate tx_buf @ %#llx!\n", dsi->tx_buf);
	}
	memset((void *)dsi->tx_buf, 0xff, 0x1000);
	dsi->rx_buf = malloc(0x1000);

	/* For bonded DSI the primary DSI controller initialises everything */
	if (IS_BONDED_DSI() && dsi->id != DSI_0)
		return 0;

	if (IS_BONDED_DSI() && !GET_DSI(DSI_1)) {
		dev_err(dev, "DSI1 not probed yet!\n");
		return -EAGAIN;
	}

	ret = phy_ops->init(device);
	if (ret)
		return ret;

	/* Ugly poke into through the DSI bridge device to the phy */
	struct qcom_dsi_priv *bridge = (struct qcom_dsi_priv *)(device);
	struct msm_dsi_phy *phy = dev_get_priv(bridge->phy.dev);

	dsi_timing_setup(dsi, timings);
	dsi_sw_reset(dsi);
	dsi_ctrl_enable(dsi, &phy->timing.shared_timings, phy);

	if (IS_BONDED_DSI()) {
		struct qcom_mipi_dsi *sdsi = host_to_dsi(GET_DSI(DSI_1));
		struct qcom_dsi_priv *sdsi_bridge = (struct qcom_dsi_priv *)(host_to_dsi(GET_DSI(DSI_1))->device);
		struct msm_dsi_phy *sdsi_phy = dev_get_priv(sdsi_bridge->phy.dev);

		dev_dbg(sdsi->device->dev, "Bonded DSI init!\n");

		dsi_timing_setup(sdsi, timings);
		dsi_sw_reset(sdsi);
		dsi_ctrl_enable(sdsi, &sdsi_phy->timing.shared_timings, sdsi_phy);
	}

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

	dsi->id = dev_seq(dev);

	qcom_dsi_globals.dsi[dsi->id] = &dsi->dsi_host;
	dev_info(dev, "%s\n", __func__);

	qcom_dsi_globals.bonded_dsi |= dev_read_bool(dev, "qcom,dual-dsi-mode");
	qcom_dsi_globals.sync_needed |= dev_read_bool(dev, "qcom,sync-dual-dsi");

	/* check hardware version */
	dsi_get_version(dsi->base, &dsi->cfg_hnd.major, &dsi->cfg_hnd.minor);
	dev_dbg(dev, "DSI version %#x.%#x\n", dsi->cfg_hnd.major, dsi->cfg_hnd.minor);
	if (dsi->cfg_hnd.minor != MSM_DSI_6G_VER_MINOR_V2_8_0) { // sm8650
		dev_err(dev, "DSI version %#x.%#x not supported\n", dsi->cfg_hnd.major, dsi->cfg_hnd.minor);
		dev_dbg(dev, "remove and unbind all DSI child\n");
		device_chld_remove(dev, NULL, DM_REMOVE_NORMAL);
		device_chld_unbind(dev, NULL);
		return -ENODEV;
	}

	dsi->base += 4;

	dev_dbg(dev, "DSI %d host probe\n", dsi->id);

	return 0;
}

static int qcom_mipi_dsi_remove(struct udevice *dev)
{
	struct qcom_mipi_dsi *dsi = dev_get_priv(dev);
	
	dev_dbg(dev, "%s\n", __func__);

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

/**
 * dsi_adjust_pclk_for_compression() - Adjust the pclk rate for compression case
 * @mode: The selected mode for the DSI output
 * @dsc: DRM DSC configuration for this DSI output
 *
 * Adjust the pclk rate by calculating a new hdisplay proportional to
 * the compression ratio such that:
 *     new_hdisplay = old_hdisplay * compressed_bpp / uncompressed_bpp
 *
 * Porches do not need to be adjusted:
 * - For VIDEO mode they are not compressed by DSC and are passed as is.
 * - For CMD mode there are no actual porches. Instead these fields
 *   currently represent the overhead to the image data transfer. As such, they
 *   are calculated for the final mode parameters (after the compression) and
 *   are not to be adjusted too.
 *
 *  FIXME: Reconsider this if/when CMD mode handling is rewritten to use
 *  transfer time and data overhead as a starting point of the calculations.
 */
static unsigned long dsi_adjust_pclk_for_compression(const struct display_timing *timings,
		const struct drm_dsc_config *dsc)
{
	int hdisplay, new_hdisplay, new_htotal, htotal;

	/*
	 * For bonded DSI, split hdisplay across two links and round up each
	 * half separately, passing the full hdisplay would only round up once.
	 * This also aligns with the hdisplay we program later in
	 * dsi_timing_setup()
	 */
	hdisplay = timings->hactive.typ;
	if (IS_BONDED_DSI())
		hdisplay /= 2;

	new_hdisplay = DIV_ROUND_UP(hdisplay * drm_dsc_get_bpp_int(dsc),
				    dsc->bits_per_component * 3);

	if (IS_BONDED_DSI())
		new_hdisplay *= 2;

	htotal = dsi_timing_htotal(timings, timings->hactive.typ);
	new_htotal = htotal - timings->hactive.typ + new_hdisplay;

	return mult_frac(timings->pixelclock.typ, new_htotal, htotal);

	// int new_hdisplay = DIV_ROUND_UP(timings->hactive.typ * drm_dsc_get_bpp_int(dsc),
	// 		dsc->bits_per_component * 3);

	// int htotal = dsi_timing_htotal(timings, timings->hactive.typ);
	// int new_htotal = htotal - timings->hactive.typ + new_hdisplay;
	// pr_info("PCLK-DSC: htotal: %d new_htotal: %d\n", htotal, new_htotal);

	// return mult_frac(timings->pixelclock.typ * 1000u, new_htotal, htotal);
}

static int dsi_phy_calc_clk_req(struct qcom_dsi_priv *dsi, struct msm_dsi_phy_clk_request *clk_req)
{
	struct mipi_dsi_panel_plat *mplat = dev_get_plat(dsi->panel);
	struct msm_dsi_phy *phy = dev_get_priv(dsi->phy.dev);
	u32 pixelclock = dsi->timings.pixelclock.typ;
	u32 bpp = mipi_dsi_pixel_format_to_bpp(mplat->format);
	u64 pclk_rate = pixelclock;
	u64 byteclk_rate;

	if (dsi->device.dsc)
		pclk_rate = dsi_adjust_pclk_for_compression(&dsi->timings, dsi->device.dsc);

	if (IS_BONDED_DSI())
		pclk_rate /= 2;

	// printf("FORCING PCLK RATE %u (calculated %llu)\n", 243149748, pclk_rate);
	// 	 // 144218277
	// pclk_rate = 243149748;

	if (phy->cphy_mode)
		byteclk_rate = mult_frac(pclk_rate, bpp, 16 * mplat->lanes);
	else
		byteclk_rate = mult_frac(pclk_rate, bpp, 8 * mplat->lanes);

	dsi->pixel_clk_rate = pclk_rate;
	dsi->byte_clk_rate = byteclk_rate;

	if (phy->cphy_mode)
		clk_req->bitclk_rate = byteclk_rate * 7;
	else
		clk_req->bitclk_rate = byteclk_rate * 8;

	clk_req->escclk_rate = 19200000; // FIXME: less would be nice perhaps but eh

	dev_dbg(dsi->device.dev, "%s: calculated pclk: %llu bitclk: %lu\n", __func__, pclk_rate, clk_req->bitclk_rate);

	return 0;
}

static int dsi_host_reset_phy(void *base)
{
	dsi_write(base + REG_DSI_PHY_RESET, 0x1);
	wmb();
	mdelay(1);
	dsi_write(base + REG_DSI_PHY_RESET, 0x0);
	mdelay(1);

	return 0;
}

static int enable_phy(struct qcom_dsi_priv *msm_dsi,
		      struct msm_dsi_phy_shared_timings *shared_timings)
{
	struct msm_dsi_phy *phy = dev_get_priv(msm_dsi->phy.dev);
	struct msm_dsi_phy_clk_request clk_req;

	dsi_phy_calc_clk_req(msm_dsi, &clk_req);

	return msm_dsi_phy_enable(phy, &clk_req, shared_timings);
}

static int dsi_enable_clocks(struct qcom_dsi_priv *dsi, struct msm_dsi_phy_shared_timings *shared_timings)
{
	int ret;

	dev_dbg(dsi->device.dev, "DSI%d %s\n", dsi->id, __func__);

	dsi->byte_intf_clk_rate = dsi->byte_clk_rate;
	if (shared_timings->byte_intf_clk_div_2)
		dsi->byte_intf_clk_rate /= 2;

	// clk_set_rate(&dsi->esc_clk, 19200000);
	if (clk_set_rate(&dsi->byte_clk, dsi->byte_clk_rate) < 0) {
		dev_err(dsi->device.dev, "clk_set_rate(byte_clk) failed!\n");
		return -EINVAL;
	}
	if (clk_set_rate(&dsi->byte_intf_clk, dsi->byte_intf_clk_rate) < 0) {
		dev_err(dsi->device.dev, "clk_set_rate(byte_intf_clk) failed!\n");
		return -EINVAL;
	}
	if (clk_set_rate(&dsi->pixel_clk, dsi->pixel_clk_rate) < 0) {
		dev_err(dsi->device.dev, "clk_set_rate(pixel_clk) failed!\n");
		return -EINVAL;
	}

	ret = clk_enable(&dsi->esc_clk);
	if (ret < 0) {
		dev_err(dsi->device.dev, "clk_enable(esc_clk) failed! %d\n", ret);
		return ret;
	}
	ret = clk_enable(&dsi->byte_clk);
	if (ret < 0) {
		dev_err(dsi->device.dev, "clk_enable(byte_clk) failed! %d\n", ret);
		return ret;
	}
	ret = clk_enable(&dsi->byte_intf_clk);
	if (ret < 0) {
		dev_err(dsi->device.dev, "clk_enable(byte_intf_clk) failed! %d\n", ret);
		return ret;
	}
	ret = clk_enable(&dsi->pixel_clk);
	if (ret < 0) {
		dev_err(dsi->device.dev, "clk_enable(pixel_clk) failed! %d\n", ret);
		return ret;
	}

	return 0;
}

static int dsi_phy_init(void *priv_data)
{
	struct mipi_dsi_device *device = priv_data;
	struct udevice *dev = device->dev;
	// FIXME: layering violation, this is bridge priv...
	struct qcom_dsi_priv *dsi = dev_get_priv(dev);
	struct qcom_dsi_priv *mdsi = dsi, *sdsi;
	struct msm_dsi_phy *phy = dev_get_priv(dsi->phy.dev);
	struct msm_dsi_phy *sdsi_phy; // = dev_get_priv(dsi->phy.dev);
	struct msm_dsi_phy_shared_timings shared_timings[DSI_MAX];
	int ret;

	if (IS_BONDED_DSI() && DSI_ID(dsi) != DSI_0) {
		dev_err(dev, "%s called for secondary DSI in bonded DSI mode!\n", __func__);
		return 0;
	}

	dev_dbg(dev, "DSI%d phy init\n", dsi->id);

	if (!generic_phy_valid(&dsi->phy)) {
		dev_dbg(dev, "fail to validate phy, phy %p phy->dev %p\n", &dsi->phy, dsi->phy.dev);
	}

	/* At this point for bonded DSI we are the primary DSI */
	if (IS_BONDED_DSI()) {
		sdsi = (struct qcom_dsi_priv *)(host_to_dsi(GET_DSI(DSI_1))->device);
		sdsi_phy = dev_get_priv(sdsi->phy.dev);
		phy->usecase = MSM_DSI_PHY_MASTER;
		sdsi_phy->usecase = MSM_DSI_PHY_SLAVE;

		dsi_host_reset_phy(mdsi->base);
		dsi_host_reset_phy(sdsi->base);

		ret = enable_phy(mdsi, &shared_timings[DSI_CLOCK_MASTER]);
		if (ret)
			return ret;
		ret = enable_phy(sdsi, &shared_timings[DSI_CLOCK_SLAVE]);
		if (ret) {
			return ret;
		}

		/* FIXME: should do that in DSI host init not phy init... */
		ret = dsi_enable_clocks(mdsi, &shared_timings[DSI_CLOCK_MASTER]);
		if (ret)
			return ret;
		ret = dsi_enable_clocks(sdsi, &shared_timings[DSI_CLOCK_SLAVE]);
		if (ret)
			return ret;
	} else {
		dsi_host_reset_phy(dsi->base);
		ret = enable_phy(dsi, &shared_timings[dsi->id]);
		if (ret)
			return ret;

		ret = dsi_enable_clocks(dsi, &shared_timings[dsi->id]);
		if (ret)
			return ret;
	}

	return 0;
}

static const struct mipi_dsi_phy_ops mdss_dsi_phy_ops = {
	.init = dsi_phy_init,
};

// Video bridge device (parent of DSI host)
static int qcom_dsi_attach(struct udevice *dev)
{
	struct qcom_dsi_priv *priv = dev_get_priv(dev);
	struct mipi_dsi_device *device = &priv->device;
	struct mipi_dsi_panel_plat *mplat;
	struct udevice *panel = NULL;
	ofnode pn;
	int ret;

	dev_info(dev, "%s\n", __func__);

	// ret = uclass_get_device(UCLASS_PANEL, priv->id, &priv->panel);
	// // ret = uclass_get_device_by_endpoint(UCLASS_PANEL, dev, 1, -1, &priv->panel);
	// if (ret) {
	// 	dev_err(dev, "panel@%d device error %d\n", priv->id, ret);
	// 	return ret;
	// }

	ret = uclass_find_device_by_endpoint(UCLASS_PANEL, dev, 1, -1, &panel);
	if (ret) {
		dev_err(dev, "Couldn't find panel! %d\n", ret);
		return ret;
	}

	/* If the panel is our direct child then we are allll good */
	if (ofnode_equal(dev_ofnode(panel), ofnode_find_subnode(dev_ofnode(dev), "panel"))) {
		priv->panel = panel;
	} else {
		/*
		* If not but we have a valid panel device then we're doing dual-dsi... bind a second panel device since
		* U-Boot can't handle a single panel with two DSI interfaces :/
		*/
		dev_info(dev, "Binding dummy panel for dual-DSI\n");
		ret = device_bind_with_driver_data(dev, panel->driver, "panel@1", 1, dev_ofnode(panel), &priv->panel);
		if (ret) {
			dev_err(dev, "Failed to bind dummy panel for dual-DSI! %d\n", ret);
			return ret;
		}
	}

	pn = dev_ofnode(priv->panel);
	dev_info(dev, "Got panel %s: %pOF\n", priv->panel->name, &pn);

	/* We need to actually probe the panel :) */
	ret = device_probe(priv->panel);
	if (ret)
		return ret;

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

static int qcom_dsi_get_dsc_config(struct udevice *dev, struct drm_dsc_config *dsc)
{
	struct qcom_dsi_priv *priv = dev_get_priv(dev);

	if (!priv->device.dsc)
		return -ENOENT;

	memcpy(dsc, priv->device.dsc, sizeof(struct drm_dsc_config));

	return 0;
}

static int qcom_dsi_set_backlight(struct udevice *dev, int percent)
{
	struct qcom_dsi_priv *priv = dev_get_priv(dev);
	int ret;

	dev_info(dev, "%s: %d\n", __func__, percent);

	if (percent) {
		ret = panel_enable_backlight(priv->panel);
		if (ret)
			return ret;
	}

	return panel_set_backlight(priv->panel, percent);
}

static int qcom_dsi_bind(struct udevice *dev)
{
	int ret, id;
	char *name;
	struct udevice *host;
#define NAMELEN (strlen(NAMEFMT) + 1)

#define NAMEFMT "DSI%d-bridge"
	id = dev_seq(dev);
	name = malloc(NAMELEN);
	if (!name)
		return -ENOMEM;
	snprintf(name, NAMELEN, NAMEFMT, id);
	device_set_name(dev, name);
	free(name);

#undef NAMEFMT
#define NAMEFMT "DSI%d-host"
	name = malloc(NAMELEN);
	if (!name)
		return -ENOMEM;
	snprintf(name, NAMELEN, NAMEFMT, id);

	ret = device_bind_driver_to_node(dev, "qcom_mipi_dsi", strdup(name),
					 dev_ofnode(dev), &host);
	free(name);
	if (ret)
		return ret;

	device_set_name_alloced(host);

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
	priv->id = dev_seq(dev);

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

	ret = clk_get_by_name(dev, "byte", &priv->byte_clk);
	if (ret) {
		dev_err(dev, "byte clock get error %d\n", ret);
		goto err_reg;
	}
	ret = clk_get_by_name(dev, "byte_intf", &priv->byte_intf_clk);
	if (ret < 0 && ret != -ENOENT) {
		dev_err(dev, "byte_intf clock get error %d\n", ret);
		goto err_reg;
	}
	ret = clk_get_by_name(dev, "pixel", &priv->pixel_clk);
	if (ret) {
		dev_err(dev, "pixel clock get error %d\n", ret);
		goto err_reg;
	}
	ret = clk_get_by_name(dev, "core", &priv->esc_clk);
	if (ret) {
		dev_err(dev, "core clock get error %d\n", ret);
		goto err_reg;
	}
	ret = clk_get_by_name(dev, "iface", &priv->iface_clk);
	if (ret) {
		dev_err(dev, "iface clock get error %d\n", ret);
		goto err_reg;
	}
	ret = clk_get_by_name(dev, "bus", &priv->bus_clk);
	if (ret) {
		dev_err(dev, "bus clock get error %d\n", ret);
		goto err_reg;
	}

	clk_set_rate(&priv->iface_clk, 19200000);
	clk_set_rate(&priv->bus_clk, ULONG_MAX);

	clk_enable(&priv->iface_clk);
	clk_enable(&priv->bus_clk);

	debug("DSI%d bridge probe done!\n", DSI_ID(priv));

	return 0;
err_reg:
	regulator_set_enable(priv->vdd_reg, false);

	return ret;
}

static int qcom_dsi_remove(struct udevice *dev)
{
	struct qcom_dsi_priv *priv = dev_get_priv(dev);
	struct qcom_mipi_dsi *sdsi_host = host_to_dsi(GET_DSI(DSI_1) ?: NULL);
	struct qcom_dsi_priv *sdsi;
	u32 val;

	if (DSI_ID(priv) == DSI_0) {
		/* Remove secondary DSI first if it exists */
		if (sdsi_host) {
			sdsi = (struct qcom_dsi_priv *)sdsi_host->device;
			device_remove(sdsi->device.dev, DM_REMOVE_NORMAL);
		}
	}

	printf("%s\n", __func__);

	val = dsi_read(priv->base + REG_DSI_CTRL);
	val &= ~DSI_CTRL_ENABLE;
	dsi_write(priv->base + REG_DSI_CTRL, val);

	printf("%s: disabled DSI%d ctrl\n", __func__, DSI_ID(priv));

	// generic_phy_power_off(&priv->phy);

	clk_disable(&priv->byte_clk);
	clk_disable(&priv->byte_intf_clk);
	clk_disable(&priv->pixel_clk);

	// clk_disable_bulk(&priv->bulk);

	regulator_set_enable(priv->vdd_reg, false);

	qcom_dsi_globals.dsi[DSI_ID(priv)] = NULL;

	if (DSI_ID(priv) == DSI_1 && IS_BONDED_DSI())
		return 0;

	msm_dsi_phy_disable(dev_get_priv(priv->phy.dev));
	if (IS_BONDED_DSI() && sdsi)
		msm_dsi_phy_disable(dev_get_priv(sdsi->phy.dev));

	return 0;
}

struct video_bridge_ops qcom_dsi_ops = {
	.attach = qcom_dsi_attach,
	.get_display_timing = qcom_dsi_get_display_timing,
	.get_dsc_config = qcom_dsi_get_dsc_config,
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
	.flags = DM_FLAG_LEAVE_PD_ON | DM_FLAG_OS_PREPARE,
};
