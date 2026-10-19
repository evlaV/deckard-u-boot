/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * USB serial console (CDC ACM) from U-Boot proper to the kernel.
 *
 * With usb_serial=y in the environment, U-Boot proper starts an ACM gadget,
 * replays SPL's log (see deckard_spl_log.h) and what it printed so far, and
 * continues on "usbacm". It then tells the kernel to bring up g_serial with
 * ttyGS0 as a console, and hands it whatever the host has not read yet. Both
 * stages enumerate with the same USB identity, so a host terminal that
 * reconnects (e.g. `tio --auto-connect`) follows the whole boot.
 */

#ifndef __DECKARD_USB_SERIAL_H
#define __DECKARD_USB_SERIAL_H

#include <linux/kconfig.h>
#include <linux/types.h>

/* /chosen property for the kernel: U-Boot used the USB serial console */
#define DECKARD_USB_SERIAL_PROP		"u-boot,usb-serial"

#define DECKARD_USB_SERIAL_MANUFACTURER	"Valve"
#define DECKARD_USB_SERIAL_PRODUCT	"Steam Frame"

/*
 * Output the USB host has not read when U-Boot boots the kernel, in a
 * /reserved-memory region with this compatible. The kernel's ttyGS console
 * sends it ahead of the kernel log (CONFIG_U_SERIAL_CONSOLE_BOOTLOADER_LOG).
 */
#define DECKARD_USB_SERIAL_LOG_COMPAT	"valve,usb-serial-log"
#define DECKARD_USB_SERIAL_LOG_MAGIC	0x474c5355 /* "USLG" */

struct deckard_usb_serial_log {
	u32 magic;	/* set once the log is complete */
	u32 size;	/* capacity of data[] */
	u32 len;	/* bytes in data[] */
	u32 reserved;
	char data[];	/* terminal stream, with "\r\n" line endings */
};

#if IS_ENABLED(CONFIG_USB_FUNCTION_ACM)
/**
 * deckard_usb_serial_init() - decide whether to use the USB serial console
 *
 * Call early in misc_init_r().
 *
 * Return: true if the USB serial console should be used
 */
bool deckard_usb_serial_init(void);

/**
 * deckard_usb_serial_enabled() - whether the USB serial console is in use
 */
bool deckard_usb_serial_enabled(void);

/**
 * deckard_usb_serial_disable() - give up the controller for this boot
 *
 * @why: what needs the controller, for the log
 */
void deckard_usb_serial_disable(const char *why);

/**
 * deckard_usb_serial_bootargs() - kernel arguments for the USB console
 *
 * @bootargs: kernel command line built so far, used to decide whether the
 *	      UART console has to be named explicitly
 * @buf: output buffer
 * @size: size of @buf
 */
void deckard_usb_serial_bootargs(const char *bootargs, char *buf, size_t size);

/**
 * deckard_usb_serial_ft_setup() - reserve memory to hand unread output over
 *
 * @fdt: the kernel's device tree
 */
void deckard_usb_serial_ft_setup(void *fdt);

/**
 * deckard_usb_serial_handoff() - flush and disconnect before booting the OS
 *
 * Whatever the host has not read is passed to the kernel, if
 * deckard_usb_serial_ft_setup() reserved memory for it.
 */
void deckard_usb_serial_handoff(void);
#else
static inline bool deckard_usb_serial_init(void) { return false; }
static inline bool deckard_usb_serial_enabled(void) { return false; }
static inline void deckard_usb_serial_disable(const char *why) {}
static inline void deckard_usb_serial_bootargs(const char *bootargs, char *buf,
					       size_t size)
{
	buf[0] = '\0';
}
static inline void deckard_usb_serial_ft_setup(void *fdt) {}
static inline void deckard_usb_serial_handoff(void) {}
#endif

#endif /* __DECKARD_USB_SERIAL_H */
