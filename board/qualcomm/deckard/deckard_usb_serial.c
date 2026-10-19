// SPDX-License-Identifier: GPL-2.0+
/*
 * USB serial console (CDC ACM) from U-Boot proper to the kernel.
 * See deckard_usb_serial.h for the overall flow.
 */

#include <console.h>
#include <cyclic.h>
#include <env.h>
#include <g_dnl.h>
#include <membuf.h>
#include <time.h>
#include <asm/global_data.h>
#include <linux/string.h>
#include <soc/qcom/smem.h>
#include <soc/qcom/socinfo.h>
#include <usb/f_acm.h>

#include "deckard_devices.h"
#include "deckard_log.h"
#include "deckard_usb_serial.h"

DECLARE_GLOBAL_DATA_PTR;

/*
 * Before disconnecting, keep answering the host for this long. Vanishing in
 * the middle of a control transfer (e.g. while the host's cdc_acm probes us)
 * leaves the host's hub stuck in transfer timeouts for seconds, during which
 * it misses the next stage's device entirely.
 */
#define USB_SERIAL_SETTLE_MS	200

static char s_serial[16];
static bool s_enabled;
static struct cyclic_info s_cyclic;
static bool s_prefilled;

/* Same identity as the kernel's g_serial, see deckard_usb_serial_bootargs() */
static const char *usb_serial_set_identity(void)
{
	struct socinfo *info;
	size_t size;

	if (!s_serial[0] && !qcom_smem_init()) {
		info = qcom_smem_get(QCOM_SMEM_HOST_ANY, SMEM_HW_SW_BUILD_ID, &size);
		if (!IS_ERR_OR_NULL(info) &&
		    offsetof(struct socinfo, serial_num) < size)
			snprintf(s_serial, sizeof(s_serial), "%u",
				 le32_to_cpu(info->serial_num));
	}

	g_dnl_set_manufacturer(DECKARD_USB_SERIAL_MANUFACTURER);
	g_dnl_set_product(DECKARD_USB_SERIAL_PRODUCT);
	if (s_serial[0])
		g_dnl_set_serialnumber(s_serial);

	return s_serial[0] ? s_serial : NULL;
}

static void usb_serial_cyclic(struct cyclic_info *c)
{
	acm_service();
}

/* Disconnect without leaving the host stuck on a half-done request */
static void usb_serial_stop(void)
{
	ulong start = get_timer(0);

	while (get_timer(start) < USB_SERIAL_SETTLE_MS)
		acm_service();

	acm_flush(500);
	cyclic_unregister(&s_cyclic);

	/* Dropping usbacm from the console stops it and disconnects */
	console_state_clear_flags(USB_CONSOLE_MASK);
	console_state_activate();
}

bool deckard_usb_serial_init(void)
{
	const char *slot = env_get("booted_slot");

	s_enabled = env_get_yesno("usb_serial") == 1;
	if (!s_enabled)
		return false;

	/* The controller is needed in host mode to boot from a USB disk */
	if (slot && slot[0] == 'U') {
		deckard_log("usb_serial: booted from USB, not starting\n");
		s_enabled = false;
		return false;
	}

	usb_serial_set_identity();
	acm_set_nonblocking(true);

	/* answer the host promptly even while nothing is being printed */
	cyclic_register(&s_cyclic, usb_serial_cyclic, 1000, "usb_serial");

	deckard_log("usb_serial: enabled\n");

	return true;
}

bool deckard_usb_serial_enabled(void)
{
	return s_enabled;
}

void deckard_usb_serial_disable(const char *why)
{
	if (!s_enabled)
		return;

	deckard_log("usb_serial: stopping, %s needs the USB controller\n", why);
	s_enabled = false;

	usb_serial_stop();
}

/* Called when "usbacm" starts: replay our output so far */
void acm_board_prefill(void)
{
	if (!s_enabled || s_prefilled)
		return;
	s_prefilled = true;

#if CONFIG_IS_ENABLED(CONSOLE_RECORD)
	{
		/* peek at a copy so 'fastboot oem console' still sees it all */
		struct membuf rec = gd->console_out;
		char chunk[256];
		int n;

		while ((n = membuf_get(&rec, chunk, sizeof(chunk))) > 0)
			acm_queue(chunk, n, true);
	}
#endif
}

void deckard_usb_serial_bootargs(const char *bootargs, char *buf, size_t size)
{
	const char *serial;
	int n;

	buf[0] = '\0';
	if (!s_enabled)
		return;

	serial = usb_serial_set_identity();

	/*
	 * ttyGS0 goes first so the last console= (which becomes /dev/console)
	 * stays what it was. Naming any console stops the kernel from using
	 * stdout-path, so name the UART too if nothing else did.
	 */
	n = snprintf(buf, size,
		     "console=ttyGS0 g_serial.enable=1 g_serial.idVendor=0x%04x g_serial.idProduct=0x%04x g_serial.iManufacturer=%s g_serial.iProduct=\"%s\"",
		     CONFIG_USB_GADGET_VENDOR_NUM, CONFIG_USB_GADGET_PRODUCT_NUM,
		     DECKARD_USB_SERIAL_MANUFACTURER, DECKARD_USB_SERIAL_PRODUCT);

	if (serial && n < size)
		n += snprintf(buf + n, size - n, " g_serial.iSerialNumber=%s",
			      serial);

	if (!strstr(bootargs, "console=") && n < size)
		snprintf(buf + n, size - n, " console=ttyMSM0,115200n8");
}

void deckard_usb_serial_handoff(void)
{
	if (!s_enabled)
		return;

	usb_serial_stop();
}
