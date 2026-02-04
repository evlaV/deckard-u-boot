
#include <stdbool.h>

#include <command.h>
#include <fs.h>
#include <linux/delay.h>

#include "pm8550b.h"
#include "pmk8550.h"
#include "power/slg4ad48669m_hotswap.h"
#include <sysreset.h>

#include "deckard.h"
#include "deckard_boulder.h"
#include "deckard_log.h"

/* --- Bit macros --- */
/* LATCHED_FAULTS (RO) */
#define LT_HW_CONTROL BIT(3)
#define LT_DV_CHARGER_INT BIT(4)
#define LT_EV_SLOW_CHARGER BIT(5)
#define LT_DV_USE_INT_BATTERY BIT(5)
#define LT_PACK_STATE_CHANGED BIT(6)
#define LT_INTERNAL_UVLO BIT(7)

/* REALTIME_STATUS (RO) */
#define RT_PACK_CONNECTED BIT(2)
#define RT_EV_SLOW_CHARGER_ENABLED BIT(6)
#define RT_DV_CROSS_CHG_INT BIT(6)
#define RT_DV_CROSS_CHG_REM BIT(7)

/* I2C_VIRTUAL_IN (RW) */
#define VI_USE_INTERNAL_BATTERY BIT(0)
#define VI_DV_CHG_REMOTE BIT(1)
#define VI_ENABLE_SLOW_CHARGER BIT(2)
#define VI_EV_CLEAR_CHARGER_FAULT BIT(3)
// In DV this line must be driven high then low again
// In EV, it needed to be toggled
#define VI_CLEAR_FAULT_EDGE BIT(4)

/* FET_STATUS (RO) */
#define FET_INT_CHG BIT(0)
#define FET_INT_DIS BIT(1)
#define FET_REM_CHG BIT(2)
#define FET_REM_DSG BIT(3)
#define FET_INT_ACT BIT(5)
#define FET_REM_ACT BIT(6)

/* REALTIME_STATUS2 (RO) */
#define RT2_REMOVABLE_V_OK BIT(1)
#define RT2_INTERNAL_V_OK BIT(2)

//these PATTERN_ID_* are expected to increase monotonically to be considered valid
#define PATTERN_ID_EV 0x04
#define PATTERN_ID_DV 0x05

static int deckard_boulder_get_reg_pattern_id(struct udevice *slg4ad48669m) {
  uint8_t reg;
  if (pmic_read(slg4ad48669m, REG_PATTERN_ID, &reg, sizeof(reg))) {
    return -1;
  }

  return (reg);
}

bool deckard_boulder_detected(struct udevice *slg4ad48669m) {
  bool valid_reg_pattern_detected = deckard_boulder_get_reg_pattern_id(slg4ad48669m) >= PATTERN_ID_EV;
  return valid_reg_pattern_detected;
}


static int deckard_boulder_enable_charging_internal(struct udevice *slg4ad48669m, bool enable, enum deckard_boulder_battery_t target_battery) {
  uint8_t reg;
  if (pmic_read(slg4ad48669m, REG_I2C_VIRTUAL_IN, &reg, sizeof(reg))) {
    return -ENXIO;
  }

  reg |= VI_ENABLE_SLOW_CHARGER;
  if ( deckard_boulder_get_reg_pattern_id(slg4ad48669m) >= PATTERN_ID_DV ) {
    //DV and onward, we should do the following
    if (target_battery == BOULDER_BATTERY_INTERNAL) {
      // Disable Charge Remote
      reg &= ~VI_DV_CHG_REMOTE;
    } else {
      // Set Charge Remote
      reg |= VI_DV_CHG_REMOTE;
    }
  } // Else EV, Ignore Bit, we always cross charge

  return pmic_write(slg4ad48669m, REG_I2C_VIRTUAL_IN, &reg, sizeof(reg));
}

bool deckard_boulder_disable_charging(struct udevice *slg4ad48669m, enum deckard_boulder_battery_t target_battery) {
  
  int ret = deckard_boulder_enable_charging_internal(slg4ad48669m, false, target_battery);
  if(ret != 0) return true;

  return false;
}

bool deckard_boulder_enable_charging(struct udevice *slg4ad48669m, enum deckard_boulder_battery_t target_battery) {

  int ret = deckard_boulder_enable_charging_internal(slg4ad48669m, true, target_battery);

  if(ret != 0) return true;

  return false;
}

bool deckard_boulder_external_battery_is_installed(struct udevice *slg4ad48669m) {

  uint8_t reg;
  if (pmic_read(slg4ad48669m, REG_REALTIME_STATUS, &reg, sizeof(reg))) {
    return false;
  }

  return !!(reg & RT_PACK_CONNECTED);
}

bool deckard_boulder_on_external_battery(struct udevice *slg4ad48669m) {

  uint8_t reg;
  if (pmic_read(slg4ad48669m, REG_FET_STATUS, &reg, sizeof(reg))) {
    return false;
  }

  return !!(reg & FET_REM_ACT);
}

bool deckard_boulder_charger_enabled(struct udevice *slg4ad48669m) {
  uint8_t reg;
  if (pmic_read(slg4ad48669m, REG_I2C_VIRTUAL_IN, &reg, sizeof(reg))) {
    return false;
  }

  return !!(reg & VI_ENABLE_SLOW_CHARGER);
}
