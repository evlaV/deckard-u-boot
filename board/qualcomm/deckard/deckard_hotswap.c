
#include <stdbool.h>

#include <command.h>
#include <fs.h>
#include <led.h>
#include <linux/delay.h>

#include "pm8550b.h"
#include "pmk8550.h"
#include "power/slg4ad48669m_hotswap.h"
#include <sysreset.h>

#include "deckard.h"
#include "deckard_hotswap.h"
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

#define PATTERN_ID_EV 0x04
#define PATTERN_ID_DV 0x05

static int16_t pattern_id = -1;

bool deckard_hotswap_supported(struct udevice *slg4ad48669m) {
  uint8_t reg;
  if (pmic_read(slg4ad48669m, REG_PATTERN_ID, &reg, sizeof(reg))) {
    return false;
  }

  pattern_id = reg;

  return (reg >= PATTERN_ID_EV);
}

int deckard_hotswap_enable_charging(struct udevice *slg4ad48669m, bool enable,
                                    bool charge_internal) {

  // Check Pattern ID
  if (pattern_id < 0) {
    deckard_hotswap_supported(slg4ad48669m);
  }

  uint8_t reg;
  if (pmic_read(slg4ad48669m, REG_I2C_VIRTUAL_IN, &reg, sizeof(reg))) {
    return -ENXIO;
  }

  reg |= VI_ENABLE_SLOW_CHARGER;
  if (pattern_id >= PATTERN_ID_DV) {
    if (charge_internal) {
      // Disable Charge Remote
      reg &= ~VI_DV_CHG_REMOTE;
    } else {
      // Set Charge Remote
      reg |= VI_DV_CHG_REMOTE;
    }
  } // Else EV, Ignore Bit, we always cross charge

  return pmic_write(slg4ad48669m, REG_I2C_VIRTUAL_IN, &reg, sizeof(reg));
}

bool deckard_hotswap_get_external_battery_installed(
    struct udevice *slg4ad48669m) {

  uint8_t reg;
  if (pmic_read(slg4ad48669m, REG_REALTIME_STATUS, &reg, sizeof(reg))) {
    return false;
  }

  return !!(reg & RT_PACK_CONNECTED);
}

bool deckard_hotswap_on_external_battery(struct udevice *slg4ad48669m) {

  uint8_t reg;
  if (pmic_read(slg4ad48669m, REG_FET_STATUS, &reg, sizeof(reg))) {
    return false;
  }

  return !!(reg & FET_REM_ACT);
}

bool deckard_hotswap_charger_enabled(struct udevice *slg4ad48669m) {
  uint8_t reg;
  if (pmic_read(slg4ad48669m, REG_I2C_VIRTUAL_IN, &reg, sizeof(reg))) {
    return -ENXIO;
  }

  return !!(reg & VI_ENABLE_SLOW_CHARGER);
}
