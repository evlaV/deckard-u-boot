
#include "pmk8550.h"

#include <linux/delay.h>

// PMK8550
#define ADC_APP1_BASE          (0x9000)
#define ADC_APP1_EOC_STS       (ADC_APP1_BASE + 0x4A)
#define ADC_APP1_SID           (ADC_APP1_BASE + 0x4F)
#define SID_PM8550B            (0x07)
#define ADC_APP1_PERIPH_CH     (ADC_APP1_BASE + 0x50)
#define ADC_APP1_TIMER_SEL     (ADC_APP1_BASE + 0x51)
#define ADC_APP1_ADC_DIG_PARAM (ADC_APP1_BASE + 0x52)
#define ADC_APP1_FAST_AVG      (ADC_APP1_BASE + 0x53)
#define ADC_APP1_ADC_CH_SEL    (ADC_APP1_BASE + 0x54)
#define ADC_APP1_DELAY_CTL     (ADC_APP1_BASE + 0x55)
#define ADC_APP1_CH0_DATA0     (ADC_APP1_BASE + 0x5C)
#define ADC_APP1_CH0_DATA1     (ADC_APP1_BASE + 0x5D)
#define ADC_APP1_TRIGGER_SET   (ADC_APP1_BASE + 0xE5)

#define PON_AOP_PON_REASON1		0x7269

/// @brief Read the VBAT_SNS_Charger ADC for the 2S Voltage.
/// @param pmic
/// @param battery_mv
/// @return
int pmk8550_get_battery_voltage(struct udevice *pmic, uint16_t *battery_mv)
{
	// Zero out Voltage
	*battery_mv = 0;

	// The QC Reference code uses channel 0x8F which is tied to the QC Fuel Gauge.
	// Lots of Data analysis shows that 0x94 is always correct with a 2S system.
	// 0x8F can be artificially high
	const int channel = 0x8F;

	const int channel_sf    = 6;    // Mux Divisor for 2S Channel
	const int vref          = 1875; // ADC Voltage Reference in mV
	const int adc_fs_counts = 28900;

	// Reset ADC, by triggering on peripheral 0
	pmic_reg_write(pmic, ADC_APP1_EOC_STS, 0x00);
	pmic_reg_write(pmic, ADC_APP1_PERIPH_CH, 0x00);
	pmic_reg_write(pmic, ADC_APP1_TRIGGER_SET, 0x01);

	int loops = 0;
	while (loops < 10) {
		int ret = pmic_reg_read(pmic, ADC_APP1_EOC_STS);
		if (ret & 0x01) {
			break;
		}
		loops++;
		mdelay(10);
	}

	// Setup ADC
	pmic_reg_write(pmic, ADC_APP1_SID, SID_PM8550B);
	pmic_reg_write(pmic, ADC_APP1_PERIPH_CH, 0x80);
	pmic_reg_write(pmic, ADC_APP1_TIMER_SEL, 0x01);
	pmic_clrsetbits(pmic, ADC_APP1_ADC_DIG_PARAM, 0x3C, 0x28); // Absolute Cal
	pmic_reg_write(pmic, ADC_APP1_FAST_AVG, 0x83);             // Average 2 Samples
	pmic_reg_write(pmic, ADC_APP1_ADC_CH_SEL, channel);
	pmic_reg_write(pmic, ADC_APP1_DELAY_CTL, 0x0A); // 500 us settling
	// Trigger Conversion
	pmic_reg_write(pmic, ADC_APP1_TRIGGER_SET, 0x01);

	loops = 0;
	while (loops < 10) {
		int ret = pmic_reg_read(pmic, ADC_APP1_EOC_STS);
		if (ret & 0x01) {
			uint32_t voltage;
			int      data[2];

			data[0] = pmic_reg_read(pmic, ADC_APP1_CH0_DATA0);
			data[1] = pmic_reg_read(pmic, ADC_APP1_CH0_DATA1);
			voltage = data[1] << 8 | data[0];
			if (voltage > adc_fs_counts) {
				voltage = 0;
			}

			voltage *= vref * channel_sf;
			voltage /= adc_fs_counts;

			*battery_mv = voltage;

			break;
		}
		loops++;
		mdelay(10);
	}

	pmic_reg_write(pmic, ADC_APP1_EOC_STS, 0x00);

	// printf("Battery: %d mV\n", *battery_mv);

	return (0);
}

int pmk8550_get_pon_reason(struct udevice *pmic)
{
	int ret = pmic_reg_read(pmic, PON_AOP_PON_REASON1);

	return ret;
}
