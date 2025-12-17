
#include "pmk8550.h"

#include <linux/delay.h>

/* PON Event Logging */
#define SDAM05_BASE_ADDR		0x7400

#define MEM_OFFSET_START        	0x40
#define PON_EVENT_LOG_AREA_SIZE 	((127 - 11 + 1) * 2) // MEM_011 to MEM_127 on SDMA05&06
#define MAX_PON_EVENT_PER_SDAM		(PON_EVENT_LOG_AREA_SIZE / 4) // 4 bytes each log item

#define PON_LOG_SDAM_COUNT_ADDR          (SDAM05_BASE_ADDR + MEM_OFFSET_START + 5)
#define PON_LOG_ENQUEUE_ADDR             (SDAM05_BASE_ADDR + MEM_OFFSET_START + 6)
#define PON_LOG_ENQUEUE_SDAM_NUM         (SDAM05_BASE_ADDR + MEM_OFFSET_START + 7)
#define PON_PBS_PON_LOGGING_AREA_START   (SDAM05_BASE_ADDR + MEM_OFFSET_START + 11)
#define PON_PBS_PON_LOGGING_AREA_END     (SDAM05_BASE_ADDR + MEM_OFFSET_START + 127)
#define PON_LOG_PUSH_PTR_INDEX(x)        (x - (MEM_OFFSET_START + 11))

#define PON_PBS_KEYPD_PWR_N_S2_RSN     0x0080
#define PON_PBS_RESIN_N_S2_RSN         0x0081
#define PON_PBS_KPDPWR_AND_RESIN_RSN   0x0082
#define PON_PBS_PMIC_WD_RSN            0x0083
#define PON_PBS_PS_HOLD_RSN            0x0084
#define PON_PBS_SW_RST_RSN             0x0085
#define PON_PBS_RESIN_N_DEB_RSN        0x0086
#define PON_PBS_KEYPD_PWR_N_RSN        0x0087
#define PON_PBS_SMPL_RSN               0x0640
#define PON_PBS_PON1_RSN               0x18C1
#define PON_PBS_CBLPWR_RSN             0x18C0
#define PON_PBS_SYS_OK_RSN             0x71C2
#define PON_PBS_RTC_ALARM_RSN          0x0621

#define PON_AOP_PON_REASON1			   0x7269
#define PON_AOP_BOOT1_POFF_REASON1	   0x726C
#define PON_AOP_BOOT1_OFF_REASON	   0x726D

#define PON_PBS_SOFT_RB_SPARE 			0x7148

#define PON_PBS_RAW_XVDD_RB_MASK             0x8000
#define PON_PBS_RAW_DVDD_RB_MASK             0x4000
#define PON_PBS_IMMEDIATE_XVDD_SHUTDOWN_MASK 0x2000
#define PON_PBS_S3_RESET_MASK                0xF0
#define PON_PBS_S3_RESET_BSHIFT              4

#define PM_PON_OPTION_USB_CHG_REDGE_REG (SDAM02_BASE_ADDR + MEM_OFFSET_START + 95)

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

