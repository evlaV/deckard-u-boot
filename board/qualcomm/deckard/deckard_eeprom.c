// SPDX-License-Identifier: GPL-2.0+
/*
 * SM8650 Deckard EEPROM
 *
 * The core and strap EEPROMs are read exactly once, here, during board init.
 * Everything anyone needs later is cached in this file, and the immutable
 * identity fields are handed to the kernel as device tree properties under
 * /board-info so that userspace can read them out of
 * /sys/firmware/devicetree/base/board-info/ (equivalently
 * /proc/device-tree/board-info/) without ever touching i2c.
 */

#include "deckard_eeprom.h"
#include <dm.h>
#include <fdt_support.h>
#include <asm/global_data.h>
#include <init.h>
#include <i2c.h>
#include <i2c_eeprom.h>
#include <env.h>
#include <net.h>
#include <string.h>
#include <linux/errno.h>
#include <linux/string.h>
#include "deckard_log.h"

DECLARE_GLOBAL_DATA_PTR;

/* Device tree node the identity fields are published under. */
#define DECKARD_BOARD_INFO_NODE      "board-info"

/*
 * Widest field we export is EEPROM_CONFIG_STRING_LEN bytes. Allow one more, so
 * that even a field carrying no NUL at all can still be forwarded whole and
 * terminated.
 */
#define DECKARD_BOARD_INFO_VALUE_MAX (EEPROM_CONFIG_STRING_LEN + 1)

static eeprom_strap_v2_t s_strap;
static bool s_strap_valid;

static uint8_t s_wifi_mac_addr[EEPROM_MAC_ADDR_LEN];
static bool s_wifi_mac_addr_valid;

/*
 * Cached identity fields, in the order they get emitted. Property names
 * deliberately match the field names the userspace deckard-eeprom tool uses,
 * so `cat /proc/device-tree/board-info/unit_sn` and
 * `eeprom_console get unit_sn` name the same thing.
 */
enum board_info_index {
	BI_BOARD_REVISION,
	BI_BOARD_MINOR,
	BI_IMU_REV,
	BI_MPCBA_SN,
	BI_UNIT_SN,
	BI_UNIT_CONFIG,
	BI_CORE_CM_SN,
	BI_CORE_ASSEMBLY_CM_SN,
	BI_HEAT_PIPE_SN,
	BI_FAN_SN,
	BI_MIDWALL_HEAT_PIPE_SN,
	BI_STRAP_SN,
	BI_STRAP_REVISION,
	BI_STRAP_MINOR,
	BI_STRAP_CONFIG,
	BI_AUDIO_MODULE_VENDOR,
	BI_LEFT_AUDIO_SN,
	BI_RIGHT_AUDIO_SN,
	BI_COUNT
};

struct board_info_entry {
	const char *name;
	char value[DECKARD_BOARD_INFO_VALUE_MAX];
	bool present;
};

static struct board_info_entry s_board_info[BI_COUNT] = {
	[BI_BOARD_REVISION]       = { .name = "board_revision" },
	[BI_BOARD_MINOR]          = { .name = "board_minor" },
	[BI_IMU_REV]              = { .name = "imu_rev" },
	[BI_MPCBA_SN]             = { .name = "mpcba_sn" },
	[BI_UNIT_SN]              = { .name = "unit_sn" },
	[BI_UNIT_CONFIG]          = { .name = "unit_config" },
	[BI_CORE_CM_SN]           = { .name = "core_cm_sn" },
	[BI_CORE_ASSEMBLY_CM_SN]  = { .name = "core_assembly_cm_sn" },
	[BI_HEAT_PIPE_SN]         = { .name = "heat_pipe_sn" },
	[BI_FAN_SN]               = { .name = "fan_sn" },
	[BI_MIDWALL_HEAT_PIPE_SN] = { .name = "midwall_heat_pipe_sn" },
	[BI_STRAP_SN]             = { .name = "strap_sn" },
	[BI_STRAP_REVISION]       = { .name = "strap_revision" },
	[BI_STRAP_MINOR]          = { .name = "strap_minor" },
	[BI_STRAP_CONFIG]         = { .name = "strap_config" },
	[BI_AUDIO_MODULE_VENDOR]  = { .name = "audio_module_vendor" },
	[BI_LEFT_AUDIO_SN]        = { .name = "left_audio_sn" },
	[BI_RIGHT_AUDIO_SN]       = { .name = "right_audio_sn" },
};

/* Record lengths per header version, terminated by a zero length. */
static const eeprom_version_len_t s_bootloader_versions[] = {
	{ 3, sizeof(eeprom_bootloader_t) }, /* EV2, EV3, DV1, DV2, PV */
	{ 0, 0                           },
};

