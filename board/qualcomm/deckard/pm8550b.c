#include "pm8550b.h"

#include <power/pmic.h>

#define PM8550B_CHGR_BASE                                       0x2600
#define PM8550B_CHGR_CHG_EN_ADDR                                (PM8550B_CHGR_BASE + 0x46)
#define PM8550B_CHGR_CHG_EN_CHARGING_PAUSE_CMD                  0x10
#define PM8550B_CHGR_CHG_EN_CHARGING_ENABLE_CMD                 0x01
#define PM8550B_CHGR_IPRE_CFG_ADDR                              (PM8550B_CHGR_BASE + 0x50)
#define PM8550B_CHGR_ICHG_CFG_ADDR                              (PM8550B_CHGR_BASE + 0x54)
#define PM8550B_CHGR_ICHG_MAX_ADDR                              (PM8550B_CHGR_BASE + 0x55)
#define PM8550B_CHGR_VFLT_CFG_ADDR                              (PM8550B_CHGR_BASE + 0x58)
#define PM8550B_CHGR_VFLT_MAX_ADDR                              (PM8550B_CHGR_BASE + 0x59)
#define PM8550B_CHGR_TERM_CFG_ADDR                              (PM8550B_CHGR_BASE + 0x60)
#define PM8550B_CHGR_TERM_CFG_I_TERM                            0x80
#define PM8550B_CHGR_TERM_CFG_USE_ANALOG_TERMINATION            0x08
#define PM8550B_CHGR_TERM_CFG_ANALOG_CHARGE_CURRENT_TERMINATION 0x07
#define PM8550B_CHGR_WD_CFG_ADDR                                (PM8550B_CHGR_BASE + 0x94)
#define PM8550B_CHGR_WD_CFG_BITE_WDOG_DISABLE_CHARGING_CFG      BIT(7) // disable charging if bite wdog expire
#define PM8550B_CHGR_WD_CFG_BITE_WDOG_INT_EN                    BIT(5)
#define PM8550B_CHGR_WD_CFG_BITE_BARK_WDOG_INT_EN               BIT(4)
#define PM8550B_CHGR_WD_CFG_BITE_WDOG_TIMER_EN_ON_PLUGIN        BIT(1)
#define PM8550B_CHGR_WD_CFG_BITE_WDOG_TIMER_EN                  BIT(0)

#define PM8550B_BATIF_BASE               0x2800
#define PM8550B_BATIF_JEITA_EN           (BATIF_BASE + 0x7F)
#define PM8550B_BATIF_JEITA_FV_WARM      (BATIF_BASE + 0x80)
#define PM8550B_BATIF_JEITA_FV_COOL      (BATIF_BASE + 0x81)
#define PM8550B_BATIF_JEITA_CC_WARM      (BATIF_BASE + 0x82)
#define PM8550B_BATIF_JEITA_CC_COOL      (BATIF_BASE + 0x83)
#define PM8550B_BATIF_JEITA_THOT_MSB     (BATIF_BASE + 0x84)
#define PM8550B_BATIF_JEITA_THOT_LSB     (BATIF_BASE + 0x85)
#define PM8550B_BATIF_JEITA_WARM_MSB     (BATIF_BASE + 0x86)
#define PM8550B_BATIF_JEITA_WARM_LSB     (BATIF_BASE + 0x87)
#define PM8550B_BATIF_JEITA_COOL_MSB     (BATIF_BASE + 0x88)
#define PM8550B_BATIF_JEITA_COOL_LSB     (BATIF_BASE + 0x89)
#define PM8550B_BATIF_JEITA_COLD_MSB     (BATIF_BASE + 0x8A)
#define PM8550B_BATIF_JEITA_COLD_LSB     (BATIF_BASE + 0x8B)
#define PM8550B_BATIF_JEITA_THOT_AFP_MSB (BATIF_BASE + 0x8C)
#define PM8550B_BATIF_JEITA_THOT_AFP_LSB (BATIF_BASE + 0x8D)
#define PM8550B_BATIF_JEITA_COLD_AFP_MSB (BATIF_BASE + 0x8E)
#define PM8550B_BATIF_JEITA_COLD_AFP_LSB (BATIF_BASE + 0x8F)

