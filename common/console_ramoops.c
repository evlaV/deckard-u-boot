
// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2010 Marco Stornelli <marco.stornelli@gmail.com>
 * Copyright (C) 2011 Kees Cook <keescook@chromium.org>
 * Copyright (C) 2012 Google, Inc.
 * Copyright (C) 2025 Alexey Minnekhanov <alexeymin@postmarketos.org>
 * Copyright (C) 2025 Rui Miguel Silva <rui.silva@linaro.org>
 *
 * Stdio driver that will write log output to pstore-ramoops reserved memory.
 * Based on Linux pstore ramoops backend.
 */

#include "dm/device.h"
#include "dm/uclass-id.h"
#include "linux/errno.h"
#include <u-boot/zlib.h>
#include <console.h>
#include <dm.h>
#include <dm/device-internal.h>
#include <dm/lists.h>
#include <log.h>
#include <stdint.h>
#include <stdlib.h>
#include <malloc.h>
#include <mapmem.h>
#include <stddef.h>
#include <stdio_dev.h>
#include <time.h>

#ifdef DEBUG
#include <debug_uart.h>
#endif

#ifndef MAX_WBITS
#define MAX_WBITS 15
#endif

/* Header of persistent RAM zone ringbuffer */
struct persistent_ram_buffer {
	u32    sig;
	u32    start;
	u32    size;
	u8     data[0];
};

#define PERSISTENT_RAM_SIG (0x43474244) /* DBGC */
#define RAMOOPS_KERNMSG_HDR "===="
#define RAMOOPS_READ_BUFFER_SIZE 0x100

enum pstore_type_id {
	PSTORE_TYPE_DMESG	= 0,
	PSTORE_TYPE_MCE		= 1,
	PSTORE_TYPE_CONSOLE	= 2,
	PSTORE_TYPE_FTRACE	= 3,
	PSTORE_TYPE_PPC_RTAS	= 4,
	PSTORE_TYPE_PPC_OF	= 5,
	PSTORE_TYPE_PPC_COMMON	= 6,
	PSTORE_TYPE_PMSG	= 7,
	PSTORE_TYPE_PPC_OPAL	= 8,
	PSTORE_TYPE_UNKNOWN	= 255
};


struct persistent_ram_zone {
	phys_addr_t paddr;
	size_t size;
	struct persistent_ram_buffer *buffer;
	size_t buffer_size;
	int header_length;
	bool content_is_old;
	bool is_compressed;
};

struct console_ramoops_data {
	phys_addr_t base;  /* memory region base */
	u64 region_size;   /* memory region size */
	bool started;

	uint32_t ecc_size;
	uint32_t record_size; /* size of one panic dump recod */
	uint32_t console_size;
	uint32_t ftrace_size;
	uint32_t pmsg_size;

	struct persistent_ram_zone **przs; /* zones for panic dumps, multiple */
	struct persistent_ram_zone *cprz;  /* console zone */
	struct persistent_ram_zone *fprz;  /* ftrace zone */
	struct persistent_ram_zone *mprz;  /* pmsg zone */

	uint32_t max_dump_cnt; /* max number panic dumps (number of zones) */
	uint32_t dump_write_cnt; /* how many dumps were written by us */
};

/* increase and wrap the start pointer, returning the old value */
static size_t buffer_start_add(struct persistent_ram_zone *prz, size_t a)
{
	u32 olds, news;

	olds = prz->buffer->start;
	news = olds + a;
	while (unlikely(news >= prz->buffer_size))
		news -= prz->buffer_size;
	prz->buffer->start = news;

	return olds;
}

/* increase the size counter until it hits the max size */
static void buffer_size_add(struct persistent_ram_zone *prz, size_t a)
{
	size_t olds, news;

	olds = prz->buffer->size;
	if (olds == prz->buffer_size)
		return;

	news = olds + a;
	if (news > prz->buffer_size)
		news = prz->buffer_size;
	prz->buffer->size = news;
}

static int persistent_ram_write(struct persistent_ram_zone *prz, const void *s, unsigned int count)
{
	int rem;
	int c = count;
	size_t start;

	if (unlikely(c > prz->buffer_size)) {
		s += c - prz->buffer_size;
		c = prz->buffer_size;
	}

	buffer_size_add(prz, c);
	start = buffer_start_add(prz, c);
	rem = prz->buffer_size - start;

	if (unlikely(rem < c)) {
		memcpy(prz->buffer->data + start, s, rem);
		//persistent_ram_update_ecc(prz, start, count);
		s += rem;
		c -= rem;
		start = 0;
	}

	memcpy(prz->buffer->data + start, s, c);
	//persistent_ram_update_ecc(prz, start, count);
	//persistent_ram_update_header_ecc(prz);

	return count;
}