typedef enum
{
	PM_PON_EVENT_RESERVED                = 0x00,
	PM_PON_EVENT_PON_TRIGGER             = 0x01,
	PM_PON_EVENT_OTP_COPY                = 0x02,
	PM_PON_EVENT_TRIM_COMPLETE           = 0x03,
	PM_PON_EVENT_XVLO_CHECK_COMPLETE     = 0x04,
	PM_PON_EVENT_PMIC_CHECK_COMPLETE     = 0x05,
	PM_PON_EVENT_RESET_TRIGGER           = 0x06,
	PM_PON_EVENT_RESET_TYPE              = 0x07,
	PM_PON_EVENT_WARM_RESET_COUNT        = 0x08,
	PM_PON_EVENT_FAULT_REASON_PART1      = 0x09,
	PM_PON_EVENT_FAULT_REASON_PART2      = 0x0A,
	PM_PON_EVENT_PBS_FAULT               = 0x0B,
	PM_PON_EVENT_FUNDAMENTAL_RESET       = 0x0C,
	PM_PON_EVENT_PON_SEQUENCE_START      = 0x0D,
	PM_PON_EVENT_PON_SUCCESS             = 0x0E,
	PM_PON_EVENT_PSHOLD_POLL             = 0x0F,
	PM_PON_EVENT_FAULT_PMIC_B            = 0x10,
	PM_PON_EVENT_FAULT_PMIC_C            = 0x11,
	PM_PON_EVENT_FAULT_PMIC_D            = 0x12,
	PM_PON_EVENT_FAULT_PMIC_E            = 0x13,
	PM_PON_EVENT_FAULT_PMIC_F            = 0x14,
	PM_PON_EVENT_FAULT_PMIC_G            = 0x15,
	PM_PON_EVENT_FAULT_PMIC_H            = 0x16,
	PM_PON_EVENT_FAULT_PMIC_I            = 0x17,
	PM_PON_EVENT_FAULT_PMIC_J            = 0x18,
	PM_PON_EVENT_FAULT_PMIC_K            = 0x19,
	PM_PON_EVENT_FAULT_PMIC_L            = 0x1A,
	PM_PON_EVENT_FAULT_PMIC_M            = 0x1B,
	PM_PON_EVENT_FAULT_PMIC_N            = 0x1C,
	PM_PON_EVENT_INVALID,
	PM_PON_EVENT_COUNT = PM_PON_EVENT_INVALID
} pm_pon_event_type;

static const char *pon_type_str[PON_RESET_TYPE_COUNT] = {
	[PON_RESET_TYPE_UNKNOWN] = "Unknown",
	[PON_RESET_TYPE_PON] = "PON",
	[PON_RESET_TYPE_WARM] = "Warm",
	[PON_RESET_TYPE_SHUTDOWN] = "Shutdown",
	[PON_RESET_TYPE_HARD_RESET] = "Hard Reset",
};

static const char *pon_trigger_str[PON_RESET_TRIGGER_COUNT] = {
	[PON_RESET_TRIGGER_UNKNOWN] = "Unknown",
	[PON_RESET_TRIGGER_SOFT] = "Soft",
	[PON_RESET_TRIGGER_PS_HOLD] = "PS-Hold",
	[PON_RESET_TRIGGER_PMIC_WD] = "PMIC Watchdog",
	[PON_RESET_TRIGGER_KPDPWR_AND_RESIN] = "KPDPWR_N and RESIN_N",
	[PON_RESET_TRIGGER_RESIN] = "RESIN_N",
	[PON_RESET_TRIGGER_KPDPWR] = "KPDPWR_N",
	[PON_RESET_TRIGGER_OTHER] = "TBD",
};

const char *pmk8550_get_pon_type_str(enum pon_reset_type type)
{
	if (type < PON_RESET_TYPE_COUNT)
		return pon_type_str[type];
	return "Invalid";
}

const char *pmk8550_get_pon_trigger_str(enum pon_reset_trigger trigger)
{
	if (trigger < PON_RESET_TRIGGER_COUNT)
		return pon_trigger_str[trigger];
	return "Invalid";
}

int pmk8550_get_pbs_soft_rb_spare(struct udevice *pmic)
{
	int ret = pmic_reg_read(pmic, PON_PBS_SOFT_RB_SPARE);

	return ret;
}

int pmk8550_set_pbs_soft_rb_spare(struct udevice *pmic, u8 val)
{
	int ret = pmic_reg_write(pmic, PON_PBS_SOFT_RB_SPARE,val);

	return ret;
}

enum reboot_command_reason_t pmk8550_get_reboot_command_reason(struct udevice *pmic)
{
	enum reboot_command_reason_t reboot_command_reason;
	int pbs_soft_rb = pmk8550_get_pbs_soft_rb_spare(pmic);
	
