/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * PMK8550 Interface
 *
 */

#ifndef __PMK8550_H
#define __PMK8550_H

#include <stdint.h>
#include <stdbool.h>

#include <power/pmic.h>

int pmk8550_get_battery_voltage(struct udevice *pmic, uint16_t *battery_mv);

#endif /* __PMK8550_H */