static void persistent_ram_zap(struct persistent_ram_zone *prz)
{
	prz->buffer->start = 0;
	prz->buffer->size = 0;
	//persistent_ram_update_header_ecc(prz);
}

static void persistent_ram_free(struct persistent_ram_zone *prz)
{
	if (!prz)
		return;

	unmap_sysmem(prz->buffer);
	free(prz);
}

static struct persistent_ram_zone *persistent_ram_new(phys_addr_t start, size_t size, u32 sig)
{
	struct persistent_ram_zone *prz;

	prz = calloc(sizeof(struct persistent_ram_zone), 1);
	if (!prz) {
		pr_err("failed to allocate persistent ram zone\n");
		goto err;
	}

	prz->paddr = start;
	prz->size = size;
	prz->buffer = map_sysmem(start, size);
	prz->buffer_size = size - sizeof(struct persistent_ram_buffer);

	sig ^= PERSISTENT_RAM_SIG;

	if (prz->buffer->sig == sig) {
		if (prz->buffer->size > prz->buffer_size ||
		    prz->buffer->start > prz->buffer->size) {
			printf("resetting existing invalid buffer: size %u, start %u\n",
				prz->buffer->size, prz->buffer->start);
			persistent_ram_zap(prz);
		} else {
			printf("reusing existing buffer: size %u, start %u\n",
				 prz->buffer->size, prz->buffer->start);
			prz->content_is_old = true;
		}
	} else {
		printf("resetting invalid buffer (sig = 0x%08x)\n", prz->buffer->sig);
		/* Reset missing or invalid memory area. */
		prz->buffer->sig = sig;
		persistent_ram_zap(prz);
	}

	return prz;
err:
	persistent_ram_free(prz);
	return NULL;
}


static int ramoops_read_kmsg_hdr(char *buffer, bool *compressed)
{
	int header_length = 0;
	unsigned long tv_sec, tv_usec;
	char data_type;

	if (sscanf(buffer, RAMOOPS_KERNMSG_HDR "%lld.%lu-%c\n%n",
		   &tv_sec, &tv_usec, &data_type, &header_length) == 3) {
		if (data_type == 'C')
			*compressed = true;
		else
			*compressed = false;
	} else if (sscanf(buffer, RAMOOPS_KERNMSG_HDR "%lld.%lu\n%n",
			  &tv_sec, &tv_usec, &header_length) == 2) {
		*compressed = false;
	} else {
		*compressed = false;
	}

	return header_length;
}

static size_t ramoops_write_kmsg_hdr(struct persistent_ram_zone *prz, bool compressed)
{
	size_t len;
	unsigned long tv_sec, tv_usec;
	char hdr[48];

	/* Is there a better way to get time in struct tiveval format? */
	tv_usec = timer_get_us();
	tv_sec = tv_usec / 1000000;
	tv_usec -= tv_sec * 1000000;

	memset(hdr, '\0', sizeof(hdr));
	snprintf(hdr, sizeof(hdr)-1, RAMOOPS_KERNMSG_HDR "%lu.%lu-%c\n",
		tv_sec, tv_usec, compressed ? 'C' : 'D');
	len = strlen(hdr);
	persistent_ram_write(prz, hdr, len);

	return len;
}

static int ramoops_pstore_write_buf(struct console_ramoops_data *cxt,
				    enum pstore_type_id type,
				    const char *buf,
				    size_t size,
				    bool compressed)
{
	struct persistent_ram_zone *prz;
	size_t hlen;

	if (type == PSTORE_TYPE_CONSOLE) {
		if (!cxt->cprz)
			return -ENOMEM;
		persistent_ram_write(cxt->cprz, buf, size);
		return 0;
	} else if (type == PSTORE_TYPE_FTRACE) {
		if (!cxt->fprz)
			return -ENOMEM;
		persistent_ram_write(cxt->fprz, buf, size);
		return 0;
	} else if (type == PSTORE_TYPE_PMSG) {
		if (!cxt->mprz)
			return -ENOMEM;
		persistent_ram_write(cxt->mprz, buf, size);
		return 0;
	}

	if (type != PSTORE_TYPE_DMESG)
		return -EINVAL;

	if (!cxt->przs)
		return -ENOSPC;

	prz = cxt->przs[cxt->dump_write_cnt];

	hlen = ramoops_write_kmsg_hdr(prz, compressed);
	if (size + hlen > prz->buffer_size)
		size = prz->buffer_size - hlen;
	persistent_ram_write(prz, buf, size);

	cxt->dump_write_cnt = (cxt->dump_write_cnt + 1) % cxt->max_dump_cnt;

	return 0;
}

