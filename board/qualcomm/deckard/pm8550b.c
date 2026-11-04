#include "pm8550b.h"

#include <power/pmic.h>
#include <linux/delay.h>
#include <linux/kernel.h>

#define PM8550B_CHGR_BASE                                       0x2600
#define PM8550B_SCSCPQ_PK_CHGR2_ADDR                            (PM8550B_CHGR_BASE + 0x25)
#define PM8550B_BUS_ADDR                            			(PM8550B_CHGR_BASE + 0x04)
#define PM8550B_ADC_CAL		                            		(PM8550B_CHGR_BASE + 0x3A)
#define PM8550B_XXX		                            			(PM8550B_CHGR_BASE + 0x3B)
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

#define PM8550B_TYPEC_PORT_BASE                                 0x2B00
#define PM8550B_TYPEC_MISC_STATUS                               (PM8550B_TYPEC_PORT_BASE + 0x0B)
#define PM8550B_TYPEC_VBUS_DETECT                               BIT(5)
#define PM8550B_TYPEC_MODE_CFG_REG                         		(PM8550B_TYPEC_PORT_BASE + 0x44)
#define TYPEC_DISABLE_CMD                               		BIT(0)
#define PM8550B_TYPEC_VCONN_CONTROL_REG						    (PM8550B_TYPEC_PORT_BASE + 0x46)

#define PM8550B_TYPEC_PHY_BASE                                  0x2D00
#define PM8550B_USB_PDPHY_TX_CONTROL_REG                        (PM8550B_TYPEC_PHY_BASE + 0x44)
#define USB_PDPHY_FRAME_FILTER_REG                        		(PM8550B_TYPEC_PHY_BASE + 0x4C)

#define PM8550B_USB_BASE               							0x2900
#define PM8550B_USB_SUSPEND										(PM8550B_USB_BASE + 0x54)

#define PM8550B_BATIF_BASE               0x2800
#define PM8550B_BATIF_SHIP_MODE          (PM8550B_BATIF_BASE + 0x52)
#define PM8550B_BATIF_JEITA_EN           (PM8550B_BATIF_BASE + 0x7F)
#define PM8550B_BATIF_JEITA_FV_WARM      (PM8550B_BATIF_BASE + 0x80)
#define PM8550B_BATIF_JEITA_FV_COOL      (PM8550B_BATIF_BASE + 0x81)
#define PM8550B_BATIF_JEITA_CC_WARM      (PM8550B_BATIF_BASE + 0x82)
#define PM8550B_BATIF_JEITA_CC_COOL      (PM8550B_BATIF_BASE + 0x83)
#define PM8550B_BATIF_JEITA_THOT_MSB     (PM8550B_BATIF_BASE + 0x84)
#define PM8550B_BATIF_JEITA_THOT_LSB     (PM8550B_BATIF_BASE + 0x85)
#define PM8550B_BATIF_JEITA_WARM_MSB     (PM8550B_BATIF_BASE + 0x86)
#define PM8550B_BATIF_JEITA_WARM_LSB     (PM8550B_BATIF_BASE + 0x87)
#define PM8550B_BATIF_JEITA_COOL_MSB     (PM8550B_BATIF_BASE + 0x88)
#define PM8550B_BATIF_JEITA_COOL_LSB     (PM8550B_BATIF_BASE + 0x89)
#define PM8550B_BATIF_JEITA_COLD_MSB     (PM8550B_BATIF_BASE + 0x8A)
#define PM8550B_BATIF_JEITA_COLD_LSB     (PM8550B_BATIF_BASE + 0x8B)
#define PM8550B_BATIF_JEITA_THOT_AFP_MSB (PM8550B_BATIF_BASE + 0x8C)
#define PM8550B_BATIF_JEITA_THOT_AFP_LSB (PM8550B_BATIF_BASE + 0x8D)
#define PM8550B_BATIF_JEITA_COLD_AFP_MSB (PM8550B_BATIF_BASE + 0x8E)
#define PM8550B_BATIF_JEITA_COLD_AFP_LSB (PM8550B_BATIF_BASE + 0x8F)

