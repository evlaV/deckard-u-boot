// SPDX-License-Identifier: GPL-2.0+
#ifndef __HYPERVISOR_H__
#define __HYPERVISOR_H__

#include <stdbool.h>
#include <linux/types.h>

/*
 * Guest<->EL2 HVC service codes.  Project-local numbers, not SMMI function
 * identifiers (claiming an SMMI number would also require GetHypervisor/
 * GetLimits - see scripts/HYPERVISOR.md).  Shared by the EL2 handler in hyp/hv.c
 * and the guest-side `hvc` command in cmd/smccc.c.
 */
#define HV_GET_ID			0x8000ULL
#define HV_ECHO			0x8001ULL
#define HV_ECHO_STR		0x8002ULL	/* x1 = guest VA of NUL string */
#define HV_ID_MAGIC		0x55424248ULL		/* "UBh" - proof EL2 ran */
#define HV_ECHO_KEY		0x5a5aULL
#define HV_ABI_PSA_X0		1ULL				/* function code in x0 */
#define HV_ENOTSUPPORTED	(-0x8022LL)		/* SMMI_NOT_SUPPORTED */

/* Secondary-core services (hyp/smp.c, scripts/SMP.md): */
#define HV_SMP_ON			0x8010ULL	/* x1 = MPIDR to start */
#define HV_SMP_PRINT		0x8011ULL	/* x1 = guest VA of NUL string */
#define HV_SMP_HELLO		0x8012ULL	/* this core is alive at EL1 */
#define HV_SMP_DONE		0x8013ULL	/* this core is parked */
#define HV_SMP_DUMP		0x8014ULL	/* print the vCPU table */

/* Self-test services (hyp/test.c, scripts/TESTING.md): */
#define HV_TEST_RESULT		0x8020ULL	/* x1 = stage, x2 = value, x3 = expect */

/* USB controller hand-over (hyp/usb.c, scripts/USB_GADGET.md): */
#define HV_USB_TAKEOVER		0x8030ULL	/* the guest is done with the controller */

/* The guest's last word before it starts the OS (hyp/smmu.c): returns the number of stream
 * mappings of U-Boot's own that EL2 removed.  May be issued more than once. */
#define HV_OS_HANDOFF		0x8031ULL

#ifdef CONFIG_HYPERVISOR
int hv_init(void);
void hv_start_vm(u64 entrypoint, u64 offset);

/**
 * hyp_mmio_watch() - trap a guest MMIO region and hand every access to a driver
 *
 * [start, end) is cleared from the hypervisor's stage-2 identity map (2 MiB
 * blocks, rounded out), so each guest access there takes a stage-2 translation
 * fault which hyp_s2_emulate() replays write-through against the real device
 * and then reports here.  hyp_mmio_unwatch() puts the blocks back.  The map
 * has to exist first, so a driver registers its watch from its HYP_DRIVER
 * probe - the Hyp DM pass runs in hv_start_vm() right after hyp_s2_install().
 *
 * The hook fn(cpu, far, store, size, val, from_guest) is called with the value
 * involved and may MODIFY it: for stores before the write-through (the
 * paravirtualisation point), for loads after the physical read (it may
 * override what the guest sees).  A hook lives with the driver that owns the
 * device, and runs at EL2 after relocation: it and everything it touches must
 * be __hyp / __hyp_data / __hyp_rodata.
 *
 * EL2's own accesses are never trapped: HCR_EL2.VM enables stage 2 for the EL1&0
 * regime only, and the `mmio.el2-unaffected` self-test reads and writes a
 * watched block from EL2 to prove it.  So a hook only ever runs for the guest
 * (U-Boot at EL1 or the kernel), and @from_guest is true for every call; it is
 * kept for hooks written when this was thought to be otherwise.
 *
 * And U-Boot *is* EL1 code once hv_start_vm() drops it there, so from_guest is
 * true for it too: hyp_s2_is_uboot_access() (below) tells U-Boot's own accesses
 * from the kernel's by the trapping ELR.
 *
 * @start: first address of the watched span
 * @end: one past the last address
 * @fn: hook called for every trapped access
 */
typedef void (*hyp_mmio_watch_fn)(int cpu, u64 far, bool store, u32 size,
				  u64 *val, bool from_guest);
void hyp_mmio_watch(u64 start, u64 end, hyp_mmio_watch_fn fn);
void hyp_mmio_unwatch(u64 start);