static void ramoops_console_write_buf(struct console_ramoops_data *cxt, const char *buf, size_t size)
{
	/* prefer to use console zone, if possible; fallback to zone used for panic/oops */
	if (cxt->console_size > 0) {
		ramoops_pstore_write_buf(cxt, PSTORE_TYPE_CONSOLE, buf, size, false);
	} else {
		/* This is worse, can overwrite panic logs from the OS */
		ramoops_pstore_write_buf(cxt, PSTORE_TYPE_DMESG, buf, size, false);
	}
}

static int zerr(int ret)
{
	switch (ret) {
	case Z_STREAM_ERROR:
		return -EINVAL;
	case Z_DATA_ERROR:
		return -EIO;
	case Z_MEM_ERROR:
		return -ENOMEM;
	case Z_ERRNO:
	default:
		return -EFAULT;
	}
}

static int console_decompress_record(void *src, uint32_t size, void *dst, uint32_t *decoded_length)
{
	z_stream strm = { };
	int ret = 0;

	if (!src || !size || !dst || !decoded_length)
		return -EINVAL;

	strm.zalloc = Z_NULL;
	strm.zfree = Z_NULL;
	strm.opaque = Z_NULL;
	strm.avail_in = 0;
	strm.next_in = Z_NULL;

	ret = inflateInit2(&strm, -MAX_WBITS);
	if (ret != Z_OK)
		return zerr(ret);

	strm.next_in = src;
	strm.avail_in = size;
	strm.next_out = dst;
	strm.avail_out = *decoded_length;

	ret = inflate(&strm, Z_FINISH);
	if (ret != Z_STREAM_END) {
		if (ret != Z_OK)
			ret = zerr(ret);
	}

	inflateEnd(&strm);

	*decoded_length = strm.avail_out;

	return ret;
}

static int ramoops_console_dump_buf(struct persistent_ram_zone *prz,
				    enum pstore_type_id type)
{
	struct persistent_ram_buffer *buf;
	u32 decoded_length;
	int start, size;
	u8 *old_log;
	u8 *tmp_log;
	int tmp_size;
	int ret;

	if (!prz)
		return -ENOMEM;

	buf = prz->buffer;
	start = buf->start;
	size = buf->size;
	if (!size) {
		printf("records do not exist or could not be found\n");
		return -ENOMSG;
	}

	if (type == PSTORE_TYPE_DMESG && prz->is_compressed) {
		decoded_length = prz->buffer_size * 3;

		tmp_log = calloc(decoded_length, 1);
		if (!tmp_log)
			return -ENOMEM;

		size -= prz->header_length;
		ret = console_decompress_record(&buf->data[0] + prz->header_length,
						size, tmp_log, &decoded_length);
		if (ret < 0) {
			printf("Decompress: 0x%px size: 0x%x ERROR: %d\n", tmp_log, size, ret);
			free(tmp_log);
			return ret;
		}

		/* now that we know the size copy it to a smaller buffer */
		size = decoded_length;
		old_log = memdup(tmp_log, size);
		free(tmp_log);
	} else {
		old_log = calloc(size + 1, 1);
		if (!old_log)
			return -ENOMEM;

		memcpy(old_log, (const void *)&buf->data[start], size - start);
		memcpy(old_log + size - start, (const void *)&buf->data[0], start);
	}

	tmp_log = calloc(CONFIG_SYS_PBSIZE + 1, 1);
	start = 0;
	while(size - start > 0) {
		memset(tmp_log, '\0', CONFIG_SYS_PBSIZE + 1);
		tmp_size = min(CONFIG_SYS_PBSIZE, size - start);

		memcpy(tmp_log, old_log + start, tmp_size);
		printf("%s", tmp_log);
		start += tmp_size;
	}
	printf("\n");

	free(old_log);
	free(tmp_log);

	return 0;
}