	reboot_command_reason = (pbs_soft_rb >> 1) & 0x7F; //bits 7:1 are the reboot reason, bit 0 is unused

	printf("[xxxx] reason->pbs_soft_rb = %d, reboot_command_reason = %d\n",pbs_soft_rb,reboot_command_reason);

	switch(reboot_command_reason)
	{
		case REBOOT_COMMAND_REASON_RECOVERY:
			printf("reboot command reason: REBOOT_COMMAND_REASON_RECOVERY\n");
			break;
		case REBOOT_COMMAND_REASON_BOOTLOADER:
			printf("reboot command reason: REBOOT_COMMAND_REASON_BOOTLOADER\n");
			break;
		case REBOOT_COMMAND_REASON_EDL:			
			printf("reboot command reason: REBOOT_COMMAND_REASON_EDL\n");
			break;
		default:
			printf("reboot command reason: NONE\n");
			break;
	}

	return reboot_command_reason;
}

/// @brief Read the PON log to get the reset type & trigger
/// @param pmic
/// @param reason
/// @return
int pmk8550_get_on_off_reason(struct pon_on_off_reason *reason)
{
	struct udevice *pmic;
	uint8_t pon_events_raw[PON_EVENT_LOG_AREA_SIZE] = {0};
	uint8_t pon_events[PON_EVENT_LOG_AREA_SIZE] = {0};
	uint8_t push_ptr = 0, sdam_count = 0, enqueue_sdam_num = 0;
	uint32_t read_size = (PON_EVENT_LOG_AREA_SIZE >> 1);
	uint16_t data;
	int i, ret;

	ret = pmic_get("pmic@0", &pmic);
	if (ret != 0) {
		printf("Failed to Find PMK8650\n");
		return ret;
	}

	memset(reason, 0, sizeof(*reason));

	// PARSE LOG
	for (i = 0; i < (int) read_size; ++i)
	{
		pon_events_raw[i] = pmic_reg_read(pmic, PON_PBS_PON_LOGGING_AREA_START + i);
	}

	sdam_count = pmic_reg_read(pmic, PON_LOG_SDAM_COUNT_ADDR);
	// 0: Only use 1 SDAM
	// 1: Use 2 SDAM
	// Currently only extend to 2 continuous SDAM.
	if(sdam_count)
	{
		for (i = 0; i < (int) read_size; ++i)
		{
			pon_events_raw[read_size + i] =
				pmic_reg_read(pmic, PON_PBS_PON_LOGGING_AREA_START + 0x100 + i);
		}
	}

	push_ptr = pmic_reg_read(pmic, PON_LOG_ENQUEUE_ADDR);
	enqueue_sdam_num = pmic_reg_read(pmic, PON_LOG_ENQUEUE_SDAM_NUM);
	push_ptr = PON_LOG_PUSH_PTR_INDEX(push_ptr) + (enqueue_sdam_num * read_size);
	if(push_ptr >= PON_EVENT_LOG_AREA_SIZE)
		return -EINVAL;

	for(i = 0; i < push_ptr; i++)
		pon_events[i] = pon_events_raw[push_ptr - i - 1];

	for(i = 0; i < (PON_EVENT_LOG_AREA_SIZE - push_ptr); i++)
		pon_events[push_ptr + i] = pon_events_raw[PON_EVENT_LOG_AREA_SIZE - i - 1] ;

	for(i = 0; i < PON_EVENT_LOG_AREA_SIZE - 2; i += 4) {
		switch (pon_events[i + 2]) {
		case PM_PON_EVENT_RESET_TYPE:
			data = pon_events[i + 1] | pon_events[i];

			// Only use first one
			if (reason->type)
				continue;

			switch (data) {
			case 1:
				reason->type = PON_RESET_TYPE_WARM;
				break;
			case 4:
				reason->type = PON_RESET_TYPE_SHUTDOWN;
				break;
			case 7:
				reason->type = PON_RESET_TYPE_HARD_RESET;
				break;
			default:
				reason->type = PON_RESET_TYPE_PON;
			}
			break;
		case PM_PON_EVENT_RESET_TRIGGER:
			data = pon_events[i + 1] | pon_events[i];

			// Only use first one
			if (reason->trigger)
				continue;

			switch (data) {
			case PON_PBS_KEYPD_PWR_N_S2_RSN:
			case PON_PBS_KEYPD_PWR_N_RSN:
				reason->trigger = PON_RESET_TRIGGER_KPDPWR;
				break;
			case PON_PBS_RESIN_N_S2_RSN :
			case PON_PBS_RESIN_N_DEB_RSN :
				reason->trigger = PON_RESET_TRIGGER_RESIN;
				break;
			case PON_PBS_KPDPWR_AND_RESIN_RSN :
				reason->trigger = PON_RESET_TRIGGER_KPDPWR_AND_RESIN;
				break;
			case PON_PBS_PMIC_WD_RSN :
				reason->trigger = PON_RESET_TRIGGER_PMIC_WD;
				break;
			case PON_PBS_PS_HOLD_RSN:
				reason->trigger = PON_RESET_TRIGGER_PS_HOLD;
				break;
			case PON_PBS_SW_RST_RSN :
				reason->trigger = PON_RESET_TRIGGER_SOFT;
				break;
			default:
				reason->trigger = PON_RESET_TRIGGER_OTHER;
				break;
			}

			reason->trigger_data.sid = (data & 0xF000) >> 12;
			reason->trigger_data.pid = (data & 0x0FF0) >> 4;
			reason->trigger_data.irq = data & 0x7;

			break;
		}

		if (reason->type && reason->trigger)
			break;
	}

	// READ REGISTERS
	// These should be consistent with the data parsed from the logs above
	reason->pon_reason = pmk8550_get_pon_reason(pmic);

	reason->pon_boot1_poff_reason = pmk8550_get_boot1_poff_reason1(pmic);

	reason->pon_boot1_off_reason = pmk8550_get_boot1_off_reason(pmic);

	reason->pon_rtc_time = pmk8550_get_pon_rtc_time(pmic);

	reason->power_button_release_rtc_time = pmk8550_get_power_button_release_rtc_time(pmic);

	reason->reboot_command_reason = pmk8550_get_reboot_command_reason(pmic);

	return 0;
}

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

