// SPDX-License-Identifier: BSD-3-Clause
/*
 * Qualcomm SPMI bus driver
 *
 * (C) Copyright 2015 Mateusz Kulikowski <mateusz.kulikowski@gmail.com>
 *
 * Loosely based on Little Kernel driver
 */

#include <common.h>
#include <dm.h>
#include <errno.h>
#include <fdtdec.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <dm/device_compat.h>
#include <spmi/spmi.h>

DECLARE_GLOBAL_DATA_PTR;

#define dev_dbgx(dev, fmt, ...) \
	printf(fmt, ##__VA_ARGS__)

/* PMIC Arbiter configuration registers */
#define PMIC_ARB_VERSION 0x0000
#define PMIC_ARB_VERSION_V2_MIN 0x20010000
#define PMIC_ARB_VERSION_V3_MIN 0x30000000
#define PMIC_ARB_VERSION_V5_MIN 0x50000000
#define PMIC_ARB_VERSION_V7_MIN	0x70000000

#define PMIC_ARB_FEATURES		0x0004
#define PMIC_ARB_FEATURES_PERIPH_MASK	GENMASK(10, 0)

#define APID_MAP_OFFSET_V1_V2_V3 (0x800)
#define APID_MAP_OFFSET_V5 (0x900)
#define APID_MAP_OFFSET_V7 (0x2000)
#define ARB_CHANNEL_OFFSET(n) (0x4 * (n))
#define SPMI_CH_OFFSET(chnl) ((chnl) * 0x8000)
#define SPMI_V5_OBS_CH_OFFSET(chnl) ((chnl) * 0x80)
#define SPMI_V7_OBS_CH_OFFSET(chnl) ((chnl) * 0x20)
#define SPMI_V5_RW_CH_OFFSET(chnl) ((chnl) * 0x10000)
#define SPMI_V7_RW_CH_OFFSET(chnl) ((chnl) * 0x1000)

#define SPMI_OWNERSHIP_PERIPH2OWNER(x)	((x) & 0x7)
#define PMIC_ARB_CHAN_IS_IRQ_OWNER(reg)	((reg) & BIT(24))

#define SPMI_REG_CMD0 0x0
#define SPMI_REG_CONFIG 0x4
#define SPMI_REG_STATUS 0x8
#define SPMI_REG_WDATA 0x10
#define SPMI_REG_RDATA 0x18

#define SPMI_CMD_OPCODE_SHIFT 27
#define SPMI_CMD_SLAVE_ID_SHIFT 20
#define SPMI_CMD_ADDR_SHIFT 12
#define SPMI_CMD_ADDR_OFFSET_SHIFT 4
#define SPMI_CMD_BYTE_CNT_SHIFT 0

#define SPMI_CMD_EXT_REG_WRITE_LONG 0x00
#define SPMI_CMD_EXT_REG_READ_LONG 0x01

#define SPMI_STATUS_DONE 0x1

#define SPMI_MAX_CHANNELS 128
#define SPMI_MAX_CHANNELS_V5	512
#define SPMI_MAX_CHANNELS_V7	1024
#define SPMI_MAX_SLAVES 16
#define SPMI_MAX_PERIPH 256

#define SPMI_CHANNEL_READ_ONLY	BIT(31)
#define SPMI_CHANNEL_VALID	BIT(30)
#define SPMI_CHANNEL_IRQ_EE	BIT(29)
#define SPMI_CHANNEL_MASK	0xffff

enum arb_ver {
	V1 = 1,
	V2,
	V3,
	V5 = 5,
	V7 = 7
};

/*
 * PMIC arbiter version 5 uses different register offsets for read/write vs
 * observer channels.
 */
enum pmic_arb_channel {
	PMIC_ARB_CHANNEL_RW,
	PMIC_ARB_CHANNEL_OBS,
};

struct msm_spmi_priv {
	phys_addr_t arb_chnl;  /* ARB channel mapping base */
	phys_addr_t spmi_chnls; /* SPMI channels */
	phys_addr_t spmi_obs;  /* SPMI observer */
	phys_addr_t spmi_cnfg;  /* SPMI config */
	u32 owner;	/* Current owner */
	unsigned int max_channels; /* Max channels */
	/* SPMI channel map */
	uint32_t channel_map[SPMI_MAX_SLAVES][SPMI_MAX_PERIPH];
	/* SPMI bus arbiter version */
	u32 arb_ver;
};

static u32 pmic_arb_fmt_cmd_v1(u8 opc, u8 sid, u8 pid, u8 off)
{
	return (opc << 27) | (sid << 20) | (pid << 12) | (off << 4) | 1;
}

static u32 pmic_arb_fmt_cmd_v2(u8 opc, u8 off)
{
	return (opc << 27) | (off << 4) | 1;
}

static int msm_spmi_write(struct udevice *dev, int usid, int pid, int off,
			  uint8_t val)
{
	struct msm_spmi_priv *priv = dev_get_priv(dev);
	unsigned channel;
	unsigned int ch_offset;
	uint32_t reg = 0;

	if (usid >= SPMI_MAX_SLAVES)
		return -EIO;
	if (pid >= SPMI_MAX_PERIPH)
		return -EIO;
	if (!(priv->channel_map[usid][pid] & SPMI_CHANNEL_VALID))
		return -EINVAL;
	if (priv->channel_map[usid][pid] & SPMI_CHANNEL_READ_ONLY)
		return -EPERM;

	channel = priv->channel_map[usid][pid] & SPMI_CHANNEL_MASK;

	dev_dbg(dev, "[%d:%d] %s: channel %d\n", usid, pid, __func__, channel);

	switch (priv->arb_ver) {
	case V1:
		ch_offset = SPMI_CH_OFFSET(channel);

		reg = pmic_arb_fmt_cmd_v1(SPMI_CMD_EXT_REG_WRITE_LONG,
					  usid, pid, off);
		break;

	case V2:
		ch_offset = SPMI_CH_OFFSET(channel);

		reg = pmic_arb_fmt_cmd_v2(SPMI_CMD_EXT_REG_WRITE_LONG, off);
		break;

	case V5:
		ch_offset = SPMI_V5_RW_CH_OFFSET(channel);

		reg = pmic_arb_fmt_cmd_v2(SPMI_CMD_EXT_REG_WRITE_LONG, off);
		break;

	case V7:
		ch_offset = SPMI_V7_RW_CH_OFFSET(channel);

		reg = pmic_arb_fmt_cmd_v2(SPMI_CMD_EXT_REG_WRITE_LONG, off);
		break;
	}

	/* Disable IRQ mode for the current channel*/
	writel(0x0, priv->spmi_chnls + ch_offset + SPMI_REG_CONFIG);

	/* Write single byte */
	writel(val, priv->spmi_chnls + ch_offset + SPMI_REG_WDATA);

	/* Send write command */
	writel(reg, priv->spmi_chnls + ch_offset + SPMI_REG_CMD0);

	/* Wait till CMD DONE status */
	reg = 0;
	while (!reg) {
		reg = readl(priv->spmi_chnls + ch_offset +
			    SPMI_REG_STATUS);
	}

	if (reg ^ SPMI_STATUS_DONE) {
		printf("SPMI write failure.\n");
		return -EIO;
	}

	return 0;
}

static int msm_spmi_read(struct udevice *dev, int usid, int pid, int off)
{
	struct msm_spmi_priv *priv = dev_get_priv(dev);
	unsigned channel;
	unsigned int ch_offset;
	uint32_t reg = 0;

	if (usid >= SPMI_MAX_SLAVES)
		return -EIO;
	if (pid >= SPMI_MAX_PERIPH)
		return -EIO;
	if (!(priv->channel_map[usid][pid] & SPMI_CHANNEL_VALID))
		return -EINVAL;

	channel = priv->channel_map[usid][pid] & SPMI_CHANNEL_MASK;

	dev_dbg(dev, "[%d:%d] %s: channel %d\n", usid, pid, __func__, channel);

	switch (priv->arb_ver) {
	case V1:
		ch_offset = SPMI_CH_OFFSET(channel);

		/* Prepare read command */
		reg = pmic_arb_fmt_cmd_v1(SPMI_CMD_EXT_REG_READ_LONG,
					  usid, pid, off);
		break;

	case V2:
		ch_offset = SPMI_CH_OFFSET(channel);

		/* Prepare read command */
		reg = pmic_arb_fmt_cmd_v2(SPMI_CMD_EXT_REG_READ_LONG, off);
		break;

	case V5:
		ch_offset = SPMI_V5_OBS_CH_OFFSET(channel);

		/* Prepare read command */
		reg = pmic_arb_fmt_cmd_v2(SPMI_CMD_EXT_REG_READ_LONG, off);
		break;

	case V7:
		ch_offset = SPMI_V7_OBS_CH_OFFSET(channel);

		/* Prepare read command */
		reg = pmic_arb_fmt_cmd_v2(SPMI_CMD_EXT_REG_READ_LONG, off);
		break;
	}

	/* Disable IRQ mode for the current channel*/
	writel(0x0, priv->spmi_obs + ch_offset + SPMI_REG_CONFIG);

	/* Request read */
	writel(reg, priv->spmi_obs + ch_offset + SPMI_REG_CMD0);

	/* Wait till CMD DONE status */
	reg = 0;
	while (!reg) {
		reg = readl(priv->spmi_obs + ch_offset + SPMI_REG_STATUS);
	}

	if (reg ^ SPMI_STATUS_DONE) {
		printf("SPMI read failure.\n");
		return -EIO;
	}

	/* Read the data */
	return readl(priv->spmi_obs + ch_offset +
				SPMI_REG_RDATA) & 0xFF;
}

static struct dm_spmi_ops msm_spmi_ops = {
	.read = msm_spmi_read,
	.write = msm_spmi_write,
};

static void spmi_dump_channel_map(struct msm_spmi_priv *priv)
{
	int i, j;
	uint32_t (*spmi_channel_map)[SPMI_MAX_PERIPH];
	spmi_channel_map = priv->channel_map;

	printk("SPMI channel map\n");
	for (i = 0; i < SPMI_MAX_SLAVES; i++) {
		for (j = 0; j < SPMI_MAX_PERIPH; ++j) {
			u32 map = spmi_channel_map[i][j];

			if (!(map & SPMI_CHANNEL_VALID))
				continue;
			
			bool writable = !(map & SPMI_CHANNEL_READ_ONLY);
		
			printk("%s ch: 0x%04x sid 0x%02x pid 0x%02x irq:%d\n",
				   writable ? "[RW]" : "[R] ",
			       map & SPMI_CHANNEL_MASK, i, j,
				   !!(map & SPMI_CHANNEL_IRQ_EE));
		}
	}
}

/*
 * In order to allow multiple EEs to write to a single PPID in arbiter
 * version 5 and 7, there is more than one APID mapped to each PPID.
 * The owner field for each of these mappings specifies the EE which is
 * allowed to write to the APID.
 */
static void msm_spmi_channel_map_v5(struct msm_spmi_priv *priv, unsigned int ch,
				    uint32_t map, uint8_t slave_id, uint8_t pid)
{
	bool valid = priv->channel_map[slave_id][pid] & SPMI_CHANNEL_VALID;
	uint32_t read_only = priv->channel_map[slave_id][pid] & SPMI_CHANNEL_READ_ONLY;
	uint32_t cnfg = readl(priv->spmi_cnfg + ARB_CHANNEL_OFFSET(ch));
	uint8_t owner = SPMI_OWNERSHIP_PERIPH2OWNER(cnfg);
	uint32_t periph = readl(priv->arb_chnl + ARB_CHANNEL_OFFSET(ch));
	bool is_irq_ee = PMIC_ARB_CHAN_IS_IRQ_OWNER(periph);

	if (!valid) {
		/* First PPID mapping */
		priv->channel_map[slave_id][pid] = ch | SPMI_CHANNEL_VALID;
		if (owner != priv->owner)
			priv->channel_map[slave_id][pid] |= SPMI_CHANNEL_READ_ONLY;
		else if (is_irq_ee)
			priv->channel_map[slave_id][pid] |= SPMI_CHANNEL_IRQ_EE;			
	} else if ((owner == priv->owner) && read_only) {
		/* Read only and we found one we own, switch */
		priv->channel_map[slave_id][pid] = ch | SPMI_CHANNEL_VALID;
	} else if ((owner == priv->owner) && !read_only) {
		/* Writable and we found one we own, set IRQ bit */
		priv->channel_map[slave_id][pid] = ch | SPMI_CHANNEL_VALID;
		priv->channel_map[slave_id][pid] |= SPMI_CHANNEL_IRQ_EE;
	}
}

static int msm_spmi_probe(struct udevice *dev)
{
	struct msm_spmi_priv *priv = dev_get_priv(dev);
	phys_addr_t core_addr;
	u32 hw_ver;
	int i;

	core_addr = dev_read_addr_name(dev, "core");
	priv->spmi_chnls = dev_read_addr_name(dev, "chnls");
	priv->spmi_obs = dev_read_addr_name(dev, "obsrvr");
	dev_read_u32(dev, "qcom,ee", &priv->owner);

	hw_ver = readl(core_addr + PMIC_ARB_VERSION);

	if (hw_ver < PMIC_ARB_VERSION_V3_MIN) {
		priv->arb_ver = V2;
		priv->arb_chnl = core_addr + APID_MAP_OFFSET_V1_V2_V3;
		priv->max_channels = SPMI_MAX_CHANNELS;
	} else if (hw_ver < PMIC_ARB_VERSION_V5_MIN) {
		priv->arb_ver = V3;
		priv->arb_chnl = core_addr + APID_MAP_OFFSET_V1_V2_V3;
		priv->max_channels = SPMI_MAX_CHANNELS;
	} else if (hw_ver < PMIC_ARB_VERSION_V7_MIN) {
		priv->arb_ver = V5;
		priv->arb_chnl = core_addr + APID_MAP_OFFSET_V5;
		priv->max_channels = min_t(u32, readl(core_addr + PMIC_ARB_FEATURES) &
						PMIC_ARB_FEATURES_PERIPH_MASK,
					   SPMI_MAX_CHANNELS_V5);
		priv->spmi_cnfg = dev_read_addr_name(dev, "cnfg");
	} else {
		/* TOFIX: handle second bus */
		priv->arb_ver = V7;
		priv->arb_chnl = core_addr + APID_MAP_OFFSET_V7;
		priv->max_channels = min_t(u32, readl(core_addr + PMIC_ARB_FEATURES) &
						PMIC_ARB_FEATURES_PERIPH_MASK,
					   SPMI_MAX_CHANNELS_V7);
		priv->spmi_cnfg = dev_read_addr_name(dev, "cnfg");
	}

	dev_dbgx(dev, "PMIC Arb Version-%d (%#x)\n", hw_ver >> 28, hw_ver);

	if (priv->arb_chnl == FDT_ADDR_T_NONE ||
	    priv->spmi_chnls == FDT_ADDR_T_NONE ||
	    priv->spmi_obs == FDT_ADDR_T_NONE)
		return -EINVAL;

	dev_dbgx(dev, "priv->arb_chnl address (%#08llx)\n", priv->arb_chnl);
	dev_dbgx(dev, "priv->spmi_chnls address (%#08llx)\n", priv->spmi_chnls);
	dev_dbgx(dev, "priv->spmi_obs address (%#08llx)\n", priv->spmi_obs);
	/* Scan peripherals connected to each SPMI channel */
	for (i = 0; i < priv->max_channels; i++) {
		uint32_t periph = readl(priv->arb_chnl + ARB_CHANNEL_OFFSET(i));
		uint8_t slave_id = (periph & 0xf0000) >> 16;
		uint8_t pid = (periph & 0xff00) >> 8;

		switch (priv->arb_ver) {
		case V2:
		case V3:
			priv->channel_map[slave_id][pid] = i | SPMI_CHANNEL_VALID;
			break;

		case V5:
		case V7:
			msm_spmi_channel_map_v5(priv, i, periph, slave_id, pid);
			break;
		}
	}

	//spmi_dump_channel_map(priv);

	return 0;
}

static const struct udevice_id msm_spmi_ids[] = {
	{ .compatible = "qcom,spmi-pmic-arb" },
	{ }
};

U_BOOT_DRIVER(msm_spmi) = {
	.name = "msm_spmi",
	.id = UCLASS_SPMI,
	.of_match = msm_spmi_ids,
	.ops = &msm_spmi_ops,
	.probe = msm_spmi_probe,
	.priv_auto = sizeof(struct msm_spmi_priv),
};
