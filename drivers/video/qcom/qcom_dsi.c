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

#include "mdssreg.h"
#include "qcom_mipi_dsi.h"
#include "dsi.xml.h"
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

#define REG_DSI_VERSION						0x000001f0

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
	u32 hw_version;
	int lane_min_kbps;
	int lane_max_kbps;
	unsigned int lane_mbps; /* per lane */
	struct udevice *vdd_reg;
	struct udevice *dsi_host;
	struct clk_bulk bulk;
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
	phys_addr_t tx_buf;
	u8 *rx_buf;
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

//#define REG_DSI_VBIF_CTRL	(0x01CC - 4)
static int dsi_cmds2buf_tx(struct mipi_dsi_host *host,
			   const struct mipi_dsi_msg *msg)
{
	struct qcom_mipi_dsi *dsi = host_to_dsi(host);
	struct mipi_dsi_packet packet;
	int ret, len, count = 0;
	void __iomem *base = dsi->base;
	u8 *data;
	unsigned int i, fifo_writes;
	u32 val = 0;

	val = dsi_read(base + REG_DSI_STATUS0);
	printf("======%s tx %lu rx %lu with base %p status %x\n", __func__, msg->tx_len, msg->rx_len, dsi->base, val);
	if (val & DSI_STATUS0_VIDEO_MODE_ENGINE_BUSY) {
		while (count < 1000) {
			val = dsi_read(base + REG_DSI_STATUS0) ;

			if (!(val & DSI_STATUS0_VIDEO_MODE_ENGINE_BUSY)) 
				break;

			++count;
		}

		if (val & DSI_STATUS0_VIDEO_MODE_ENGINE_BUSY) {
			dev_err(host->dev, "failed to wait for video engine busy (%x)\n", val);
			return ret;
		}
	}
#if 0
	count = 0;
	if (val & DSI_STATUS0_CMD_MODE_DMA_BUSY) {
		while (count < 1000) {
			val = dsi_read(base + REG_DSI_STATUS0) ;

			if (!(val & DSI_STATUS0_CMD_MODE_DMA_BUSY)) 
				break;

			++count;
		}

		if (val & DSI_STATUS0_CMD_MODE_DMA_BUSY) {
			dev_err(host->dev, "failed to wait for CMD DMA engine busy (%x)\n", val);
		}
	}
#endif

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

#if 0
	//dsi_write(base + REG_DSI_CTRL, 0x177);
	//dsi_write(base + REG_DSI_INTR_CTRL, 0xAA20AA02);
	// Unmask
	dsi_write(base + REG_DSI_INTR_CTRL, DSI_IRQ_MASK_CMD_DMA_DONE);
	// Clear
	dsi_write(base + REG_DSI_INTR_CTRL, DSI_IRQ_MASK_CMD_DMA_DONE | DSI_IRQ_CMD_DMA_DONE);
	dsi_write(base + REG_DSI_DMA_BASE, dsi->tx_buf);
	dsi_write(base + REG_DSI_DMA_LEN, len);
	//dsi_write(base + REG_DSI_VBIF_CTRL, 0x7);
	dsi_write(base + REG_DSI_TRIG_DMA, 0x1);
	//dsi_write(base + REG_DSI_INTR_CTRL, 0xAA20AA03);
	printf("Started DMA, waiting for DONE\n");
	//printf("DSI reading irq off at 0x%u\n", base + REG_DSI_INTR_CTRL);

	val = dsi_read(base + REG_DSI_INTR_CTRL);
	count = 0;
	while (!(val & (DSI_IRQ_CMD_DMA_DONE))) {
		if (count > 100) {
			printf("DSI CMD: count :%d dma failed\n", count);
			printf("DSI CMD: irq value = %x\n", dsi_read(base + REG_DSI_INTR_CTRL));
			goto out_err;
		}
		udelay(1000);
		val = dsi_read(base + REG_DSI_INTR_CTRL);
		count++;
	}

	// Clear
	dsi_write(base + REG_DSI_INTR_CTRL, DSI_IRQ_MASK_CMD_DMA_DONE | DSI_IRQ_CMD_DMA_DONE);
	// Mask
	dsi_write(base + REG_DSI_INTR_CTRL, 0);

	return packet.size;

out_err:
	// Clear
	dsi_write(base + REG_DSI_INTR_CTRL, DSI_IRQ_MASK_CMD_DMA_DONE | DSI_IRQ_CMD_DMA_DONE);
	// Mask
	dsi_write(base + REG_DSI_INTR_CTRL, 0);

	return -EIO;
#elif 0
	// Unmask
	dsi_write(base + REG_DSI_INTR_CTRL, DSI_IRQ_MASK_CMD_DMA_DONE);
	// Clear
	dsi_write(base + REG_DSI_INTR_CTRL, DSI_IRQ_MASK_CMD_DMA_DONE | DSI_IRQ_CMD_DMA_DONE);

	dsi_write(base + REG_DSI_DMA_LEN, len);
	dsi_write(base + REG_DSI_TEST_PATTERN_GEN_CTRL, DSI_TEST_PATTERN_GEN_CTRL_CMD_DMA_TPG_EN |
							DSI_TEST_PATTERN_GEN_CTRL_TPG_DMA_FIFO_MODE |
							DSI_TEST_PATTERN_GEN_CTRL_CMD_DMA_PATTERN_SEL(3));

	data32 = (u32 *)data;
	fifo_writes = 0;
	for (i = 0; i < ((len+3)/4) ; ++i) {
		printf("CMD[%d]=%x\n", i, data32[i]);
		dsi_write(base + REG_DSI_TEST_PATTERN_GEN_CMD_DMA_INIT_VAL, data32[i]);
		fifo_writes++;
	}

	if ((fifo_writes % 2) == 0)
		dsi_write(base + REG_DSI_TEST_PATTERN_GEN_CMD_DMA_INIT_VAL, 0);

	dsi_read(base + REG_DSI_DMA_LEN);
	dsi_write(base + REG_DSI_TRIG_DMA, 0x1);

	printf("Started DMA, waiting for DONE\n");
	//printf("DSI reading irq off at 0x%u\n", base + REG_DSI_INTR_CTRL);

	val = dsi_read(base + REG_DSI_INTR_CTRL);
	count = 0;
	while (!(val & (DSI_IRQ_CMD_DMA_DONE))) {
		if (count > 100) {
			printf("DSI CMD: count :%d dma failed\n", count);
			printf("DSI CMD: irq value = %x\n", dsi_read(base + REG_DSI_INTR_CTRL));
			goto out_err;
		}
		udelay(1000);
		val = dsi_read(base + REG_DSI_INTR_CTRL);
		count++;
	}

	// Clear
	dsi_write(base + REG_DSI_INTR_CTRL, DSI_IRQ_MASK_CMD_DMA_DONE | DSI_IRQ_CMD_DMA_DONE);
	// Mask
	dsi_write(base + REG_DSI_INTR_CTRL, 0);

	return packet.size;

out_err:
	// Clear
	dsi_write(base + REG_DSI_INTR_CTRL, DSI_IRQ_MASK_CMD_DMA_DONE | DSI_IRQ_CMD_DMA_DONE);
	// Mask
	dsi_write(base + REG_DSI_INTR_CTRL, 0);

	return -EIO;
#else
	dsi_write(base + REG_DSI_TEST_PATTERN_GEN_CTRL, DSI_TEST_PATTERN_GEN_CTRL_CMD_DMA_TPG_EN |
							DSI_TEST_PATTERN_GEN_CTRL_TPG_DMA_FIFO_MODE |
							DSI_TEST_PATTERN_GEN_CTRL_CMD_DMA_PATTERN_SEL(CMD_DMA_CUSTOM_PATTERN_DMA_FIFO));

	for (i = 0; i < len ; i+=4) {
		//printf("CMD[%d]=%x\n", i, *(u32 *)(&data[i]));
		dsi_write(base + REG_DSI_TEST_PATTERN_GEN_CMD_DMA_INIT_VAL, *(u32 *)(&data[i]));
	}

	if ((len % 8) != 0)
		dsi_write(base + REG_DSI_TEST_PATTERN_GEN_CMD_DMA_INIT_VAL, 0);

	dsi_write(base + REG_DSI_DMA_LEN, len);

	// Unmask
	dsi_write(base + REG_DSI_INTR_CTRL, DSI_IRQ_MASK_CMD_DMA_DONE);
	// Clear
	dsi_write(base + REG_DSI_INTR_CTRL, DSI_IRQ_MASK_CMD_DMA_DONE | DSI_IRQ_CMD_DMA_DONE);

	dsi_write(base + REG_DSI_TRIG_DMA, 0x1);

	//printf("Started DMA for len %d, waiting for DONE\n", len);

	val = dsi_read(base + REG_DSI_INTR_CTRL) & DSI_IRQ_CMD_DMA_DONE;
	count = 0;
	ret = 0;
	while (!val) {
		if (count > 0xffff) {
			printf("DSI CMD: count :%d dma failed\n", count);
			//printf("DSI CMD: irq value = %x\n", dsi_read(base + REG_DSI_INTR_CTRL));
			ret = -EIO;
			break;
		}
		udelay(10);
		val = dsi_read(base + REG_DSI_INTR_CTRL) & DSI_IRQ_CMD_DMA_DONE;
		count++;
	}

	// Clear
	dsi_write(base + REG_DSI_INTR_CTRL, DSI_IRQ_MASK_CMD_DMA_DONE | DSI_IRQ_CMD_DMA_DONE);
	// Mask
	dsi_write(base + REG_DSI_INTR_CTRL, 0);

	dsi_write(base + REG_DSI_TPG_DMA_FIFO_RESET, 1);
	dsi_write(base + REG_DSI_TPG_DMA_FIFO_RESET, 0);

	dsi_write(base + REG_DSI_TEST_PATTERN_GEN_CTRL, 0);

	return ret < 0 ? ret : packet.size;
#endif
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
		printf("data = 0x%x and ntohl(data) = 0x%x\n", data, ntohl(data));
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

	//printf("Dsi RX on Host %d", dsi->id);
	printf("Dsi RX\n");
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

		printf("rlen=%d pkt_size=%d rx_byte=%d\n",
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
			printf("buf=%p dlen=%d diff=%d\n", buf, dlen, diff);
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
	u32 status;
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
	printf("DSI host controller init\n");

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
	//dsi_write(base + REG_DSI_ERR_INT_MASK0, 0x13FF3FE0);
	//dsi_write(base + REG_DSI_ERR_INT_MASK0, 0);
	dsi_write(base + REG_DSI_INTR_CTRL, 0);
	dsi_write(base + REG_DSI_CLK_CTRL, 0x23F);
	dsi_write(base + REG_DSI_LANE_SWAP_CTRL, 0x0);
	dsi_write(base + REG_DSI_LANE_CTRL, DSI_LANE_CTRL_HS_REQ_SEL_PHY);

	dsi_write(base + REG_DSI_CTRL, 0x173);

	dsi_write(base + REG_DSI_CPHY_MODE_CTRL, 0x1);
}

static void qcom_mipi_dsi_host_enable(void __iomem *base)
{
	printf("DSI host controller enable\n");

	/* NOP */
}

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
	dsi->base += 4;
	/* offset in xml is wrong, we should shift it by 4 */
	//dsi->base -= 4;

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

	dsi->tx_buf =  lmb_alloc_base(0x1000, 0x1000, 0xf0000000, LMB_NONE);
	dsi->rx_buf =  malloc(0x1000);

	dsi_write(dsi->base + REG_DSI_PHY_RESET, 0x1);
	mdelay(1);
	dsi_write(dsi->base + REG_DSI_PHY_RESET, 0x0);
	mdelay(1);

	ret = phy_ops->init(device);

	qcom_mipi_dsi_init_ctrl(dsi->base);

	return 0;
}