static int ramoops_init_dump_zones(struct console_ramoops_data *cxt, phys_addr_t *paddr, size_t dump_mem_sz)
{
	int err = -ENOMEM;
	size_t zone_sz;
	int i;

	if (!cxt->record_size)
		return 0;

	if (*paddr + dump_mem_sz - cxt->base > cxt->region_size) {
		pr_err("no room for dumps\n");
		return -ENOMEM;
	}
	cxt->max_dump_cnt = dump_mem_sz / cxt->record_size;
	if (!cxt->max_dump_cnt)
		return -ENOMEM;

	cxt->dump_write_cnt = 0;

	cxt->przs = calloc(sizeof(struct persistent_ram_zone *), cxt->max_dump_cnt);
	if (!cxt->przs) {
		pr_err("failed to initialize a prz array for dumps\n");
		goto fail_mem;
	}

	zone_sz = dump_mem_sz / cxt->max_dump_cnt;
	zone_sz = ALIGN_DOWN(zone_sz, 2);

	for (i = 0; i < cxt->max_dump_cnt; i++) {
		cxt->przs[i] = persistent_ram_new(*paddr, zone_sz, 0);
		if (!cxt->przs[i]) {
			pr_err("failed to request mem region (0x%lx @ 0x%llx)\n",
				zone_sz, (unsigned long long)*paddr);

			while (i > 0) {
				i--;
				persistent_ram_free(cxt->przs[i]);
			}
			goto fail_prz;
		}
		*paddr += zone_sz;
	}

	return 0;
fail_prz:
	free(cxt->przs);
fail_mem:
	cxt->max_dump_cnt = 0;
	return err;
}

static int ramoops_init_zone(struct console_ramoops_data *cxt,
			     struct persistent_ram_zone **prz,
			     phys_addr_t *paddr, size_t sz, u32 sig)
{
	if (!sz)
		return 0;

	if (*paddr + sz - cxt->base > cxt->region_size) {
		pr_err("no room for mem region (0x%zx@0x%llx) in (0x%llx@0x%llx)\n",
			sz, (unsigned long long)*paddr,
			cxt->region_size, (unsigned long long)cxt->base);
		return -ENOMEM;
	}

	*prz = persistent_ram_new(*paddr, sz, sig);
	if (*prz == NULL) {
		pr_err("failed to request mem region (0x%zx@0x%llx)\n",
			sz, (unsigned long long)*paddr);
		return -ENOMEM;
	}

	/* we only zap the ram if there was not already a dump in the zone */
	if (!(*prz)->content_is_old)
		persistent_ram_zap(*prz);

	*paddr += sz;

	return 0;
}

/* Called to start the device */
static int console_ramoops_init(struct udevice *dev)
{
	struct console_ramoops_data *priv = dev_get_priv(dev);
	phys_addr_t paddr;
	size_t dump_mem_sz;
	int ret;

	if (priv->started)
		return 0;

	paddr = priv->base;
	dump_mem_sz = priv->region_size - priv->console_size - priv->ftrace_size - priv->pmsg_size;

#ifdef DEBUG
	printascii("console_ramoops_start: starting..\n");
#endif

	priv->przs = NULL;
	priv->cprz = NULL;
	priv->fprz = NULL;
	priv->mprz = NULL;

	ret = ramoops_init_dump_zones(priv, &paddr, dump_mem_sz);
	if (ret) {
		log_err("%s: failed to init oops/panic zones: %d\n", __func__, ret);
#ifdef DEBUG
		printascii("console_ramoops_start: ERR, failed to init oops/panic zones\n");
#endif
		return ret;
	}

	ret = ramoops_init_zone(priv, &priv->cprz, &paddr, priv->console_size, 0);
	if (ret)
		goto fail_init_zone;

	ret = ramoops_init_zone(priv, &priv->mprz, &paddr, priv->pmsg_size, 0);
	if (ret)
		goto fail_init_zone;
	ret = ramoops_init_zone(priv, &priv->fprz, &paddr, priv->ftrace_size, 0);
	if (ret)
		goto fail_init_zone;

#ifdef DEBUG
	printascii("console_ramoops_start: started OK\n");
#endif

	if (IS_ENABLED(CONFIG_DEBUG_UART)) {
		/*
		 * if CONFIG_DEBUG_UART is enabled, and GD_FLG_SERIAL_READY flag
		 * is not set, U-Boot will *ONLY* output to debug_uart and nowhere else.
		 */
		gd->flags |= GD_FLG_SERIAL_READY;
	}

	/* Tell U-Boot's stdio system the we can use console at this point. */
	/* Without this flag it will only output to pre_console buffer */
	gd->flags |= GD_FLG_HAVE_CONSOLE;

	priv->started = true;

	return 0;

fail_init_zone:
	if (priv->cprz) {
		free(priv->cprz);
		priv->cprz = NULL;
	}

	if (priv->fprz) {
		free(priv->fprz);
		priv->fprz = NULL;
	}

	if (priv->mprz) {
		free(priv->mprz);
		priv->mprz = NULL;
	}

	log_err("console_ramoops_start: failed to init one of the zones (console, ftrace, pmsg): %d\n", ret);
#ifdef DEBUG
	printascii("console_ramoops_start: ERR, failed to init one of the zones\n");
#endif
	return ret;
}