static const eeprom_version_len_t s_core_versions[] = {
	{ 3, EEPROM_CORE_V3_LENGTH }, /* EV2, EV3 */
	{ 4, EEPROM_CORE_V4_LENGTH }, /* DV1 */
	{ 5, EEPROM_CORE_V5_LENGTH }, /* DV2, PV */
	{ 0, 0                     },
};

static const eeprom_version_len_t s_strap_v2_versions[] = {
	{ 1, sizeof(eeprom_strap_v2_t) }, /* EV3.3, DV1, DV2, Boulder EV2 */
	{ 0, 0                         },
};

// These are taken directly from Deckard EEPROM user space tool
static eeprom_checksum_t _eeprom_calculate_checksum(const uint8_t *data, uint32_t len)
{
	//https://en.wikipedia.org/wiki/Fletcher's_checksum

	eeprom_checksum_t check;
	check.value = 0;

	if (NULL == data)
	{
		return check;
	}

	uint16_t sum1 = 0;
	uint16_t sum2 = 0;

	for (uint32_t index = 0; index < len; ++index)
	{
		sum1 = (sum1 + data[index]) % 255;
		sum2 = (sum2 + sum1) % 255;
	}

	check.value = (sum2 << 8) | sum1;
	return check;
}

/*
 * Resolve an EEPROM /aliases entry to a bound i2c_eeprom device.
 */
static int _eeprom_get_dev(const char *alias, struct udevice **devp)
{
	const char *path;
	ofnode node;
	int aliases;
	int ret;

	if (!gd->fdt_blob) {
		deckard_log("%s: don't have a valid gd->fdt_blob!\n", __func__);
		return -ENODEV;
	}

	aliases = fdt_path_offset(gd->fdt_blob, "/aliases");
	if (aliases < 0) {
		deckard_log("%s: no aliases found\n", __func__);
		return -ENODEV;
	}

	path = fdt_getprop(gd->fdt_blob, aliases, alias, NULL);
	if (!path) {
		deckard_log("%s: no alias for %s\n", __func__, alias);
		return -ENODEV;
	}

	node = ofnode_path(path);
	if (!ofnode_valid(node)) {
		deckard_log("%s: invalid hardware path to %s\n", __func__, alias);
		return -ENODEV;
	}

	ret = uclass_get_device_by_ofnode(UCLASS_I2C_EEPROM, node, devp);
	if (ret) {
		deckard_log("%s: cannot find %s by node: %d\n", __func__, alias, ret);
		return ret;
	}

	return 0;
}

/*
 * Read one versioned EEPROM record into @buf.
 *
 * A record's length depends on the version in its header, and the two byte
 * Fletcher checksum always sits at the end of that length - so the version has
 * to be resolved from the header before the body can be read and verified.
 * @buf is zeroed first, so a short (older) record leaves the trailing members
 * of the widest struct at zero rather than at whatever followed it on the part.
 */
static int _eeprom_load(struct udevice *dev, uint32_t offset, uint32_t magic,
			const eeprom_version_len_t *versions, void *buf,
			uint32_t buf_len, const char *what)
{
	eeprom_checksum_t stored;
	eeprom_checksum_t calc;
	eeprom_header_t hdr;
	uint32_t checked_len;
	uint32_t length = 0;
	int ret;

	memset(buf, 0, buf_len);

	ret = i2c_eeprom_read(dev, offset, (uint8_t *)&hdr, sizeof(hdr));
	if (ret) {
		deckard_log("EEPROM %s: could not read header: %d\n", what, ret);
		return ret;
	}

	if (hdr.magic_number != magic) {
		deckard_log("EEPROM %s: magic number %08x, expected %08x\n", what,
			    hdr.magic_number, magic);
		return -EINVAL;
	}

	for (unsigned int i = 0; versions[i].length; i++) {
		if (versions[i].version == hdr.version) {
			length = versions[i].length;
			break;
		}
	}

	if (!length) {
		deckard_log("EEPROM %s: version %u not supported by u-boot\n", what, hdr.version);
		return -ENOTSUPP;
	}

	if (length > buf_len) {
		deckard_log("EEPROM %s: version %u wants %u bytes, buffer is %u\n", what,
			    hdr.version, length, buf_len);
		return -ENOSPC;
	}

	ret = i2c_eeprom_read(dev, offset, buf, length);
	if (ret) {
		deckard_log("EEPROM %s: could not read %u bytes: %d\n", what, length, ret);
		return ret;
	}

	checked_len = length - sizeof(eeprom_checksum_t);
	memcpy(&stored, (const uint8_t *)buf + checked_len, sizeof(stored));
	calc = _eeprom_calculate_checksum(buf, checked_len);

	if (calc.value != stored.value) {
		deckard_log("EEPROM %s: checksum %04x, expected %04x\n", what,
			    stored.value, calc.value);
		return -EBADMSG;
	}

	deckard_log("EEPROM %s: v%u verified (%u bytes)\n", what, hdr.version, length);
	return 0;
}