/**
 * hyp_mmio_deny() - take a region away from the guest
 *
 * Like hyp_mmio_watch() the covering 2 MiB blocks are cleared from stage-2, but
 * nothing is replayed: a guest load from [start, end) reads zero and a guest
 * store is dropped.  Other addresses inside the same blocks keep being replayed
 * against the device as for any watch.  hyp_mmio_unwatch(start) gives the
 * region back.
 *
 * @start: first address of the denied span
 * @end: one past the last address
 */
void hyp_mmio_deny(u64 start, u64 end);

/**
 * hyp_mmio_emulate() - a virtual device in the guest's physical address space
 *
 * The covering 2 MiB blocks are cleared from stage 2 as for hyp_mmio_deny(), and every guest
 * access to [start, end) goes to @fn instead of any hardware.  For a store @val is the value
 * written; for a load @fn fills it in.  @fn returns false for an access the device does not
 * take (a load then reads zero).  It runs on the core that trapped, with interrupts masked,
 * and must not block.  hyp_mmio_unwatch(start) removes the device.
 *
 * @start: first address of the device
 * @end: one past the last address
 * @fn: access handler
 */
typedef bool (*hyp_mmio_emu_fn)(int cpu, u64 pa, bool store, u32 size, u64 *val);
void hyp_mmio_emulate(u64 start, u64 end, hyp_mmio_emu_fn fn);

/*
 * hyp_printf() - write one whole hypervisor log line, from one core
 * @tag: the caller's console tag ("[HYP] ", "[SPY] ")
 * @fmt: printf format for the message
 *
 * The line gets the tag and the core's number in front, goes into the log ring that the :2222
 * console serves (hyp/log.c) and, while `log uart` is on, out of the debug UART under the
 * console lock.  hprintf() in hyp/hv.h is the usual caller.
 */
void hyp_printf(const char *tag, const char *fmt, ...);

/* The lock that keeps one core's EL2 print from landing in the middle of another's (hyp/smp.c);
 * a trap hook that prints several lines takes it around each. */
void hyp_smp_console_lock(void);
void hyp_smp_console_unlock(void);

/**
 * hyp_gic_own_irq() - keep an SPI for the hypervisor
 *
 * The interrupt is acknowledged and deactivated at EL2 and never injected into
 * the guest.  The handler runs under the GIC lock with interrupts masked and
 * must quiesce the device itself.  hyp_gic_disarm_irq() stops the line from
 * being signalled; hyp_gic_disown_irq() also drops the handler.
 *
 * @intid: INTID of the SPI (32 and up)
 * @level: level-triggered rather than edge
 * @fn: handler
 */
typedef void (*hyp_irq_fn)(int intid);
int hyp_gic_own_irq(int intid, bool level, hyp_irq_fn fn);
void hyp_gic_disarm_irq(int intid);
void hyp_gic_disown_irq(int intid);

/**
 * hyp_s2_is_uboot_access() - did U-Boot's own code make the trapped access a
 * watch hook is being called for?  True for EL2 hypervisor code and for EL1
 * code inside the U-Boot image; false for anything else (the kernel).
 *
 * @cpu: the hook's cpu argument (whose trap is being replayed)
 */
bool hyp_s2_is_uboot_access(int cpu);

/**
 * __hyp_data - declares a non-const variable for EFI runtime section
 *
 * This macro indicates that a variable is non-const and should go into the
 * EFI runtime section, and thus still be available when the OS is running.
 *
 * Only use on variables not declared const.
 *
 * Example:
 *
 * ::
 *
 *   static __hyp_data my_computed_table[256];
 */
#define __hyp_data __section(".data.hyp")

/**
 * __hyp_rodata - declares a read-only variable for EFI runtime section
 *
 * This macro indicates that a variable is read-only (const) and should go into
 * the EFI runtime section, and thus still be available when the OS is running.
 *
 * Only use on variables also declared const.
 *
 * Example:
 *
 * ::
 *
 *   static const __hyp_rodata my_const_table[] = { 1, 2, 3 };
 */
#define __hyp_rodata __section(".rodata.hyp")

/**
 * __hyp - declares a function for EFI runtime section
 *
 * This macro indicates that a function should go into the EFI runtime section,
 * and thus still be available when the OS is running.
 *
 * Example:
 *
 * ::
 *
 *   static __hyp compute_my_table(void);
 */
#define __hyp __section(".text.hyp")

#else
static inline int hv_init(void) { return 0; }
static inline void hv_start_vm(u64 entrypoint) { }

#define __hyp_data
#define __hyp_rodata
#define __hyp
#endif
#endif /* __HYPERVISOR_H__ */
