
#include "smb1510.h"

#include <errno.h>

#include <linux/delay.h>

#define MAX_WAIT_COUNT    (5)
#define SPMI_BRIDGE_DELAY (30)
#define SMB1510_SID       (0xD)

#define PERIPHERAL_ID(reg) ((u32)(((reg)&0xFF00) >> 8))
#define REGISTER(reg)      ((u32)((reg)&0x00FF))

#define PMK8550_PBS3_CLIENT2_BASE_TRIG_CTL     (0x2142)
#define PMK8550_PBS3_CLIENT2_BASE_TRIG_BRG_PBS (0x10)

#define PMK8550_SDAM15_BASE             (0x7E00)
#define PMK8550_SDAM15_SPMI2_COMMAND    (PMK8550_SDAM15_BASE + 0x4B)
#define CMD_SID_MASK                    (0x0F)
#define CMD_WRITE                       (0x20)
#define CMD_READ                        (0x10)
#define CMD_COMPLETE_STATUS_MASK        (0x40)
#define CMD_SDAM_STATUS_IN_PROCESS      (0x00)
#define CMD_SDAM_STATUS_COMPLETED       (0x40)
#define PMK8550_SDAM15_SPMI2_PERIPHERAL (PMK8550_SDAM15_BASE + 0x4C)
#define PMK8550_SDAM15_SPMI2_REGISTER   (PMK8550_SDAM15_BASE + 0x4D)
#define PMK8550_SDAM15_SPMI2_VALUE      (PMK8550_SDAM15_BASE + 0x4E)

#define SMB1510_REVID_BASE    (0x0100)
#define SMB1510_REVID_SUBTYPE (SMB1510_REVID_BASE + 0x05)
#define SMB_SVST_SUBTYPE      (0x54)

#define SMB1510_SCHG_PERPH0_BASE          (0x3000)
#define SMB1510_SCHG_PERPH0_DCDC_FREQ_SEL (SMB1510_SCHG_PERPH0_BASE + 0xC9)

int smb1510_read(struct udevice *pmic, uint reg)
{
	int cnt = 0;
	int ret;

	// Set register SID/PID/Offset and data
	pmic_reg_write(pmic, PMK8550_SDAM15_SPMI2_COMMAND, SMB1510_SID | CMD_READ);
	pmic_reg_write(pmic, PMK8550_SDAM15_SPMI2_PERIPHERAL, PERIPHERAL_ID(reg));
	pmic_reg_write(pmic, PMK8550_SDAM15_SPMI2_REGISTER, REGISTER(reg));

	// Trigger PBS
	pmic_reg_write(pmic, PMK8550_PBS3_CLIENT2_BASE_TRIG_CTL, PMK8550_PBS3_CLIENT2_BASE_TRIG_BRG_PBS);

	// Wait for read cmd completion
	while (cnt++ < MAX_WAIT_COUNT) {
		udelay(SPMI_BRIDGE_DELAY);
		ret = pmic_reg_read(pmic, PMK8550_SDAM15_SPMI2_COMMAND);
		if ((ret & CMD_COMPLETE_STATUS_MASK)) {
			break;
		}
	}

	if (!(ret & CMD_SDAM_STATUS_COMPLETED)) {
		printf("SMB1510 bridge read not completed, cmd_status = 0x%02X, count = %d\n", ret, cnt);
		return -ETIMEDOUT;
	}

	ret = pmic_reg_read(pmic, PMK8550_SDAM15_SPMI2_VALUE);

	return ret;
}

int smb1510_write(struct udevice *pmic, uint reg, uint value)
{
	int cnt = 0;
	int ret;

	// Set register SID/PID/Offset and data
	pmic_reg_write(pmic, PMK8550_SDAM15_SPMI2_COMMAND, SMB1510_SID | CMD_WRITE);
	pmic_reg_write(pmic, PMK8550_SDAM15_SPMI2_PERIPHERAL, PERIPHERAL_ID(reg));
	pmic_reg_write(pmic, PMK8550_SDAM15_SPMI2_REGISTER, REGISTER(reg));
	pmic_reg_write(pmic, PMK8550_SDAM15_SPMI2_VALUE, (uint8_t)value);

	// Trigger PBS
	pmic_reg_write(pmic, PMK8550_PBS3_CLIENT2_BASE_TRIG_CTL, PMK8550_PBS3_CLIENT2_BASE_TRIG_BRG_PBS);

	// Wait for read cmd completion
	while (cnt++ < MAX_WAIT_COUNT) {
		udelay(SPMI_BRIDGE_DELAY);
		ret = pmic_reg_read(pmic, PMK8550_SDAM15_SPMI2_COMMAND);
		if ((ret & CMD_COMPLETE_STATUS_MASK)) {
			break;
		}
	}

	if (!(ret & CMD_SDAM_STATUS_COMPLETED)) {
		printf("SMB1510 bridge read not completed, cmd_status = 0x%02X, count = %d\n", ret, cnt);
		return -ETIMEDOUT;
	}

	return 0;
}

int smb1510_masked_write(struct udevice *pmic, uint reg, uint mask, uint value)
{
	int ret;

	ret = smb1510_read(pmic, reg);
	if (ret < 0) {
		return ret;
	}

	value = ((uint8_t)ret & ~mask) | (value & mask);

	return smb1510_write(pmic, reg, value);
}

int smb1510_check_subtype(struct udevice *pmic)
{
	int ret = smb1510_read(pmic, SMB1510_REVID_SUBTYPE);
	if (ret < 0) {
		return ret;
	}

	if (ret == SMB_SVST_SUBTYPE) {
		printf("Found SMB1510\n");
		return 0;
	} else {
		printf("SMB1510 read Failed - Check XBL Config!!!!\n");
	}

	return ret;
}

void smb1510_set_switching_frequency(struct udevice *pmic, uint8_t frequency)
{
	smb1510_write(pmic, SMB1510_SCHG_PERPH0_DCDC_FREQ_SEL, frequency);
}