static int console_ramoops_start(struct stdio_dev *sdev)
{
	struct udevice *dev = (struct udevice *)sdev->priv;

	return console_ramoops_init(dev);
}

/* Called to stop the device */
static int console_ramoops_stop(struct stdio_dev *sdev)
{
	/* we never stop */
	return 0;
}
void console_ramoops_records_get(enum pstore_type_id type)
{
	struct persistent_ram_zone *prz = NULL;
	struct console_ramoops_data *cxt;
	int header_length = 0;
	struct udevice *dev;
	bool compressed;
	int ret;
	int i;

	ret = uclass_get_device_by_driver(UCLASS_KEYBOARD,
					  DM_DRIVER_GET(console_ramoops),
					  &dev);
	if (ret < 0)
		return;

	cxt = dev_get_priv(dev);
	if (!cxt)
		return;

	ret = console_ramoops_init(dev);
	if (ret < 0)
		return;

	if (type == PSTORE_TYPE_CONSOLE && cxt->console_size) {
		prz = cxt->cprz;
		ramoops_console_dump_buf(prz, PSTORE_TYPE_CONSOLE);
	} else if (type == PSTORE_TYPE_DMESG) {
		for (i = 0; i < cxt->max_dump_cnt; i++) {
			prz = cxt->przs[i];
			header_length = ramoops_read_kmsg_hdr(prz->buffer->data, &compressed);
			if (!header_length)
				continue;

			prz->header_length = header_length;
			prz->is_compressed = compressed;

			ramoops_console_dump_buf(prz, PSTORE_TYPE_DMESG);
		}
	}

}

/* To put a string (accelerator) */
static void console_ramoops_puts(struct stdio_dev *sdev, const char *s)
{
	struct udevice *dev = (struct udevice *)sdev->priv;
	struct console_ramoops_data *cxt = dev_get_priv(dev);
	ramoops_console_write_buf(cxt, s, strlen(s));
	return;
}

/* To put a char */
static void console_ramoops_putc(struct stdio_dev *sdev, const char c)
{
	char s[2] = {c, '\0'};
	console_ramoops_puts(sdev, s);
	return;
}

#ifdef CONFIG_CONSOLE_FLUSH_SUPPORT
static void console_ramoops_flush(struct stdio_dev *sdev) { }
#endif

static int console_ramoops_of_to_plat(struct udevice *dev)
{
	struct console_ramoops_data *priv = dev_get_priv(dev);

	priv->base = dev_read_addr_size(dev, &priv->region_size);
	if (priv->base == FDT_ADDR_T_NONE)
		return -EINVAL;

	priv->record_size = dev_read_u32_default(dev, "record-size", 4096);
	priv->console_size = dev_read_u32_default(dev, "console-size", 0);
	priv->ftrace_size = dev_read_u32_default(dev, "ftrace-size", 0);
	priv->pmsg_size = dev_read_u32_default(dev, "pmsg-size", 0);
	priv->ecc_size = dev_read_u32_default(dev, "ecc-size", 0);

	/* validity checks */
	if (priv->region_size < (priv->record_size + priv->console_size
		+ priv->ftrace_size + priv->pmsg_size + priv->ecc_size)) {
		log_err("ramoops region size is not enough to hold all parts!\n");
		return -EINVAL;
	}

	if (priv->ecc_size) {
		log_warning("ramoops: we do not support ecc\n");
	}

	if (priv->console_size == 0) {
		log_warning("ramoops: console zone size is 0, can't use it to store logs!\n");
	}

	return 0;
}