#define PM8550B_QBG_MAIN_BASE                 (0x4F00)
#define PM8550B_QBG_EN_CTL                    (PM8550B_QBG_MAIN_BASE + 0x46)
#define PM8550B_QBG_EN_CTL_EN                 (0x80)
#define PM8550B_QBG_EN_CTL_DIS                (0x00)
#define PM8550B_QBG_LAST_BURST_AVG_ACC0_DATA0 (PM8550B_QBG_MAIN_BASE + 0xC0)

// NOTE These are 1s currents
enum pre_charge_current
{
	IPRE_100MA,
	IPRE_150MA,
	IPRE_200MA,
	IPRE_250MA,
	// note: PM8550B has 100mA step size above 300mA
	IPRE_300MA,
	IPRE_400MA,
	IPRE_500MA,
	IPRE_600MA,
};

typedef enum
{
	CHGR_CHG_STATUS_INHIBIT,
	CHGR_CHG_STATUS_TRICKLE,
	CHGR_CHG_STATUS_PRECHARGE,
	CHGR_CHG_STATUS_FULLON,
	CHGR_CHG_STATUS_TAPER,
	CHGR_CHG_STATUS_TERMINATION,
	CHGR_CHG_STATUS_PAUSE,
	CHGR_CHG_STATUS_CHARGING_DISABLED
} CHGR_CHG_STATUS;

const char charger_status_string[][16] = {"Inhibit", "Trickle",     "Precharge", "Full On",
                                          "Taper",   "Termination", "Paused",    "Disabled"};

// FOR 1s VBATT IT IS 7500 * MILL_TO_10NANO_RATIO * 3 /  115600 in 10 nano V unit. LSB is 64.879uV/bit
#define VBATT_1S_LSB 19463

// #define CHGR_VBAT_STATUS            (0x2608)
// #define CHGR_VBAT_STATUS_LT_2V      (0x01)
// #define CHGR_VBAT_STATUS_GT_FULL_ON (0x02)