/*
 * Cache one identity field, but only if the EEPROM marked it valid. Anything
 * never programmed simply stays absent and no property is emitted for it.
 *
 * The value is forwarded verbatim - no trimming, no filtering. The single
 * bound applied is the field width itself: on-EEPROM strings are supposed to
 * be NUL terminated, but a corrupted or partially programmed part need not be,
 * and strlen() would then walk on into the neighbouring struct members.
 */
static void _board_info_set(enum board_info_index idx, const char *field,
			    uint32_t field_len, uint8_t valid)
{
	struct board_info_entry *entry = &s_board_info[idx];
	uint32_t len;

	if (valid != EEPROM_VALID)
		return;

	len = strnlen(field, field_len);

	/* Cannot happen with the fields below, but do not trust a future one. */
	if (len >= sizeof(entry->value)) {
		deckard_log("EEPROM %s: %u bytes will not fit a %u byte buffer, skipping\n",
			    entry->name, len, (uint32_t)sizeof(entry->value));
		return;
	}

	memcpy(entry->value, field, len);
	entry->value[len] = '\0';
	entry->present = true;
}

/* Cache a field along with the _valid byte that guards it. */
#define BOARD_INFO_SET(idx, rec, field) \
	_board_info_set((idx), (const char *)&(rec)->field, sizeof((rec)->field), (rec)->field##_valid)

static void _eeprom_load_core(void)
{
	eeprom_bootloader_t bootloader;
	struct udevice *dev;
	eeprom_core_t core;
	int ret;

	ret = _eeprom_get_dev("eeprom-core", &dev);
	if (ret)
		return;

	if (!_eeprom_load(dev, EEPROM_BOOTLOADER_ADDRESS_START, EEPROM_BOOTLOADER_MAGIC_NUMBER,
			  s_bootloader_versions, &bootloader, sizeof(bootloader), "bootloader")) {
		BOARD_INFO_SET(BI_BOARD_REVISION, &bootloader, board_revision);
		BOARD_INFO_SET(BI_BOARD_MINOR, &bootloader, board_minor);
		BOARD_INFO_SET(BI_IMU_REV, &bootloader, imu_rev);

		if (s_board_info[BI_BOARD_REVISION].present) {
			deckard_log("EEPROM Board Revision: %s\n",
				    s_board_info[BI_BOARD_REVISION].value);
			env_set("EEPROM_BOARD", s_board_info[BI_BOARD_REVISION].value);
		} else {
			deckard_log("EEPROM Board Revision not valid\n");
		}
	}

	ret = _eeprom_load(dev, EEPROM_CORE_ADDRESS_START, EEPROM_CORE_MAGIC_NUMBER,
			   s_core_versions, &core, sizeof(core), "core");

	if (core.mac_addr_wifi_valid != EEPROM_VALID) {
		deckard_log("EEPROM mac_addr_wifi not valid\n");
	} else if (!is_valid_ethaddr(core.mac_addr_wifi)) {
		deckard_log("EEPROM mac_addr_wifi is not a valid ethernet address\n");
	} else {
		memcpy(s_wifi_mac_addr, core.mac_addr_wifi, EEPROM_MAC_ADDR_LEN);
		s_wifi_mac_addr_valid = true;
		deckard_log("EEPROM wifi mac address: %pM\n", s_wifi_mac_addr);
	}

	if (ret) {
		deckard_log("EEPROM core: failed verification, not publishing core identity\n");
		return;
	}

	BOARD_INFO_SET(BI_MPCBA_SN, &core, mpcba_sn);
	BOARD_INFO_SET(BI_UNIT_SN, &core, unit_sn);
	BOARD_INFO_SET(BI_UNIT_CONFIG, &core, unit_config);
	BOARD_INFO_SET(BI_CORE_CM_SN, &core, core_cm_sn);
	BOARD_INFO_SET(BI_CORE_ASSEMBLY_CM_SN, &core, core_assembly_cm_sn);
	BOARD_INFO_SET(BI_HEAT_PIPE_SN, &core, heat_pipe_sn);
	BOARD_INFO_SET(BI_FAN_SN, &core, fan_sn);

	/* midwall_heat_pipe_sn only exists from core version 4 (DV1) onwards */
	if (core.hdr.version >= 4)
		BOARD_INFO_SET(BI_MIDWALL_HEAT_PIPE_SN, &core, midwall_heat_pipe_sn);
}

static void _eeprom_extract_strap_v2(const eeprom_strap_v2_t *strap)
{
	BOARD_INFO_SET(BI_STRAP_SN, strap, strap_sn);
	BOARD_INFO_SET(BI_STRAP_REVISION, strap, strap_revision);
	BOARD_INFO_SET(BI_STRAP_MINOR, strap, strap_minor);
	BOARD_INFO_SET(BI_STRAP_CONFIG, strap, strap_config);
	BOARD_INFO_SET(BI_AUDIO_MODULE_VENDOR, strap, audio_module_vendor);
	BOARD_INFO_SET(BI_LEFT_AUDIO_SN, strap, left_audio_sn);
	BOARD_INFO_SET(BI_RIGHT_AUDIO_SN, strap, right_audio_sn);
}

static void _eeprom_load_strap(void)
{
	struct udevice *dev;

	/*
	 * The 256 byte part on 0x51 arrived in EV3.3 and is the only strap
	 * layout we handle. EV3.2 and earlier carry an incompatible record on
	 * 0x50 under a different magic number; those boards simply come up
	 * without strap identity rather than being misread.
	 */
	if (_eeprom_get_dev("eeprom-strap-v2", &dev))
		return;

	if (_eeprom_load(dev, EEPROM_STRAP_ADDRESS_START, EEPROM_STRAP_V2_MAGIC_NUMBER,
			 s_strap_v2_versions, &s_strap, sizeof(s_strap), "strap"))
		return;

	s_strap_valid = true;
	_eeprom_extract_strap_v2(&s_strap);
}

void deckard_eeprom_init(void)
{
	_eeprom_load_core();
	_eeprom_load_strap();
}

bool deckard_eeprom_get_wifi_mac_address(uint8_t mac[EEPROM_MAC_ADDR_LEN])
{
	if (!s_wifi_mac_addr_valid)
		return false;

	memcpy(mac, s_wifi_mac_addr, EEPROM_MAC_ADDR_LEN);
	return true;
}

// Resistance clamp for Forgrand
#define CLAMP_RES_FG(res)						\
	if (res < 475 || res > 685) {					\
		deckard_log("Invalid resistance value: %u\n", res);	\
		res = 575;						\
	}
// Resistance clamp for Olewolff
#define CLAMP_RES_OW(res)						\
	if (res < 530 || res > 770) {					\
		deckard_log("Invalid resistance value: %u\n", res);	\
		res = 645;						\
	}

int deckard_eeprom_fixup_audio_info(void *fdt)
{
	const char *audio_module_vendor;
	const char *firmware_name;
	uint8_t dsm_volume_max;
	uint16_t left_temp, left_resistance, right_temp, right_resistance;
	uint32_t left_temp_reg, right_temp_reg;
	int off = -1;

	if (!s_strap_valid ||
	    !s_board_info[BI_AUDIO_MODULE_VENDOR].present ||
	    s_strap.left_audio_temp_valid != EEPROM_VALID ||
	    s_strap.left_audio_resistance_valid != EEPROM_VALID ||
	    s_strap.right_audio_temp_valid != EEPROM_VALID ||
	    s_strap.right_audio_resistance_valid != EEPROM_VALID) {
		deckard_log("Missing audio strap information\n");
		goto error;
	}

	audio_module_vendor = s_board_info[BI_AUDIO_MODULE_VENDOR].value;
	left_temp = s_strap.left_audio_temp;
	left_resistance = s_strap.left_audio_resistance;
	right_temp = s_strap.right_audio_temp;
	right_resistance = s_strap.right_audio_resistance;

	if (strcmp(audio_module_vendor, "Forgrand") == 0) {
		firmware_name = "dsm_param_FG_2018b.bin";
		dsm_volume_max = 127;
	} else if (strcmp(audio_module_vendor, "Olewolff") == 0) {
		firmware_name = "dsm_param_OW_2018b.bin";
		dsm_volume_max = 127;
	} else if (strcmp(audio_module_vendor, "ggec") == 0) {
		/* Boulder */
		firmware_name = "dsm_param_GG.bin";
		dsm_volume_max = 127; /* TODO */
	} else {
		deckard_log("Invalid audio vendor: %s\n", audio_module_vendor);
		goto error;
	}

	if (strcmp(audio_module_vendor, "ggec") != 0) {
		/* Temperature values are stored in deciC.  We'll accept
		 * temperature values between 15°C and 30°C as valid */
		if (left_temp < 150 || left_temp > 300) {
			deckard_log("Invalid left audio temp: %u\n", left_temp);
			goto error;
		}
		if (right_temp < 150 || right_temp > 300) {
			deckard_log("Invalid right audio temp: %u\n", right_temp);
			goto error;
		}

		/* deciC -> C */
		left_temp /= 10;
		right_temp /= 10;

		/* Temperature is used as a temp / 100, represented as a <4,12>
		 * fixed point value. The math below is an approximation that was
		 * originally used, do we keep it as-is. This includes a /10 bug
		 * which we also keep to match final tuning in the DSM params. */
		left_temp_reg = (left_temp * 1000 + 122) / 244;
		right_temp_reg = (right_temp * 1000 + 122) / 244;

		if (strcmp(audio_module_vendor, "Forgrand") == 0) {
			CLAMP_RES_FG(left_resistance);
			CLAMP_RES_FG(right_resistance);
		} else {
			CLAMP_RES_OW(left_resistance);
			CLAMP_RES_OW(right_resistance);
		}
	} else {
		/* Boulder gets static values for now */
		left_temp_reg = right_temp_reg = 1024;
		left_resistance = right_resistance = 406;
	}

	/* First validate that the speaker side prefixes are what we expect */
	fdt_for_each_node_by_compatible(off, fdt, -1, "maxim,max98390") {
		const char *prefix = fdt_getprop(fdt, off, "sound-name-prefix", NULL);

		if (!prefix ||
		    ((strcmp(prefix, "Speaker Left") != 0) &&
		     (strcmp(prefix, "Speaker Right") != 0))) {
			deckard_log("Device tree node missing sound-name-prefix\n");
			goto error;
		}
	}

	fdt_for_each_node_by_compatible(off, fdt, -1, "maxim,max98390") {
		const char *prefix = fdt_getprop(fdt, off, "sound-name-prefix", NULL);
		bool left = strcmp(prefix, "Speaker Left") == 0;
		uint32_t temp = left ? left_temp_reg : right_temp_reg;
		uint32_t resistance = left ? left_resistance : right_resistance;

		fdt_setprop_u32(fdt, off, "maxim,temperature_calib", temp);

		/* Resistance is used as admittance. (Rdc * 3/11)^-1,
		 * represented as a <4,20> fixed point value. Stored value is
		 * fixed point <6,8>, so math is value * 2^28 */
		fdt_setprop_u32(fdt, off, "maxim,r0_calib",
				268435456 / resistance);

		fdt_setprop_u32(fdt, off, "maxim,dsm_volume_max",
				dsm_volume_max);

		fdt_setprop_string(fdt, off, "maxim,dsm_param_name",
				firmware_name);
	}

	return 0;

error:
	deckard_log("Turning down audio output to 0 on both amps for safety");

	fdt_for_each_node_by_compatible(off, fdt, -1, "maxim,max98390") {
		fdt_setprop_u32(fdt, off, "maxim,dsm_volume_max", 0);
	}

	return -1;
}

int deckard_eeprom_fixup_board_info(void *fdt)
{
	int emitted = 0;
	int node;

	bool at_least_one_present = false;
	for (int i = 0; i < BI_COUNT; i++) {
		if (s_board_info[i].present) {
			at_least_one_present = true;
			break;
		}
	}

	/* Nothing survived validation - don't insert an empty node */
	if (!at_least_one_present) {
		deckard_log("board-info: no valid EEPROM fields, node not created\n");
		return 0;
	}

	node = fdt_find_or_add_subnode(fdt, 0, DECKARD_BOARD_INFO_NODE);
	if (node < 0) {
		deckard_log("WARNING: could not add /%s: %s\n", DECKARD_BOARD_INFO_NODE,
			    fdt_strerror(node));
		return node;
	}

	for (int i = 0; i < BI_COUNT; i++) {
		int ret;

		// Skip missing nodes
		if (!s_board_info[i].present)
			continue;

		ret = fdt_setprop_string(fdt, node, s_board_info[i].name, s_board_info[i].value);
		if (ret < 0) {
			deckard_log("WARNING: could not set %s/%s: %s\n", DECKARD_BOARD_INFO_NODE,
				    s_board_info[i].name, fdt_strerror(ret));
			return ret;
		}
		emitted++;
	}

	deckard_log("board-info: published %d of %d EEPROM fields\n", emitted, BI_COUNT);
	return 0;
}
