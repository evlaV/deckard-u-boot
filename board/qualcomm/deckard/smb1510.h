/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * SMB1510 Interface via PMK8550 SDAM
 *
 */

#ifndef __SMB1510_H
#define __SMB1510_H

#include <stdint.h>

#include <power/pmic.h>

int smb1510_read(struct udevice *pmic, uint16_t reg);
int smb1510_write(struct udevice *pmic, uint16_t reg, uint8_t value);

void smb1510_set_switching_frequency(struct udevice *pmic, uint8_t frequency);

#endif /* __SMB1510_H */