static int qcom_mipi_dsi_enable(struct udevice *dev)
{
	struct qcom_mipi_dsi *dsi = dev_get_priv(dev);

	dev_dbg(dev, "Enable DSI host\n");
	/* Switch to video mode for panel-bridge enable & panel enable */
	//qcom_mipi_dsi_set_mode(dsi, MIPI_DSI_MODE_VIDEO);

	qcom_mipi_dsi_host_enable(dsi->base);

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
	//u32 val;
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

	ret = clk_enable_bulk(&dsi->bulk);
	if (ret) {
		dev_err(dev, "peripheral clock enable error %d\n", ret);
		return ret;
	}

	

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

static int qcom_dsi_get_display_timing(struct udevice *dev, struct display_timing *timing)
{
	struct qcom_dsi_priv *priv = dev_get_priv(dev);

	return panel_get_display_timing(priv->panel, timing);
}

static int qcom_dsi_set_backlight(struct udevice *dev, int percent)
{
	struct qcom_dsi_priv *priv = dev_get_priv(dev);

#if 0
	dev_err(dev, "panel %s backlight enabling skipped to avoid panic for temp\n", priv->panel->name);
//	return 0;

	ret = panel_set_backlight(priv->panel, percent);
	if (ret) {
		dev_err(dev, "panel %s enable backlight error %d\n",
			priv->panel->name, ret);
		return ret;
	}

	/// FIXME: CASEY: dsi_host_enable() expects struct udevice not mipi_dsi_host?
	/// Jun: by definition, it expects udevice. This should be sort out,
	///      otherwise, qcom_mipi_dsi_enable will panic later.
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
#else
	return dsi_host_enable(priv->dsi_host);
#endif

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
	int ret;

	device->dev = dev;

printf("=============%s enter\n", __func__);
	priv->hw_base = dev_read_addr_ptr(dev);
	if (!priv->hw_base) {
		dev_err(dev, "dsi dt register address error\n");
		return -EINVAL;
	}
	priv->base = priv->hw_base + 4;

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
	priv->hw_version = dsi_read(priv->hw_base + REG_DSI_6G_HW_VERSION);
	printf("Got hw_version: 0x%x\n", priv->hw_version);
	if (priv->hw_version != 0x20080000) {
		dev_err(dev, "DSI version 0x%x not supported\n", priv->hw_version);
		dev_dbg(dev, "remove and unbind all DSI child\n");
		device_chld_remove(dev, NULL, DM_REMOVE_NORMAL);
		device_chld_unbind(dev, NULL);
		return -ENODEV;
	}

printf("=============%s ret 0\n", __func__);
	return 0;
err_reg:
	regulator_set_enable(priv->vdd_reg, false);

printf("=============%s ret %d\n", __func__, ret);
	return ret;
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
	.name				= "qcom-dsi",
	.id				= UCLASS_VIDEO_BRIDGE,
	.of_match			= qcom_dsi_ids,
	.bind				= qcom_dsi_bind,
	.probe				= qcom_dsi_probe,
	.ops				= &qcom_dsi_ops,
	.priv_auto		= sizeof(struct qcom_dsi_priv),
};
