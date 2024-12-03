#include "pm8550b.h"

#include <power/pmic.h>

#define PM8550B_QBG_MAIN_BASE                 (0x4F00)
#define PM8550B_QBG_EN_CTL                    (PM8550B_QBG_MAIN_BASE + 0x46)
#define PM8550B_QBG_EN_CTL_EN                 (0x80)
#define PM8550B_QBG_EN_CTL_DIS                (0x00)
#define PM8550B_QBG_LAST_BURST_AVG_ACC0_DATA0 (PM8550B_QBG_MAIN_BASE + 0xC0)

// FOR 1s VBATT IT IS 7500 * MILL_TO_10NANO_RATIO * 3 /  115600 in 10 nano V unit. LSB is 64.879uV/bit
#define VBATT_1S_LSB 19463

// #define CHGR_VBAT_STATUS            (0x2608)
// #define CHGR_VBAT_STATUS_LT_2V      (0x01)
// #define CHGR_VBAT_STATUS_GT_FULL_ON (0x02)

// #define CHGR_CHG_EN_ADDR                (0x2646)
// #define CHGR_CHG_EN_CHARGING_ENABLE_CMD BIT(0)

// #define CHGR_IPRE_CFG_ADDR (0x2650)
// #define CHGR_ICHG_CFG_ADDR (0x2654)

int pm8550b_enable_fuel_guage(struct udevice *pmic, bool enable)
{
	int ret;
	ret = pmic_reg_write(pmic, PM8550B_QBG_EN_CTL, enable ? PM8550B_QBG_EN_CTL_EN : PM8550B_QBG_EN_CTL_DIS);
	if (ret != 0) {
		printf("Failed to Enable Fuel Gauge, Dead Battery Recovery will not be available\n");
		return ret;
	}

	return 0;
}

int pm8550b_get_battery_voltage(struct udevice *pmic, uint16_t *battery_mv)
{
	int      ret;
	uint8_t  reg_data[4] = {0};
	uint16_t acc_data[2];
	uint32_t voltage_uv;

	// Read QG ADC
	for (int i = 0; i < 4; i++) {
		ret = pmic_reg_read(pmic, PM8550B_QBG_LAST_BURST_AVG_ACC0_DATA0 + i);
		if (ret < 0) {
			printf("Failed to Read Battery Voltage.\n");
			return ret;
		}
		reg_data[i] = ret;
	}

	// Build MSB Data
	acc_data[0] = reg_data[0] | (reg_data[1] << 8);
	acc_data[1] = reg_data[2] | (reg_data[3] << 8);

	printf("Batt 1: %d mV, Batt 2: %d mv\n", acc_data[0] * VBATT_1S_LSB / 100 / 1000,
	       acc_data[1] * VBATT_1S_LSB / 100 / 1000);

	voltage_uv  = acc_data[0] + acc_data[1];
	voltage_uv  = (voltage_uv * VBATT_1S_LSB) / 100;
	*battery_mv = (voltage_uv / 1000);

	return 0;
}
