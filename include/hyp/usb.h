/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef __HYP_USB_H__
#define __HYP_USB_H__

#include <dm/ofnode.h>

/**
 * hyp_usb_node() - the DT node the hypervisor takes over from the guest
 *
 * The first enabled node carrying the `hypervisor-takeover` flag, or a null
 * ofnode when the board has none.  Used by the hypervisor at boot and by the
 * EL1 board setup that hands the controller over (scripts/USB_GADGET.md).
 */
#if CONFIG_IS_ENABLED(HYP_USB)
ofnode hyp_usb_node(void);
#else
static inline ofnode hyp_usb_node(void) { return ofnode_null(); }
#endif

#endif /* __HYP_USB_H__ */