int pm8550b_enable_fuel_guage(struct udevice *pmic, bool enable)
{
	// NOTE We don't need to read and mask here, since this is the only bit

	int ret;
	ret = pmic_reg_write(pmic, PM8550B_QBG_EN_CTL, enable ? PM8550B_QBG_EN_CTL_EN : PM8550B_QBG_EN_CTL_DIS);
	if (ret != 0) {
		printf("Failed to %s Fuel Gauge.\n", enable ? "Enable" : "Disable");
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

int pm8550b_set_fast_charge_current(struct udevice *pmic, uint16_t current_ma)
{
	uint8_t reg;
	int     ret;

	// Scale Current to Register Units (60 mA increments)
	reg = (uint8_t)((current_ma + 30) / 60);

	ret = pmic_reg_write(pmic, PM8550B_CHGR_ICHG_CFG_ADDR, reg);
	if (ret != 0) {
		printf("Failed to Set Fast Charge Current\n");
		return ret;
	}

	return 0;
}

int pm8550b_set_max_fast_charge_current(struct udevice *pmic, uint16_t current_ma)
{
	uint8_t reg;
	int     ret;

	// Scale Current to Register Units (60 mA increments)
	reg = (uint8_t)((current_ma + 30) / 60);

	ret = pmic_reg_write(pmic, PM8550B_CHGR_ICHG_MAX_ADDR, reg);
	if (ret != 0) {
		printf("Failed to Set Max Fast Charge Current\n");
		return ret;
	}

	return 0;
}

int pm8550b_set_float_voltage(struct udevice *pmic, uint16_t voltage_mv)
{
	uint8_t reg;
	int     ret;

	if (voltage_mv < 7200) {
		return -EINVAL;
	}

	// Scale Voltage to Register Units (20 mV increments)
	reg = (uint8_t)((voltage_mv - 7200 + 10) / 20);

	ret = pmic_reg_write(pmic, PM8550B_CHGR_VFLT_CFG_ADDR, reg);
	if (ret != 0) {
		printf("Failed to Set Float Voltage\n");
		return ret;
	}

	return 0;
}

int pm8550b_set_max_float_voltage(struct udevice *pmic, uint16_t voltage_mv)
{
	uint8_t reg;
	int     ret;

	if (voltage_mv < 7200) {
		return -EINVAL;
	}

	// Scale Voltage to Register Units (20 mV increments)
	reg = (uint8_t)((voltage_mv - 7200 + 10) / 20);

	ret = pmic_reg_write(pmic, PM8550B_CHGR_VFLT_MAX_ADDR, reg);
	if (ret != 0) {
		printf("Failed to Set Max Float Voltage\n");
		return ret;
	}

	return 0;
}


int pm8550b_set_termination_current(struct udevice *pmic, uint16_t current_ma)
{
	uint8_t reg;
	int     ret;

	if (current_ma > 450) {
		printf("Clamping Termination Current to 450 mA\n");
		current_ma = 450;
	}
	if (current_ma < 100) {
		printf("Raising Termination Current to 100 mA\n");
		current_ma = 100;
	}

	// Scale Current to Register Units (50 mA increments, with 100 mA offset)
	reg = PM8550B_CHGR_TERM_CFG_I_TERM | (uint8_t)((current_ma - 100 + 25) / 50);

	ret = pmic_reg_write(pmic, PM8550B_CHGR_TERM_CFG_ADDR, reg);
	if (ret != 0) {
		printf("Failed to Set Termination Current\n");
		return ret;
	}

	return 0;
}

int pm8550b_enable_watchdog(struct udevice *pmic, bool enable)
{
	int ret;

	ret = pmic_reg_read(pmic, PM8550B_CHGR_WD_CFG_ADDR);
	if (ret < 0) {
		printf("Failed to %s Watchdog.\n", enable ? "Enable" : "Disable");
		return ret;
	}

	if (enable) {
		ret |= PM8550B_CHGR_WD_CFG_BITE_WDOG_TIMER_EN;
		ret |= PM8550B_CHGR_WD_CFG_BITE_WDOG_DISABLE_CHARGING_CFG;
	} else {
		ret &= ~(PM8550B_CHGR_WD_CFG_BITE_WDOG_TIMER_EN | PM8550B_CHGR_WD_CFG_BITE_WDOG_DISABLE_CHARGING_CFG);
	}

	ret = pmic_reg_write(pmic, PM8550B_CHGR_WD_CFG_ADDR, ret);
	if (ret != 0) {
		printf("Failed to %s Watchdog.\n", enable ? "Enable" : "Disable");
		return ret;
	}

	return 0;
}

int pm8550b_enable_charging(struct udevice *pmic, bool enable)
{
	int ret;

	ret = pmic_reg_read(pmic, PM8550B_CHGR_CHG_EN_ADDR);
	if (ret < 0) {
		printf("Failed to %s Charging.\n", enable ? "Enable" : "Disable");
		return ret;
	}

	if (enable) {
		ret |= PM8550B_CHGR_CHG_EN_CHARGING_ENABLE_CMD;
	} else {
		ret &= ~(PM8550B_CHGR_CHG_EN_CHARGING_ENABLE_CMD);
	}

	ret = pmic_reg_write(pmic, PM8550B_CHGR_CHG_EN_ADDR, ret);
	if (ret != 0) {
		printf("Failed to %s Charging.\n", enable ? "Enable" : "Disable");
		return ret;
	}

	return 0;
}