int pmk8550_get_boot1_poff_reason1(struct udevice *pmic)
{
	int ret = pmic_reg_read(pmic, PON_AOP_BOOT1_POFF_REASON1);

	return ret;
}

int pmk8550_get_boot1_off_reason(struct udevice *pmic)
{
	int ret = pmic_reg_read(pmic, PON_AOP_BOOT1_OFF_REASON);

	return ret;
}

int pmk8550_get_pon_reason(struct udevice *pmic)
{
	int ret = pmic_reg_read(pmic, PON_AOP_PON_REASON1);

	return ret;
}

uint32_t pmk8550_get_pon_rtc_time(struct udevice *pmic)
{
	uint32_t pon_rtc_time = 
		(pmic_reg_read(pmic, 0x715D) << 24) + 
		(pmic_reg_read(pmic, 0x715C) << 16) +
		(pmic_reg_read(pmic, 0x715B) << 8 ) +
		(pmic_reg_read(pmic, 0x715A) << 0 );

	return pon_rtc_time;
}

uint32_t pmk8550_get_power_button_release_rtc_time(struct udevice *pmic)
{
	uint32_t kpd_power_button_release_rtc_time = 
				(pmic_reg_read(pmic, 0x7161) << 24) + 
				(pmic_reg_read(pmic, 0x7160) << 16) +
				(pmic_reg_read(pmic, 0x715F) << 8 ) +
				(pmic_reg_read(pmic, 0x715E) << 0 );

	return kpd_power_button_release_rtc_time;
}
