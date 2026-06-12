/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * SM8650 Deckard bootmenu
 *
 */

#ifndef __DECKARD_BOOTMENU_H
#define __DECKARD_BOOTMENU_H

int deckard_bootmenu_populate(void);
int deckard_bootmenu_handle_actions(void);
int deckard_bootmenu_clear_all_entries(void);

#endif