static int console_ramoops_probe(struct udevice *dev)
{
	struct console_ramoops_data *priv = dev_get_priv(dev);
	struct stdio_dev sdev;

	/* register stdio device */
	memset(&sdev, 0, sizeof(sdev));
	sdev.flags = DEV_FLAGS_OUTPUT | DEV_FLAGS_DM;
	sdev.priv = dev; /* as specified by DEV_FLAGS_DM, priv is struct udevice */
	strcpy(sdev.name, "ramoops");
	sdev.start = console_ramoops_start;
	sdev.stop = console_ramoops_stop;
	sdev.putc = console_ramoops_putc;
	sdev.puts = console_ramoops_puts;
	STDIO_DEV_ASSIGN_FLUSH(&sdev, console_ramoops_flush);

	stdio_register(&sdev);

#ifdef DEBUG
	printascii("console_ramoops_probe: OK\n");
#endif
	printf("%s OK, @ 0x%llx, size 0x%llx\n", __func__, priv->base, priv->region_size);

	return 0;
}

static const struct udevice_id console_ramoops_compatibles[] = {
	{ .compatible = "ramoops" },
	{ }
};

/*
 * Here we pretend to be a keyboard uclass device, so that
 * stdio_add_devices() will find us and call our .probe()
 * so we can register our stdio device.
 */

U_BOOT_DRIVER(console_ramoops) = {
	.name           = "console_ramoops",
	.id             = UCLASS_KEYBOARD,
	.of_match       = console_ramoops_compatibles,
	.of_to_plat     = console_ramoops_of_to_plat,
	.priv_auto	= sizeof(struct console_ramoops_data),
	.probe          = console_ramoops_probe,
	.flags          = 0,
};

#ifdef CONFIG_DEBUG_UART_RAMOOPS
#include <debug_uart.h>

/* Some sanity checks */
#if (CONFIG_DEBUG_UART_RAMOOPS_BASE == 0x0) || (CONFIG_DEBUG_UART_RAMOOPS_SIZE == 0x0)
#error "Base & size of ramoops region have to be defined in config! (DEBUG_UART_RAMOOPS_BASE DEBUG_UART_RAMOOPS_SIZE)"
#endif
#if CONFIG_DEBUG_UART_RAMOOPS_CONSOLE_SIZE == 0x0
#error "DEBUG_UART_RAMOOPS is useless without console region!"
#endif

/* These come from .config now */
#define RAMOOPS_BASE CONFIG_DEBUG_UART_RAMOOPS_BASE
#define RAMOOPS_SIZE CONFIG_DEBUG_UART_RAMOOPS_SIZE
#define CONSOLE_SIZE CONFIG_DEBUG_UART_RAMOOPS_CONSOLE_SIZE
#define FTRACE_SIZE  CONFIG_DEBUG_UART_RAMOOPS_FTRACE_SIZE
#define RECORD_SIZE  CONFIG_DEBUG_UART_RAMOOPS_RECORD_SIZE
#define PMSG_SIZE    CONFIG_DEBUG_UART_RAMOOPS_PMSG_SIZE

/* The size of the region reserved for panic/oops dumps */
#define DUMPS_SIZE    (RAMOOPS_SIZE - CONSOLE_SIZE - FTRACE_SIZE - PMSG_SIZE)
#define CONSOLE_BASE  (RAMOOPS_BASE + DUMPS_SIZE)

#define CONSOLE_BUFFER_SIZE (CONSOLE_SIZE - sizeof(struct persistent_ram_buffer))

/* using __section(".data") here for this static variable to survive relocation */
static struct persistent_ram_buffer *console_zone __section(".data");

static inline void _debug_uart_init(void)
{
	console_zone = (struct persistent_ram_buffer *)CONSOLE_BASE;
	console_zone->sig = PERSISTENT_RAM_SIG;
	console_zone->start = 0;
	console_zone->size = 0;
}

static inline void _debug_uart_putc(int ch)
{
	/* write happens inside the data buffer at offset `start` */
	*(char *)( console_zone->data + console_zone->start ) = (char)( ch & 0xff );

	/* ring buffer size can grow, but not above maximum size */
	if (console_zone->size < CONSOLE_BUFFER_SIZE)
		console_zone->size++;

	/* advance the start of the ring buffer */
	console_zone->start++;
	/* if it goes past the end of the buffer, rewind it back to the begining */
	if (console_zone->start >=CONSOLE_BUFFER_SIZE)
		console_zone->start = 0;
}

DEBUG_UART_FUNCS

#endif