#define PM8550B_QBG_MAIN_BASE                 (0x4F00)
#define PM8550B_QBG_EN_CTL                    (PM8550B_QBG_MAIN_BASE + 0x46)
#define PM8550B_QBG_EN_CTL_EN                 (0x80)
#define PM8550B_QBG_EN_CTL_DIS                (0x00)
#define PM8550B_QBG_LAST_BURST_AVG_ACC0_DATA0 (PM8550B_QBG_MAIN_BASE + 0xC0)

// ADC5 Gen3 on the PMK8550 (SID 0), same SDAM as the Linux vadc@9000 node. PM8550B channels are
// converted there by selecting SID 7. The PM8550B ADC_PBS block (0x3600) is read-only from apps.
#define ADC5_GEN3_SDAM_BASE              0x9000
#define ADC5_GEN3_HS                     (ADC5_GEN3_SDAM_BASE + 0x45)
#define ADC5_GEN3_HS_READY               BIT(0)
#define ADC5_GEN3_STATUS1                (ADC5_GEN3_SDAM_BASE + 0x46)
#define ADC5_GEN3_STATUS1_CONV_FAULT     BIT(7)
#define ADC5_GEN3_EOC_STS                (ADC5_GEN3_SDAM_BASE + 0x4A)
#define ADC5_GEN3_EOC_CHAN_0             BIT(0)
#define ADC5_GEN3_EOC_CLR                (ADC5_GEN3_SDAM_BASE + 0x4B)
#define ADC5_GEN3_CONV_ERR_CLR           (ADC5_GEN3_SDAM_BASE + 0x4E)
#define ADC5_GEN3_CONV_ERR_CLR_REQ       BIT(0)
#define ADC5_GEN3_SID                    (ADC5_GEN3_SDAM_BASE + 0x4F)
#define ADC5_GEN3_PERPH_CH               (ADC5_GEN3_SDAM_BASE + 0x50)
#define ADC5_GEN3_CHAN_CONV_REQ          BIT(7) // channel 0, immediate conversion
#define ADC5_GEN3_TIMER_SEL              (ADC5_GEN3_SDAM_BASE + 0x51)
#define ADC5_GEN3_TIME_IMMEDIATE         0x01
#define ADC5_GEN3_DIG_PARAM              (ADC5_GEN3_SDAM_BASE + 0x52)
#define ADC5_GEN3_DIG_PARAM_MASK         (0x3 << 4 | 0x3 << 2) // CAL_SEL, DEC_RATIO_SEL
#define ADC5_GEN3_DIG_PARAM_ABS_CAL      (2 << 4)
#define ADC5_GEN3_DIG_PARAM_DEC_1360     (2 << 2)
#define ADC5_GEN3_FAST_AVG               (ADC5_GEN3_SDAM_BASE + 0x53)
#define ADC5_GEN3_FAST_AVG_EN            BIT(7) // 1 sample
#define ADC5_GEN3_ADC_CH_SEL_CTL         (ADC5_GEN3_SDAM_BASE + 0x54)
#define ADC5_GEN3_DELAY_CTL              (ADC5_GEN3_SDAM_BASE + 0x55) // 0 = 15 us settle
#define ADC5_GEN3_CH0_DATA0              (ADC5_GEN3_SDAM_BASE + 0x5C)
#define ADC5_GEN3_CH0_DATA1              (ADC5_GEN3_SDAM_BASE + 0x5D)
#define ADC5_GEN3_CONV_REQ               (ADC5_GEN3_SDAM_BASE + 0xE5)
#define ADC5_GEN3_CONV_REQ_REQ           BIT(0)
// PBS can take up to 15 ms to hand over the SDAM, and up to 500 ms for a conversion with retries
#define ADC5_GEN3_HS_TIMEOUT_US          15000
#define ADC5_GEN3_CONV_TIMEOUT_US        501000
#define ADC5_GEN3_POLL_US                100

