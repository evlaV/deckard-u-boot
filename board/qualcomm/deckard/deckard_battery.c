
#include "pm8550b.h"
#include "smb1510.h"

#include <linux/delay.h>

#define BATT_2S_UV_OC_PROT_TH_4P4V 4400

void deckard_battery_recover_uvlo(struct udevice *pmk8550, struct udevice *pm8550b)
{
	int      ret;
	uint16_t battery_mv;

	/* Ensure Qualcomm Fuel Gauge is enabled */
	pm8550b_enable_fuel_guage(pm8550b, true);

	/* Get Battery Voltage to Check for UVLO */
	ret = pm8550b_get_battery_voltage(pm8550b, &battery_mv);
	if (ret < 0)
		return;

	printf("Battery: %d mV\n", battery_mv);

#if 0
    // This seems low...
    vbat_thd = BATT_2S_UV_OC_PROT_TH_4P4V;

    smb_x2d2 = get_smb_pcxt(SSDEV_PORT_TYPEC0, SMB_2X_M);
    if (vbat_mv < vbat_thd) {
        /* set the flag  */
        batt_prot_recov = TRUE;

        TraceInfo(BattMngr_ICMLOG, "icm_pre_chg_init: enter Batt UV/OC protection recovery - vbat=%d, status=%d", vbat_mv, status);

        /* save the existing setting */
        status |= bcs_get_batt_vfloat(bcs_cxt, &vflt_mv);

        /* Puhi setting for Batt UV/OC protection recovery */
        status |= bcs_set_vsys_min(bcs_cxt, VSYS_MIN_4P6V);
        status |= bcs_ipre_cfg(bcs_cxt, PCC_1S_250MA_2S_125MA);
        status |= bcs_set_batt_vfloat(bcs_cxt, (bcs_cxt->pmic_info.cell_conf == CELL_2S) ? VFLT_2S_9V : VFLT_1S_4P5V);

        /* disable X2D2 SMB D2/X2 protection */
        if (bcs_cxt->pmic_info.cell_conf == CELL_2S) {
            status |= smb_enable_d2_win_ov(smb_x2d2, FALSE);
            status |= smb_enable_x2_win_ov(smb_x2d2, FALSE);
            status |= smb_enable_vbat2_pulldown(smb_x2d2, FALSE);
            status |= smb_enable_vbat2_uv(smb_x2d2, FALSE);
        }

        /*
         * Next, the following are going to happen, until vbat reaches the level vbat_thd.
         *  Charger stays in Trickle/Precharge state
         *      - VPH 4.6V, QBAT open
         *      - Puhi charges QBAT1 (up to VFLT max)
         *  Battery UV/OC protection recovers; DFET on
         */
        status |= bcs_charging_enable(bcs_cxt, TRUE);
        charger_timer_start(CHARGER_THREAD_TIMER_CHGR, DEAD_BATT_CHG_POLLING_100MS);
    } else {
        /* Main charger Trickle/Pre-Charge settings */
        status = bcs_get_batt_chgr_status(bcs_cxt, &chgr_stat);
        if (chgr_stat == CHARGER_STATUS_TRICKLE || chgr_stat == CHARGER_STATUS_PRECHARGE) {
            status |= bcs_vph_pre_chg_cfg(bcs_cxt, VPH_TRACK_SEL_7_PCT);
            status |= bcs_en_batt_diffsense(bcs_cxt, FALSE);
        } else {
            p_ctx->StateMachine.nextState = S_ICM_NCP_STBY;
        }
    }

    if (status != BATTMNGR_SUCCESS)
        TraceErr(BattMngrWPP_SMChg, "icm_pre_chg_init ERROR: batt_diffsense status=%d", status);
#endif
}

void deckard_battery_enable_charging(struct udevice *pm8550b)
{
	int ret = 0;

	/* Set Fast Charge Current to 2,040 mA (0.75C) */
	ret |= pm8550b_set_fast_charge_current(pm8550b, 2040);

	/* Set Float Voltage to 8,900 mV */
	ret |= pm8550b_set_float_voltage(pm8550b, 8900);

	/* Set Termination Current to 150 mA */
	ret |= pm8550b_set_termination_current(pm8550b, 150);

	/* Ensure Charging Watchdog is Off */
	ret |= pm8550b_enable_watchdog(pm8550b, false);

	/* Enable Charging */
	ret |= pm8550b_enable_charging(pm8550b, true);

	printf("Charger: %s\n", ret == 0 ? "Enabled" : "Setup Failure");
}
