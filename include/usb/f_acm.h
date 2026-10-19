/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * USB CDC serial (ACM) function helpers
 */

#ifndef __USB_F_ACM_H
#define __USB_F_ACM_H

#include <linux/types.h>

/**
 * acm_set_nonblocking() - don't wait for the host when starting "usbacm"
 *
 * By default starting the "usbacm" stdio device blocks until the host opens
 * the port whenever a cable is connected. When non-blocking, output is
 * buffered (up to CONFIG_USB_FUNCTION_ACM_TX_BUF_SIZE) until it does.
 *
 * @nonblocking: true to return right after registering the gadget
 */
void acm_set_nonblocking(bool nonblocking);

/**
 * acm_board_prefill() - board hook to queue output ahead of live output
 *
 * Called when the "usbacm" stdio device starts, before any other output is
 * queued. Boards can use acm_queue() here to replay earlier logs.
 */
void acm_board_prefill(void);

/**
 * acm_queue() - queue output without sending it
 *
 * @buf: data to queue
 * @len: length of @buf
 * @crlf: translate "\n" to "\r\n"
 */
void acm_queue(const char *buf, unsigned int len, bool crlf);

/**
 * acm_flush() - send queued output
 *
 * Returns early if the host has not opened the port.
 *
 * @timeout_ms: give up after this long
 * Return: 0 if everything was sent, -ETIMEDOUT or -ENOTCONN otherwise
 */
int acm_flush(unsigned long timeout_ms);

/**
 * acm_service() - service the USB controller
 *
 * Lets enumeration and pending transfers make progress while nothing is
 * printed, e.g. from a cyclic function.
 */
void acm_service(void);

#endif /* __USB_F_ACM_H */