#define PM8550B_SID                      7
#define PM8550B_ADC5_GEN3_CHG_TEMP       0x10 // charger die temp

// ADC voltage: code * 1.875 V / 0x70E4, codes above 0x7FFF are below 0 V
#define ADC5_GEN3_VREF_UV                1875000LL
#define ADC5_GEN3_FULL_SCALE_CODE        0x70E4
#define ADC5_GEN3_MAX_CODE               0x7FFF
#define ADC5_GEN3_DATA_INVALID           0x8000
// Charger die temp: T(C) = V / 3.3 mV - 277.12 (qcom_vadc_scale_hw_pm7_chg_temp)

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

#if 0
/// @brief This doesn't work reliably at low voltages, so it is abandoned in place.
/// @param pmic 
/// @param battery_mv 
/// @return 
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
#endif

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

bool pm8550b_is_charging_enabled(struct udevice *pmic)
{
	int ret;

	/* check vbus status */
	ret = pmic_reg_read(pmic, PM8550B_TYPEC_MISC_STATUS);
	if (ret < 0)
		return false;

	return !!(ret & PM8550B_TYPEC_VBUS_DETECT);
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

int pm8550b_pause_charging(struct udevice *pmic, bool pause)
{
	int ret;

	/* check vbus status */
	ret = pmic_reg_read(pmic, PM8550B_CHGR_CHG_EN_ADDR);
	if (ret < 0)
		return false;

	if (pause) {
		ret |=   PM8550B_CHGR_CHG_EN_CHARGING_PAUSE_CMD;
	} else {
		ret &= ~(PM8550B_CHGR_CHG_EN_CHARGING_PAUSE_CMD);
	}

	int write_ret = pmic_reg_write(pmic, PM8550B_CHGR_CHG_EN_ADDR, ret);
	if (write_ret != 0) {
		printf("Failed to pause Charging.\n");
		return write_ret;
	}

	return 0;
}

int pm8550b_set_ship_mode(struct udevice *pmic, bool shipmode)
{
	uint8_t reg = shipmode;
	int     ret;

	ret = pmic_reg_write(pmic, PM8550B_BATIF_SHIP_MODE, reg);
	if (ret != 0) {
		printf("Failed to set pm8550b ship mode\n");
		return ret;
	}

	return 0;
}

static int adc5_gen3_poll(struct udevice *adc, uint reg, uint mask, uint val, int timeout_us)
{
	int ret, i;

	for (i = 0; i < timeout_us / ADC5_GEN3_POLL_US; i++) {
		ret = pmic_reg_read(adc, reg);
		if (ret < 0 || (ret & mask) == val)
			return ret;
		udelay(ADC5_GEN3_POLL_US);
	}

	return -ETIMEDOUT;
}

/// @brief One polled ADC5 Gen3 conversion, following adc5_gen3_do_conversion() in Linux.
/// @param adc PMK8550
/// @param sid PMIC the channel belongs to
/// @param channel ADC5 Gen3 channel number
/// @param code raw ADC code
/// @return 0 on success, negative error code on failure
static int adc5_gen3_read_code(struct udevice *adc, uint sid, uint channel, uint16_t *code)
{
	int ret, lsb, msb;

	// Wait for PBS to hand the SDAM over
	ret = adc5_gen3_poll(adc, ADC5_GEN3_HS, 0xFF, ADC5_GEN3_HS_READY, ADC5_GEN3_HS_TIMEOUT_US);
	if (ret >= 0)
		ret = adc5_gen3_poll(adc, ADC5_GEN3_CONV_REQ, 0xFF, 0, ADC5_GEN3_HS_TIMEOUT_US);
	if (ret < 0)
		return ret;

	ret = pmic_reg_read(adc, ADC5_GEN3_DIG_PARAM);
	if (ret < 0)
		return ret;
	ret = (ret & ~ADC5_GEN3_DIG_PARAM_MASK) | ADC5_GEN3_DIG_PARAM_ABS_CAL | ADC5_GEN3_DIG_PARAM_DEC_1360;

	ret = pmic_reg_write(adc, ADC5_GEN3_DIG_PARAM, ret);
	if (!ret)
		ret = pmic_reg_write(adc, ADC5_GEN3_SID, sid & 0xF);
	if (!ret)
		ret = pmic_reg_write(adc, ADC5_GEN3_PERPH_CH, ADC5_GEN3_CHAN_CONV_REQ);
	if (!ret)
		ret = pmic_reg_write(adc, ADC5_GEN3_TIMER_SEL, ADC5_GEN3_TIME_IMMEDIATE);
	if (!ret)
		ret = pmic_reg_write(adc, ADC5_GEN3_FAST_AVG, ADC5_GEN3_FAST_AVG_EN);
	if (!ret)
		ret = pmic_reg_write(adc, ADC5_GEN3_ADC_CH_SEL_CTL, channel);
	if (!ret)
		ret = pmic_reg_write(adc, ADC5_GEN3_DELAY_CTL, 0);
	if (!ret)
		ret = pmic_reg_write(adc, ADC5_GEN3_CONV_REQ, ADC5_GEN3_CONV_REQ_REQ);
	if (ret)
		return ret;

	// Linux waits for the SDAM interrupt, which fires on channel 0 EOC
	ret = adc5_gen3_poll(adc, ADC5_GEN3_EOC_STS, ADC5_GEN3_EOC_CHAN_0, ADC5_GEN3_EOC_CHAN_0,
			     ADC5_GEN3_CONV_TIMEOUT_US);
	if (ret < 0)
		return ret;

	ret = pmic_reg_read(adc, ADC5_GEN3_STATUS1);
	if (ret < 0)
		return ret;
	if (ret & ADC5_GEN3_STATUS1_CONV_FAULT) {
		printf("ADC5 conversion fault: 0x%02x\n", ret);
		pmic_reg_write(adc, ADC5_GEN3_CONV_ERR_CLR, ADC5_GEN3_CONV_ERR_CLR_REQ);
		pmic_reg_write(adc, ADC5_GEN3_PERPH_CH, 0);
		pmic_reg_write(adc, ADC5_GEN3_CONV_REQ, ADC5_GEN3_CONV_REQ_REQ);
		return -EIO;
	}

	lsb = pmic_reg_read(adc, ADC5_GEN3_CH0_DATA0);
	msb = pmic_reg_read(adc, ADC5_GEN3_CH0_DATA1);

	// Clear EOC. PERPH_CH = 0 tells PBS the request is only a status clear.
	ret = pmic_reg_write(adc, ADC5_GEN3_EOC_CLR, ADC5_GEN3_EOC_CHAN_0);
	if (!ret)
		ret = pmic_reg_write(adc, ADC5_GEN3_PERPH_CH, 0);
	if (!ret)
		ret = pmic_reg_write(adc, ADC5_GEN3_CONV_REQ, ADC5_GEN3_CONV_REQ_REQ);

	if (lsb < 0 || msb < 0)
		return lsb < 0 ? lsb : msb;
	if (ret)
		return ret;

	*code = (msb << 8) | lsb;
	if (*code == ADC5_GEN3_DATA_INVALID)
		return -EINVAL;

	return 0;
}

/// @brief Read the charger die temperature (CHG_TEMP) through the PMK8550 ADC5 Gen3.
/// @param pmic PM8550B, only used for error checking. The conversion runs on the PMK8550.
/// @param temp_mc die temperature in milli-degrees C
/// @return 0 on success, negative error code on failure
int pm8550b_get_die_temp(struct udevice *pmic, int *temp_mc)
{
	struct udevice *pmk8550;
	uint16_t        code;
	int64_t         uv;
	int             ret;

	if (!pmic)
		return -ENODEV;

	ret = pmic_get("pmic@0", &pmk8550);
	if (ret != 0) {
		printf("Failed to Find PMK8550\n");
		return ret;
	}

	ret = adc5_gen3_read_code(pmk8550, PM8550B_SID, PM8550B_ADC5_GEN3_CHG_TEMP, &code);
	if (ret) {
		printf("Failed to Read PM8550B Die Temp: %d\n", ret);
		return ret;
	}

	if (code > ADC5_GEN3_MAX_CODE)
		code = 0;
	uv = (int64_t)code * ADC5_GEN3_VREF_UV / ADC5_GEN3_FULL_SCALE_CODE;
	*temp_mc = (int)((30303LL * uv - 27712LL * 1000000) / 100000);
	// The channel reads 0 V with no charger connected, which converts to -277 C
	if (*temp_mc < 0)
		*temp_mc = 0;

	return 0;
}

int pm8550b_disable_typec_charging(struct udevice *pmic)
{
	int ret;
	
	struct udevice *pm8550b  = pmic;

	if(!pm8550b)
	{
		// Find the PM8550B
		ret = pmic_get("pmic@7", &pm8550b);
		if (ret != 0) {
			printf("Failed to Find PM8550B\n");
			return ret;
		}
	}

	ret = pmic_reg_write(pm8550b, PM8550B_TYPEC_MODE_CFG_REG, TYPEC_DISABLE_CMD); //just set this one bit and set rest of register to 0
	if(ret != 0) goto fail;
	
	ret = pmic_reg_write(pm8550b, PM8550B_USB_PDPHY_TX_CONTROL_REG, 0);
	if(ret != 0) goto fail;
	
	ret = pmic_reg_write(pm8550b, USB_PDPHY_FRAME_FILTER_REG, 0);
	if(ret != 0) goto fail;

	ret = pmic_reg_write(pm8550b, PM8550B_TYPEC_VCONN_CONTROL_REG, 0);
	if(ret != 0) goto fail;

	//toggle the USBIN_SUSPEND bit 0,1,0 to cause the port to reset basically
	ret = pmic_reg_write(pm8550b, PM8550B_USB_SUSPEND, 0);
	mdelay(20); //todo, check if we need this or not
	ret = pmic_reg_write(pm8550b, PM8550B_USB_SUSPEND, 1);
	mdelay(20); //todo, check if we need this or not
	ret = pmic_reg_write(pm8550b, PM8550B_USB_SUSPEND, 0);


	if(ret == 0) return 0;

fail:
	if (ret != 0) {
		printf("Failed to disable typec charging\n");
		return ret;
	}

	return 0;
}

int pm8550b_test(struct udevice *pmic, bool enable)
{
	int ret;
	int write_val; 


	printf("XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX\n");
	
	ret = pmic_reg_read(pmic, PM8550B_ADC_CAL);
	printf("PM8550B_ADC_CAL READ: ret = %d\n", ret);
	write_val = ret; 
	ret = pmic_reg_write(pmic, PM8550B_ADC_CAL, write_val);
	printf("PM8550B_ADC_CAL WRITE : ret = %d\n", ret);

	ret = pmic_reg_read(pmic, PM8550B_XXX);
	printf("PM8550B_XXX READ: ret = %d\n", ret);
	write_val = 0x01; 
	ret = pmic_reg_write(pmic, PM8550B_XXX, write_val);
	printf("PM8550B_XXX WRITE : ret = %d\n", ret);
	ret = pmic_reg_read(pmic, PM8550B_XXX);
	printf("PM8550B_XXX READ: ret = %d\n", ret);
	printf("XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX\n");

	return 0;
}
