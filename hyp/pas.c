// SPDX-License-Identifier: GPL-2.0+
/*
 * Loading a remote processor for the guest: the part the secure world normally plays.
 *
 * The OS's PAS driver (qcom_q6v5_pas) starts a DSP with a handful of SCM calls to the secure
 * world, which authenticates the image, protects its memory, sets the processor up and takes it
 * out of reset.  Behind TrustZone that is also the hypervisor's job - it lends the memory to
 * the processor's VM - and with EL2 holding the place of the hypervisor TrustZone cannot do it
 * any more.  EL2 plays the whole role: the calls for the processors it owns are answered here,
 * and TrustZone never sees them.  Nothing is authenticated: the image is the one the OS loaded.
 *
 *   IS_SUPPORTED, SET_REMOTE_STATE     answer yes
 *   INIT_IMAGE (pas_id, metadata)      read the entry point from the ELF header in the metadata
 *   MEM_SETUP (pas_id, addr, size)     nothing to protect, remember the region
 *   AUTH_AND_RESET (pas_id)            the DTB image: nothing; the processor's: start it
 *   SHUTDOWN (pas_id)                  the DTB image: nothing; the processor's: halt and reset it
 *   MEM_PROT_ASSIGN (service 0xc)      any processor's memory: nothing to protect, succeed
 *
 * The OS has done its part before AUTH_AND_RESET - the processor's power domains and rail
 * votes, the XO, the interconnect bandwidth, the firmware in its carveout - so what is left is
 * the processor itself, which is the same on every Hexagon subsystem since the sc7280 LPASS
 * (drivers/remoteproc/qcom_q6v5_adsp.c): the clocks its core and its bus masters need, the
 * boot address, and the boot FSM.  The register windows and the PAS ids are the DT's: the node
 * is tagged `hypervisor-pas` and carries `hypervisor-pas-ids = <image dtb>` and the windows
 * "qdsp6ss" (the core's subsystem registers), "evb-sel" (the LPASS eFuse register that makes
 * the core boot from RST_EVB) and "aon-cc" (the clock controller with the two branch clocks of
 * the core's NoC and AXI master).
 *
 * The core's first stage finds its configuration in QDSP6SS BOOT_PARAMS0..5, which the loader
 * writes before the boot FSM runs (the Trace32 loader for the lanai ADSP,
 * Lanai.LA.1.0/.../std_loadbuild_silicon_q6.cmm, is the documentation): the base of its DTB image,
 * the upper bits of that address, the chip id (family << 16 | id), the low 16 bits of
 * TCSR_SOC_HW_VERSION (read from the TCSR), the platform (type << 24 | subtype << 16 | major << 8 |
 * minor), and the size of the DTB's footprint.  It selects its device tree from that chip and platform.  The DTB's address
 * and size are the DT's (the second `memory-region`); the chip id and the platform are the node's
 * `hypervisor-pas-boot-params`.
 *
 * The processor's own memory accesses go through the apps SMMU: hyp/smmu.c maps its streams as
 * bypass before the OS's SMMU driver probes.
 */
#define LOG_CATEGORY LOGC_HYP
#define pr_fmt(fmt) "[HYP] " fmt

#include <hypervisor.h>
#include <dm/ofnode.h>
#include <errno.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/types.h>
#include <asm/io.h>
#include <asm/ptrace.h>
#include <asm/unaligned.h>
#include <string.h>
#include <hyp/spinlock.h>

#include "hv.h"

/* The SCM call convention (drivers/firmware/qcom/qcom_scm-smc.c): x0 is
 * <fast bit> <64-bit bit> <owner> <service> <command>, x1 the argument description and
 * x2... the arguments; the results come back in x1..x3, x0 is the call's own status. */
#define SCM_OWNER_SIP		2
#define SCM_SVC_PIL		2
#define SCM_SVC_MP		0x0c
#define MP_ASSIGN		0x16

#define PAS_INIT_IMAGE		0x01
#define PAS_MEM_SETUP		0x02
#define PAS_AUTH_AND_RESET	0x05
#define PAS_SHUTDOWN		0x06
#define PAS_IS_SUPPORTED	0x07
#define PAS_SET_REMOTE_STATE	0x0a
#define PAS_GET_RSCTABLE	0x21
#define PAS_RSCTABLE_BUFFER_NOT_SUFFICIENT	20	/* the response code the OS's driver retries on */
#define PAS_RSC_MAX		4096

#define SCM_ARG_BASE		2	/* x2: the first argument */

/* The Hexagon subsystem (QDSP6SS, SM8650 layout) */
#define QDSP6SS_RST_EVB		0x10
#define QDSP6SS_NMI		0x40	/* bit 0: raise an NMI in the core */
/* In the TCSR block */
#define TCSR_SOC_HW_VERSION	0x8000
#define TCSR_BOOT_IMEM_DISABLE	0x13300	/* 1: the Q6 may reach its TCM (the Trace32 loader sets it) */

#define QDSP6SS_BOOT_PARAMS(i)	(0x60 + 4 * (i))
#define QDSP6SS_BOOT_CORE_START	0x400
#define QDSP6SS_BOOT_CMD	0x404
#define QDSP6SS_BOOT_STATUS	0x408
#define QDSP6SS_INTF_HALTREQ	0x2088
#define QDSP6SS_INTF_HALTACK	0x208c
#define QDSP6SS_RESET_CFG	0x2440
#define QDSP6SS_RESET_CMD	0x2444
#define QDSP6SS_CORE_CBCR	0x48040
#define QDSP6SS_SLEEP_CBCR	0x48060
#define QDSP6SS_XO_CBCR		0x48120
#define QDSP6SS_NMI_STATUS	0x44
#define QDSP6SS_DBG_NMI_PWR_STATUS	0x304	/* in sleep, power collapse, clock gating, core state */
#define QDSP6SS_CORE_STATUS	0x2028	/* the AXI masters' idle and clock-on bits */
#define QDSP6SS_CSR		0x80000	/* the CSR block, 0x6880000 */
#define QDSP6SS_WDOG_STATUS	(QDSP6SS_CSR + 0x4008)
#define QDSP6SS_WAKEUP_STATUS	(QDSP6SS_CSR + 0xc02c)
#define GCC_LPASS_DSP_GDSC_VOTE	0x14514c	/* GCC + 0x4514c: bit 31 flips about every 17 ms while the firmware idles */

/* The core's PLL (Lucid OLE) */
#define QDSP6SS_PLL		0x40000
#define PLL_MODE		0x00
#define PLL_OPMODE		0x04
#define PLL_L_VAL		0x10
#define PLL_ALPHA_VAL		0x14
#define PLL_USER_CTL		0x18
#define PLL_USER_CTL_U		0x1c
#define PLL_CONFIG_CTL		0x20
#define PLL_CONFIG_CTL_U	0x24
#define PLL_CONFIG_CTL_U1	0x28
#define PLL_TEST_CTL		0x2c
#define PLL_TEST_CTL_U		0x30
#define PLL_TEST_CTL_U1		0x34
#define PLL_TEST_CTL_U2		0x38
#define PLL_LOCK_DET		BIT(31)
#define PLL_UPDATE_BYPASS	BIT(23)
#define PLL_RESET_N		BIT(2)
#define PLL_OUTCTRL		BIT(0)
#define PAS_Q6_PLL_HZ		1190400000	/* the NOM corner of the core clock table */

/* The always-on clock controller's ring-oscillator PLL (Pongo OLE), the first block of LPASS_AON_CC */
#define RO_PLL			0x0
#define RO_PLL_L_VAL		0x10
#define RO_PLL_USER_CTL		0x14
#define RO_PLL_USER_CTL_U	0x18
#define RO_PLL_CONFIG_CTL	0x1c	/* CONFIG_CTL, _U, _U1, _U2, TEST_CTL, _U, _U1, _U2, _U3 follow */
#define RO_PLL_LOCK_DET		BIT(31)
#define RO_PLL_CAL_XO_PRESENT	BIT(10)
#define RO_PLL_IN_CALIBRATION	BIT(9)
#define RO_PLL_XO_CLK_AT_OUT	BIT(13)	/* in USER_CTL_U */
#define RO_PLL_POST_DIV_EVEN_2	BIT(10)	/* in USER_CTL: even output divided by 2 */
#define RO_PLL_EVEN_EN		BIT(1)

/* LPASS_AON_CC: the branch clocks of the core's NoC and AXI master */
#define LPASS_AON_CC_LPI_NOC_HS_CBCR	0x21008
#define LPASS_AON_CC_Q6_AXIM_CBCR	0x21034

#define CBCR_ENABLE		BIT(0)
#define CBCR_OFF		BIT(31)

#define PAS_WAIT_US		100000

#define PAS_MAX_SUBSYS		2

/* One ELF32 program header (the PAS metadata starts with the image's ELF header and its table) */
struct pas_phdr {
	u32 type, offset, vaddr, paddr, filesz, memsz, flags, align;
};

#define PAS_MAX_PHDR		64
#define PT_LOAD			1
#define PF_W			2

struct pas_sub {
	bool valid;
	u32 id;			/* the processor's PAS id */
	u32 dtb_id;		/* the id of its DTB image, 0 for none */
	u64 qdsp6ss;		/* register windows */
	u64 evb_sel;
	u64 aon_cc;
	u64 tcsr;		/* the TCSR block */
	u64 entry;		/* the image's entry point, from its ELF header */
	u64 mem, size;		/* its carveout, from MEM_SETUP */
	u32 boot_params[6];	/* what the firmware's first stage reads from QDSP6SS BOOT_PARAMS0..5 */
	bool running;
	bool core_up;		/* started and not halted: `running` goes with the OS's shutdown, this does not */
	u32 nphdr;		/* the ELF program headers of the image, from its metadata */
	struct pas_phdr phdr[PAS_MAX_PHDR];
	char name[24];		/* the DT node's name: its string does not outlive the DT */
	u8 rsc[PAS_RSC_MAX];	/* the resource table the OS gets (PAS_GET_RSCTABLE), if the DT has one */
	u32 rsc_len;
};

static __hyp_data struct pas_sub subsys[PAS_MAX_SUBSYS];
static __hyp_data struct spinlock pas_lock = __SPINLOCK_UNLOCKED;

/*
 * A diagnostic for a processor that boots and says nothing: `pas smoke` on the console makes the
 * next AUTH_AND_RESET start a program of EL2's instead of the firmware.  It stores a signature at
 * the end of the carveout and spins - if the signature shows up, the core runs, reaches DRAM
 * through the SMMU and the interconnect, and what is wrong is in the firmware's own start; if not,
 * it is the clocks, the power, the boot address or the memory path.  It is Hexagon code (llvm-mc
 * -triple=hexagon): r1 = ##SMOKE_SIG_OFF-in-carveout; memw(r1) = SMOKE_SIG; spin.
 */
#define SMOKE_SIG		0x48535341	/* "ASSH" */
static __hyp_data bool pas_smoke;

/* `pas tz` on the console: stop answering and let the secure world have the calls, to see what it
 * does with them (it needs a hypervisor behind it for some processors; the ADSP may not) */
static __hyp_data bool pas_passthrough;
static __hyp_data u32 pas_run_ms;	/* a diagnostic: halt the core this long after it started */
static __hyp_data u32 pas_watch_ms;	/* a diagnostic: print its SMMU state and faults this long, see hyp_smmu_watch() */
static __hyp_data u32 pas_ctx_ms;	/* a diagnostic: serve its SMMU context this long, see hyp_pas_set_ctx_ms() */
static __hyp_data u32 pas_nmi_ms;	/* ... after an NMI and this long a wait for its crash handler */
static __hyp_data u32 pas_nmi_phase = 2;	/* ... sent while bit 31 of GCC's LPASS DSP GDSC vote register reads this (2: any) */
#define PAS_RING_ENTRIES	128	/* .scratch/trace.py's ring: an index word at +0, 32-byte records from +0x100 */
static __hyp_data u64 pas_ring_pa;	/* a diagnostic: the entry tracer's ring, see pas_ring_watch() */
static __hyp_data u32 pas_ring_ms;	/* ... watch it this long after the next start */
static __hyp_data u32 pas_poll_ms;	/* a diagnostic: sample the core's status this long, see pas_poll() */
#define PAS_RO_PLL_NEVER	0xffffffffU
static __hyp_data u32 pas_ro_pll_ms;	/* a diagnostic: when the always-on PLL comes up, see hyp_pas_set_ro_pll() */
static __hyp_data bool pas_leave = true;	/* a shutdown touches no register, the core keeps running: see `pas leave` */

/* The window the node's reg-names calls @name, or 0 */
static u64 pas_window(ofnode node, const char *name)
{
	phys_addr_t size;
	fdt_addr_t base;
	int idx = ofnode_stringlist_search(node, "reg-names", name);

	if (idx < 0)
		return 0;
	base = ofnode_get_addr_size_index_notrans(node, idx, &size);

	return base == FDT_ADDR_T_NONE ? 0 : base;
}

static void pas_node(ofnode node)
{
	struct pas_sub *s = NULL;
	u32 ids[2] = { 0, 0 };

	for (int i = 0; i < PAS_MAX_SUBSYS; i++)
		if (!subsys[i].valid) {
			s = &subsys[i];
			break;
		}
	if (!s)
		return;

	if (!ofnode_device_is_compatible(node, "qcom,sm8650-adsp-pas")) {
		hprintf("pas: %s: no processor driver here for it, left to the secure world\n",
			ofnode_get_name(node));
		return;
	}
	if (ofnode_read_u32_array(node, "hypervisor-pas-ids", ids, 2) &&
	    ofnode_read_u32_array(node, "hypervisor-pas-ids", ids, 1)) {
		hprintf("pas: %s has no hypervisor-pas-ids\n", ofnode_get_name(node));
		return;
	}

	s->qdsp6ss = pas_window(node, "qdsp6ss");
	s->evb_sel = pas_window(node, "evb-sel");
	s->aon_cc = pas_window(node, "aon-cc");
	if (!s->qdsp6ss || !s->evb_sel || !s->aon_cc) {
		hprintf("pas: %s lacks a qdsp6ss, evb-sel or aon-cc window\n", ofnode_get_name(node));
		return;
	}

	/* The DTB image's carveout is the second memory-region; BOOT_PARAMS0..5 as described above */
	if (ids[1]) {
		ofnode dtb;
		phys_addr_t dtb_size;
		fdt_addr_t dtb_base;
		u32 params[2];
		ofnode tcsr = ofnode_by_compatible(ofnode_null(), "qcom,sm8650-tcsr");
		fdt_addr_t tcsr_base = ofnode_valid(tcsr) ? ofnode_get_addr_index(tcsr, 0) : FDT_ADDR_T_NONE;

		dtb = ofnode_parse_phandle(node, "memory-region", 1);
		dtb_base = ofnode_valid(dtb) ? ofnode_get_addr_size_index_notrans(dtb, 0, &dtb_size) :
			FDT_ADDR_T_NONE;
		if (dtb_base == FDT_ADDR_T_NONE ||
		    tcsr_base == FDT_ADDR_T_NONE ||
		    ofnode_read_u32_array(node, "hypervisor-pas-boot-params", params, 2)) {
			hprintf("pas: %s: no DTB carveout, TCSR or hypervisor-pas-boot-params\n",
				ofnode_get_name(node));
			return;
		}

		s->boot_params[0] = (u32)dtb_base;
		s->boot_params[1] = (u32)(dtb_base >> 32);
		s->boot_params[2] = params[0];
		s->tcsr = tcsr_base;
		s->boot_params[3] = readl(tcsr_base + TCSR_SOC_HW_VERSION) & 0xffff;
		s->boot_params[4] = params[1];
		s->boot_params[5] = (u32)dtb_size;
	}

	{
		int len = 0;
		const void *rsc = ofnode_read_prop(node, "hypervisor-pas-rsctable", &len);

		if (rsc && len > 0 && len <= PAS_RSC_MAX) {
			memcpy(s->rsc, rsc, len);
			s->rsc_len = len;
			hprintf("pas: %s: a %d byte resource table from the DT\n", ofnode_get_name(node), len);
		}
	}
	s->id = ids[0];
	s->dtb_id = ids[1];
	strlcpy(s->name, ofnode_get_name(node), sizeof(s->name));
	s->valid = true;
	hprintf("pas: %s: PAS id %u (DTB image %u), QDSP6SS %#llx, EVB select %#llx, AON CC %#llx\n",
		s->name, s->id, s->dtb_id, s->qdsp6ss, s->evb_sel, s->aon_cc);
}

static void pas_each_tagged(ofnode parent)
{
	ofnode node;

	ofnode_for_each_subnode(node, parent) {
		if (ofnode_is_enabled(node) && ofnode_read_bool(node, "hypervisor-pas"))
			pas_node(node);

		pas_each_tagged(node);
	}
}

void __hyp hyp_pas_init(void)
{
	pas_each_tagged(ofnode_root());
	hyp_clean_range(subsys, sizeof(subsys));
}

static struct pas_sub *__hyp pas_find(u32 id)
{
	for (int i = 0; i < PAS_MAX_SUBSYS; i++)
		if (subsys[i].valid && (subsys[i].id == id || (subsys[i].dtb_id && subsys[i].dtb_id == id)))
			return &subsys[i];

	return NULL;
}

/* Register access that survives a window the bus refuses (hyp/phys.c) */
static int __hyp pas_write(u64 pa, u32 val)
{
	return hyp_phys_write(pa, &val, sizeof(val));
}

static int __hyp pas_read(u64 pa, u32 *val)
{
	return hyp_phys_read(pa, val, sizeof(*val));
}

/* Wait for (reg & mask) == want; the register is read through pas_read(), so a refusal ends it */
static int __hyp pas_wait(u64 pa, u32 mask, u32 want)
{
	u64 t0 = hyp_ticks();
	u32 val;

	do {
		if (pas_read(pa, &val))
			return -EIO;
		if ((val & mask) == want)
			return 0;
		hyp_udelay(10);
	} while (!hyp_ticks_elapsed(t0, PAS_WAIT_US));

	return -ETIMEDOUT;
}

static int __hyp pas_branch_on(u64 cbcr)
{
	return pas_write(cbcr, CBCR_ENABLE) ?: pas_wait(cbcr, CBCR_OFF, 0);
}

/*
 * The firmware's clock driver gives each idle clock domain of the always-on clock controller a
 * "safe" configuration when it starts, and for the camera and NoC domains that source is the
 * controller's ring-oscillator PLL.  A domain whose root is running cannot finish switching to
 * a PLL that is off, so the update never completes and the driver gives up (ClockDriver.c
 * "Unable to configure the MUX": the second fatal error of the real firmware, found with the
 * AON_CAM_AHB domain's CMD_RCGR stuck at UPDATE).  The driver treats this PLL as
 * not its own to switch on, so the loader does it: the register values and the 1171 * 32.768 kHz
 * = 38.4 MHz setting are the driver's own (ClockBSP.c, lpass_aon_cc_ro_pll), the order is its
 * HAL (HALclkPongoolePLL.c).
 */
static int __hyp pas_ro_pll_on(struct pas_sub *s)
{
	static const u32 settings[] = {		/* CONFIG_CTL .. TEST_CTL_U3, then USER_CTL_U */
		0x60000F6A, 0x0001C808, 0, 0x04008174,
		0, 0x0080C496, 0x40100180, 0x441001BC, 0x000003D8,
	};
	u64 pll = s->aon_cc + RO_PLL;
	u32 mode = 0;
	int ret;

	pas_read(pll + PLL_MODE, &mode);
	if (mode & RO_PLL_LOCK_DET)
		return 0;

	ret = pas_write(pll + PLL_MODE, 0);
	for (int i = 0; !ret && i < ARRAY_SIZE(settings); i++)
		ret = pas_write(pll + RO_PLL_CONFIG_CTL + 4 * i, settings[i]);
	ret = ret ?: pas_write(pll + RO_PLL_USER_CTL, 0);
	ret = ret ?: pas_write(pll + RO_PLL_USER_CTL_U, 0x00E50302 | RO_PLL_XO_CLK_AT_OUT);
	ret = ret ?: pas_write(pll + RO_PLL_L_VAL, 1171);
	ret = ret ?: pas_write(pll + PLL_MODE, RO_PLL_CAL_XO_PRESENT);
	ret = ret ?: pas_wait(pll + PLL_MODE, RO_PLL_IN_CALIBRATION, 0);
	ret = ret ?: pas_write(pll + RO_PLL_USER_CTL, RO_PLL_POST_DIV_EVEN_2 | RO_PLL_EVEN_EN | PLL_OUTCTRL);
	ret = ret ?: pas_write(pll + PLL_OPMODE, 1);			/* run */
	ret = ret ?: pas_write(pll + PLL_MODE, RO_PLL_CAL_XO_PRESENT | PLL_RESET_N);
	ret = ret ?: pas_wait(pll + PLL_MODE, RO_PLL_LOCK_DET, RO_PLL_LOCK_DET);
	ret = ret ?: pas_write(pll + PLL_MODE, RO_PLL_CAL_XO_PRESENT | PLL_RESET_N | PLL_OUTCTRL);

	return ret;
}

/*
 * The firmware's clock driver sets the core to its operating frequency by slewing a PLL that
 * is already running - and treats a PLL that is not as a fatal error (ClockCPU.c, "this scenario
 * happens during standalone Q6 validations where the Q6 PLL is not brought up": the first run
 * of the real firmware died there, 68 ms in, before it created any task but rcinit's).  So the
 * loader brings it up: a Lucid OLE PLL at 19.2 MHz * L, the same register sequence the Valve loader uses
 * for the CDSP.  The firmware chooses the core clock source and divider itself.
 */
static int __hyp pas_pll_on(struct pas_sub *s, u32 hz)
{
	u64 pll = s->qdsp6ss + QDSP6SS_PLL;
	u32 l = hz / 19200000, alpha = (hz % 19200000) * 32 / 9375;
	int ret;

	ret = pas_write(pll + PLL_MODE, 0);
	ret = ret ?: pas_write(pll + PLL_L_VAL, 0x44U << 24 | 0x44U << 16 | l);
	ret = ret ?: pas_write(pll + PLL_ALPHA_VAL, alpha);
	ret = ret ?: pas_write(pll + PLL_USER_CTL, 0x00008400 | 1);
	ret = ret ?: pas_write(pll + PLL_USER_CTL_U, 0x00000005);
	ret = ret ?: pas_write(pll + PLL_CONFIG_CTL, 0x20485699);
	ret = ret ?: pas_write(pll + PLL_CONFIG_CTL_U, 0x00182261);
	ret = ret ?: pas_write(pll + PLL_CONFIG_CTL_U1, 0x82AA299C);
	ret = ret ?: pas_write(pll + PLL_TEST_CTL, 0);
	ret = ret ?: pas_write(pll + PLL_TEST_CTL_U, 0x00000003);
	ret = ret ?: pas_write(pll + PLL_TEST_CTL_U1, 0x00009000);
	ret = ret ?: pas_write(pll + PLL_TEST_CTL_U2, 0x00000034);
	ret = ret ?: pas_write(pll + PLL_MODE, PLL_UPDATE_BYPASS);
	ret = ret ?: pas_write(pll + PLL_OPMODE, 0);			/* standby */
	ret = ret ?: pas_write(pll + PLL_MODE, PLL_RESET_N | PLL_UPDATE_BYPASS);
	ret = ret ?: pas_write(pll + PLL_OPMODE, 1);			/* run */
	ret = ret ?: pas_wait(pll + PLL_MODE, PLL_LOCK_DET, PLL_LOCK_DET);
	ret = ret ?: pas_write(pll + PLL_MODE, PLL_RESET_N | PLL_OUTCTRL);

	return ret;
}

/*
 * Sample the core's own status registers (twice a millisecond at most) for @ms and print every
 * change, with the time since boot.  Once the LPASS register space stops answering the board
 * resets, so the last line printed says when the core (or the bus) went away and in which state.
 */
static void __hyp pas_poll(struct pas_sub *s, u32 ms)
{
	static const u32 regs[] = {
		QDSP6SS_DBG_NMI_PWR_STATUS, QDSP6SS_CORE_STATUS, QDSP6SS_NMI_STATUS, QDSP6SS_WAKEUP_STATUS,
		QDSP6SS_WDOG_STATUS,
	};
	u32 last[ARRAY_SIZE(regs)] = { 0 }, cur[ARRAY_SIZE(regs)];
	u64 t0 = hyp_ticks();
	bool first = true;

	do {
		bool changed = first;

		for (int i = 0; i < ARRAY_SIZE(regs); i++) {
			cur[i] = ~0U;
			pas_read(s->qdsp6ss + regs[i], &cur[i]);
			/* the watchdog's counter runs: its expiry flag is the state */
			if (cur[i] != last[i] && regs[i] != QDSP6SS_WDOG_STATUS)
				changed = true;
			if (regs[i] == QDSP6SS_WDOG_STATUS && (cur[i] ^ last[i]) & ~0x1ffffeU)
				changed = true;
		}
		if (changed) {
			hprintf("pas: %s: +%llu us pwr %#x core %#x nmi %#x wake %#x wdog %#x\n", s->name,
				hyp_ticks_us(), cur[0], cur[1], cur[2], cur[3], cur[4]);
			for (int i = 0; i < ARRAY_SIZE(regs); i++)
				last[i] = cur[i];
		}
		first = false;
		hyp_udelay(500);
	} while (!hyp_ticks_elapsed(t0, ms * 1000));
}

/*
 * Watch the entry tracer's ring (scripts/ADSP.md, "The entry tracer") for @ms after the start: a
 * DSP that hangs makes the board reset within about 150 ms, and what the DSP wrote only reaches DRAM
 * while it runs, so the AP copies each new record into DRAM next to the ring (+0x2000, same layout), which
 * outlives the reset and is read over JTAG afterwards (.scratch/trace-read.py snap).  What the DSP wrote
 * bypassed this core's caches: clean and invalidate each line before reading it.
 */
static void __hyp pas_ring_read(u64 pa, void *buf, u32 len)
{
	for (u64 a = pa & ~63ULL; a < pa + len; a += 64)
		asm volatile("dc civac, %0" :: "r"(a) : "memory");
	asm volatile("dsb sy" ::: "memory");
	hyp_phys_read(pa, buf, len);
}

static void __hyp pas_ring_watch(u32 ms)
{
	u64 t0 = hyp_ticks(), snap = pas_ring_pa + 0x2000;
	u32 seen = 0, idx = 0;

	if (!pas_ring_pa)
		return;
	do {
		pas_ring_read(pas_ring_pa, &idx, sizeof(idx));
		if (idx > 0x1000000)
			idx = seen;			/* the bus is going away: keep what there is */
		for (; seen < idx; seen++) {
			u32 r[8], k;

			if (idx - seen > PAS_RING_ENTRIES)
				seen = idx - PAS_RING_ENTRIES;
			k = seen + 1;
			pas_ring_read(pas_ring_pa + 0x100 + 32 * (k % PAS_RING_ENTRIES), r, sizeof(r));
			/* the board resets soon after a hang and the UART is too slow: keep a copy in DRAM, which survives */
			hyp_phys_write(snap + 0x100 + 32 * (k % PAS_RING_ENTRIES), r, sizeof(r));
			for (u64 a = snap + 0x100 + 32 * (k % PAS_RING_ENTRIES); a < snap + 0x100 + 32 * (k % PAS_RING_ENTRIES) + 32; a += 64)
				asm volatile("dc civac, %0" :: "r"(a & ~63ULL) : "memory");
			hyp_phys_write(snap, &k, sizeof(k));
			asm volatile("dc civac, %0" :: "r"(snap) : "memory");
			asm volatile("dsb sy" ::: "memory");
		}
		hyp_udelay(200);
	} while (!hyp_ticks_elapsed(t0, ms * 1000));
}

static int __hyp pas_halt(struct pas_sub *s);

/*
 * Take the Hexagon core out of reset, the way the secure world does for it: the NoC and AXI
 * branch clocks of the LPASS, the core's own clocks, the boot address, and the boot FSM.
 *
 * What EL2 cannot do from here: the two NoC clocks that carry the core's traffic to the system cache
 * (LPASS_LPIAON_NOC ...LLC_CLK_ENABLE_LOW at 0x7413020, LPASS_LPICX_NOC ...LLC_LPI_LPASS_CLK_ENABLE_LOW
 * at 0x7434020) are for the secure world only: the firmware's source says "set in icbcfg_lpass_pil_wa()
 * in the TZ PIL.  ADSP has no permissions", and a non-secure write from here is a NoC security error
 * that resets the board (scripts/ADSP.md).
 */
static int __hyp pas_boot(struct pas_sub *s)
{
	u32 status;
	int ret;

	ret = pas_ro_pll_ms ? 0 : pas_ro_pll_on(s);
	if (ret) {
		hprintf("pas: %s: the always-on PLL does not lock (%d)\n", s->name, ret);
		return ret;
	}

	ret = pas_branch_on(s->aon_cc + LPASS_AON_CC_LPI_NOC_HS_CBCR);
	if (!ret)
		ret = pas_branch_on(s->aon_cc + LPASS_AON_CC_Q6_AXIM_CBCR);
	if (ret) {
		hprintf("pas: %s: the LPASS NoC and AXI clocks do not come on (%d)\n", s->name, ret);
		return ret;
	}

	/* EVB_SEL 0: the core boots from RST_EVB, not from its eFuse default */
	pas_read(s->evb_sel, &status);
	ret = pas_write(s->evb_sel, 0);
	if (!ret) {
		u32 now = ~0U;

		pas_read(s->evb_sel, &now);
		hprintf("pas: %s: EVB select %#x -> %#x after writing 0\n", s->name, status, now);
	}
	if (!ret)
		ret = pas_write(s->qdsp6ss + QDSP6SS_XO_CBCR, CBCR_ENABLE);
	if (!ret)
		ret = pas_write(s->qdsp6ss + QDSP6SS_SLEEP_CBCR, CBCR_ENABLE);
	if (!ret)
		ret = pas_write(s->qdsp6ss + QDSP6SS_CORE_CBCR, CBCR_ENABLE);
	if (ret) {
		hprintf("pas: %s: the core registers refuse a write (%d)\n", s->name, ret);
		return ret;
	}

	ret = pas_pll_on(s, PAS_Q6_PLL_HZ);
	if (ret) {
		hprintf("pas: %s: the core PLL does not lock (%d)\n", s->name, ret);
		return ret;
	}

	for (int i = 0; s->dtb_id && i < 6; i++)
		pas_write(s->qdsp6ss + QDSP6SS_BOOT_PARAMS(i), s->boot_params[i]);

	if (s->tcsr) {
		u32 was = ~0U, now = ~0U;

		/* The Trace32 loader sets this for every Hexagon it boots: "this allows the Q6 access to
		 * its TCM"; on a PIL boot it is the loader's (the application processor's) job */
		pas_read(s->tcsr + TCSR_BOOT_IMEM_DISABLE, &was);
		pas_write(s->tcsr + TCSR_BOOT_IMEM_DISABLE, 1);
		pas_read(s->tcsr + TCSR_BOOT_IMEM_DISABLE, &now);
		hprintf("pas: %s: TCSR_BOOT_IMEM_DISABLE %#x -> %#x\n", s->name, was, now);
	}

	if (pas_smoke) {
		/* The signature address is baked into the program: the last page of the carveout this
		 * board's DT gives the ADSP (0xa1f7f000), so only run it where that holds */
		static const u32 __hyp_rodata prog[] = {
			0x0a1f5fc0, 0x7800c001, 0x04854d4d, 0x7800c022, 0xa181c200, 0x06f66dbd,
			0x7800c562, 0xa181c201, 0xa181c202, 0x5800c000,
		};
		u32 zero[3] = { 0, 0, 0 };

		if (s->mem + s->size != 0xa1f80000ULL) {
			hprintf("pas: %s: the smoke test needs a carveout ending at 0xa1f80000\n", s->name);
			return -EINVAL;
		}
		hyp_phys_write(0xa1f7f000, zero, sizeof(zero));
		hyp_phys_write(s->mem, prog, sizeof(prog));
		asm volatile("dsb sy" ::: "memory");
		s->entry = s->mem;
		hprintf("pas: %s: SMOKE TEST: booting a program at %#llx instead of the firmware\n", s->name,
			s->entry);
	}

	if (pas_ctx_ms) {
		ret = hyp_smmu_ctx_enable(s->mem, s->size);
		if (ret) {
			hprintf("pas: %s: no translation context (%d)\n", s->name, ret);
			pas_ctx_ms = 0;
		}
	}

	ret = pas_write(s->qdsp6ss + QDSP6SS_RST_EVB, (u32)(s->entry >> 4));
	if (!ret) {
		u32 now = ~0U;

		pas_read(s->qdsp6ss + QDSP6SS_RST_EVB, &now);
		hprintf("pas: %s: RST_EVB reads %#x for %#x\n", s->name, now, (u32)(s->entry >> 4));
	}
	if (!ret)
		ret = pas_write(s->qdsp6ss + QDSP6SS_BOOT_CORE_START, 1);
	hyp_udelay(1000);
	if (!ret)
		ret = pas_write(s->qdsp6ss + QDSP6SS_BOOT_CMD, 1);
	if (!ret)
		ret = pas_wait(s->qdsp6ss + QDSP6SS_BOOT_STATUS, BIT(0), BIT(0));
	if (ret) {
		pas_read(s->qdsp6ss + QDSP6SS_BOOT_STATUS, &status);
		hprintf("pas: %s did not boot (%d), BOOT_STATUS %#x\n", s->name, ret, status);
		return ret;
	}

	pas_read(s->qdsp6ss + QDSP6SS_BOOT_STATUS, &status);
	hprintf("pas: %s booted at %#llx, BOOT_STATUS %#x\n", s->name, s->entry, status);

	if (pas_ctx_ms) {
		hyp_smmu_ctx_service(pas_ctx_ms);
		hyp_smmu_ctx_disable();
		pas_ctx_ms = 0;
	}
	if (pas_ro_pll_ms && pas_ro_pll_ms != PAS_RO_PLL_NEVER) {
		hyp_udelay(pas_ro_pll_ms * 1000);
		hprintf("pas: %s: always-on PLL %d, %u ms after the boot\n", s->name, pas_ro_pll_on(s),
			pas_ro_pll_ms);
	}

	if (pas_smoke) {
		u32 sig = 0;
		u64 t0 = hyp_ticks();

		do {
			hyp_phys_read(0xa1f7f000, &sig, sizeof(sig));
		} while (sig != SMOKE_SIG && !hyp_ticks_elapsed(t0, 100000));
		hprintf("pas: %s: SMOKE TEST %s (read %#x)\n", s->name, sig == SMOKE_SIG ? "PASSED: the core runs" :
			"FAILED: nothing came back from the core", sig);
		pas_smoke = false;
	}

	if (pas_ring_ms) {
		u32 ms = pas_ring_ms;

		pas_ring_ms = 0;
		pas_ring_watch(ms);
	}
	if (pas_poll_ms) {
		u32 ms = pas_poll_ms;

		pas_poll_ms = 0;
		pas_poll(s, ms);
	}
	if (pas_watch_ms) {
		u32 ms = pas_watch_ms;

		pas_watch_ms = 0;
		hyp_smmu_watch(ms, 0x1000, 0x10ff);	/* the ADSP's streams */
	}
	if (pas_run_ms) {
		u32 ms = pas_run_ms;

		pas_run_ms = 0;
		hyp_udelay(ms * 1000);
		if (pas_nmi_ms) {
			/* The firmware's crash handler writes its state down and flushes its caches.  This goes
			 * first: every other access to the LPASS register space after ~120 ms resets the board
			 * (CNOC_MAIN time-out on qhs_lpass_cfg), the RCG read below included.  The register space
			 * answers in only some of the firmware's low power phases, GCC's vote register
			 * (always readable) shows which. */
			if (pas_nmi_phase != 2) {
				u64 t0 = hyp_ticks(), us0 = hyp_ticks_us();
				u32 v = 0, run = 0;

				do {
					pas_read(GCC_LPASS_DSP_GDSC_VOTE, &v);
					run = (v >> 31) == pas_nmi_phase ? run + 1 : 0;
				} while (run < 3 && !hyp_ticks_elapsed(t0, 300000));
				hprintf("pas: %s: GCC vote %#x, %s after %llu us\n", s->name, v, run >= 3 ? "phase reached" :
					"phase never reached", hyp_ticks_us() - us0);
			}
			pas_write(s->qdsp6ss + QDSP6SS_NMI, 1);
			hprintf("pas: %s: NMI sent %u ms after the boot, waiting %u ms\n", s->name, ms, pas_nmi_ms);
			hyp_udelay(pas_nmi_ms * 1000);
			pas_nmi_ms = 0;
		} else if (ms <= 100) {
			u32 cmd = ~0U, cfg = ~0U;

			/* the firmware's clock driver gets stuck on this one without the always-on PLL */
			pas_read(s->aon_cc + 0x22054, &cmd);
			pas_read(s->aon_cc + 0x22058, &cfg);
			hprintf("pas: %s: AON_CAM_AHB RCG cmd %#x cfg %#x\n", s->name, cmd, cfg);
		}
		if (pas_leave) {
			hprintf("pas: %s: left running %u ms after the boot\n", s->name, ms);
		} else {
			pas_halt(s);
			hprintf("pas: %s: stopped %u ms after the boot\n", s->name, ms);
		}
	}

	return 0;
}

/* Halt the core's bus masters, then hold the core in reset */
static int __hyp pas_halt(struct pas_sub *s)
{
	int ret;


	ret = pas_write(s->qdsp6ss + QDSP6SS_INTF_HALTREQ, 7);
	if (!ret) {
		u32 ack = 0;
		u64 t0 = hyp_ticks();

		/* Any acknowledge ends the wait, the way the Valve loader does it */
		do {
			pas_read(s->qdsp6ss + QDSP6SS_INTF_HALTACK, &ack);
		} while (!ack && !hyp_ticks_elapsed(t0, PAS_WAIT_US));
		if (!ack)
			hprintf("pas: %s: the bus masters do not acknowledge the halt\n", s->name);
	}
	if (!ret)
		ret = pas_write(s->qdsp6ss + QDSP6SS_INTF_HALTREQ, 0);
	if (!ret)
		ret = pas_write(s->qdsp6ss + QDSP6SS_RESET_CFG, ~0U);
	if (!ret)
		ret = pas_write(s->qdsp6ss + QDSP6SS_RESET_CMD, 1);
	hyp_udelay(10000);
	s->core_up = false;
	pas_write(s->qdsp6ss + QDSP6SS_RESET_CFG, 0);

	return ret;
}

/*
 * The entry point of the ELF image whose headers (the PAS metadata) sit at @pa, and - for a
 * 32-bit image - its program headers, which pas_scan() uses to find where the core may have written
 */
static int __hyp pas_entry(struct pas_sub *s, u64 pa, u64 *entry)
{
	u8 hdr[0x34];
	u32 phoff, phnum, phentsize;

	s->nphdr = 0;
	if (hyp_phys_read(pa, hdr, sizeof(hdr)))
		return -EFAULT;
	if (memcmp(hdr, "\177ELF", 4))
		return -EINVAL;

	if (hdr[4] == 2) {		/* ELFCLASS64 */
		*entry = get_unaligned_le64(hdr + 0x18);
		return 0;
	}
	if (hdr[4] != 1)
		return -EINVAL;

	*entry = get_unaligned_le32(hdr + 0x18);
	phoff = get_unaligned_le32(hdr + 0x1c);
	phentsize = get_unaligned_le16(hdr + 0x2a);
	phnum = min_t(u32, get_unaligned_le16(hdr + 0x2c), PAS_MAX_PHDR);
	if (phentsize == sizeof(struct pas_phdr) && !hyp_phys_read(pa + phoff, s->phdr, phnum * phentsize))
		s->nphdr = phnum;

	return 0;
}

/*
 * Where did the core write?  The OS zero-fills the part of every segment that the file does not
 * fill (BSS, heap, stacks), so a page of it that is not zero now was written by the core.  A core that
 * runs leaves its footprint there; one that never executes leaves none.  Logged when the processor
 * is shut down, and on the console (`pas scan`).
 */
static void __hyp pas_scan(struct pas_sub *s)
{
	u64 total = 0, dirty = 0;
	int shown = 0;

	if (!s->nphdr) {
		hprintf("pas: %s: no program headers to scan\n", s->name);
		return;
	}

	for (u32 i = 0; i < s->nphdr; i++) {
		struct pas_phdr *p = &s->phdr[i];
		u64 first = 0, last = 0, n = 0, run = 0, runlen = 0;
		u64 start, end;

		if (p->type != PT_LOAD || !(p->flags & PF_W) || p->memsz <= p->filesz)
			continue;

		/* the zero-filled tail, from the page after the last byte the file fills */
		start = ((u64)p->paddr + p->filesz + 0xfff) & ~0xfffULL;
		end = ((u64)p->paddr + p->memsz) & ~0xfffULL;

		for (u64 pa = start; pa < end; pa += 0x1000) {
			u64 buf[32];	/* the EL2 stack is 8 KiB: read a page 256 bytes at a time */
			int nz = 0;

			/* what the core wrote bypassed this core's caches: drop whatever EL2 holds */
			for (u64 a = pa; a < pa + 0x1000; a += 64)
				asm volatile("dc ivac, %0" :: "r"(a) : "memory");
			asm volatile("dsb sy" ::: "memory");
			for (u64 off = 0; off < 0x1000 && !nz; off += sizeof(buf)) {
				if (hyp_phys_read(pa + off, buf, sizeof(buf)))
					break;
				for (int w = 0; w < 32 && !nz; w++)
					nz = buf[w] != 0;
			}
			total++;
			if (nz) {
				if (!n)
					first = pa;
				last = pa;
				n++;
				if (runlen && run + runlen == pa) {
					runlen += 0x1000;
				} else {
					if (runlen)
						hprintf("pas: dirty %#llx +%#llx\n", run, runlen);
					run = pa;
					runlen = 0x1000;
				}
			}
		}
		if (runlen)
			hprintf("pas: dirty %#llx +%#llx\n", run, runlen);
		dirty += n;
		if (n && shown++ < 8)
			hprintf("pas: %s: segment %u (%#x..%#x): %llu non-zero page(s), %#llx .. %#llx\n",
				s->name, i, p->paddr, p->paddr + p->memsz, n, first, last);
	}

	hprintf("pas: %s: the core wrote %llu of %llu zero-filled pages\n", s->name, dirty, total);
}

/* Run the scan for the first processor (the console's `pas scan`) */
void __hyp hyp_pas_scan(void)
{
	for (int i = 0; i < PAS_MAX_SUBSYS; i++)
		if (subsys[i].valid)
			pas_scan(&subsys[i]);
}

/* Stop the next started processor after @ms milliseconds (`pas run MS`, 0: let it run) */
void __hyp hyp_pas_set_run_ms(u32 ms, u32 nmi_ms, u32 nmi_phase)
{
	pas_run_ms = ms;
	pas_nmi_ms = nmi_ms;
	pas_nmi_phase = nmi_phase;
}

/* Make the next shutdown leave the core running (`pas leave`); returns the new state */
bool __hyp hyp_pas_toggle_leave(void)
{
	pas_leave = !pas_leave;

	return pas_leave;
}

/* Let the secure world have the PAS calls (or take them back); returns the new state */
bool __hyp hyp_pas_toggle_tz(void)
{
	pas_passthrough = !pas_passthrough;

	return pas_passthrough;
}

/* Is a processor EL2 started running?  Its LPASS register space answers only some of the time then
 * (scripts/ADSP.md), and the board resets when an access finds it asleep */
bool __hyp hyp_pas_core_up(void)
{
	for (int i = 0; i < PAS_MAX_SUBSYS; i++)
		if (subsys[i].valid && subsys[i].core_up)
			return true;

	return false;
}

/* Arm the smoke test for the next AUTH_AND_RESET; returns the new state */
bool __hyp hyp_pas_toggle_smoke(void)
{
	pas_smoke = !pas_smoke;

	return pas_smoke;
}

/* Send the next started processor through an EL2-owned SMMU context for @ms milliseconds (`pas ctx MS`) */
void __hyp hyp_pas_set_ctx_ms(u32 ms)
{
	pas_ctx_ms = ms;
}

/*
 * When the next processor start brings up the always-on PLL (`pas ropll`): 0 before the boot, N
 * milliseconds after it, PAS_RO_PLL_NEVER not at all.  The state is kept.
 */
void __hyp hyp_pas_set_ro_pll(u32 ms)
{
	pas_ro_pll_ms = ms;
}

/**
 * hyp_pas_describe() - one line per processor EL2 loads, for the console
 * @buf: filled
 * @len: its size
 */
void __hyp hyp_pas_describe(char *buf, size_t len)
{
	size_t n = 0;

	buf[0] = '\0';
	for (int i = 0; i < PAS_MAX_SUBSYS && n < len; i++) {
		struct pas_sub *s = &subsys[i];
		u32 status = 0;

		if (!s->valid)
			continue;
		if (s->running)
			pas_read(s->qdsp6ss + QDSP6SS_BOOT_STATUS, &status);
		n += snprintf(buf + n, len - n, "pas %s: id %u %s, entry %#llx, carveout %#llx + %#llx, BOOT_STATUS %#x\r\n",
			      s->name, s->id, s->running ? "running" : "stopped", s->entry, s->mem, s->size, status);
		n += snprintf(buf + n, len - n, "pas %s: BOOT_PARAMS %#x %#x %#x %#x %#x %#x\r\n", s->name,
			      s->boot_params[0], s->boot_params[1], s->boot_params[2], s->boot_params[3],
			      s->boot_params[4], s->boot_params[5]);
	}
}

/*
 * MEM_PROT_ASSIGN: the OS lends a region to the processor's VM (and takes it back).  Behind
 * TrustZone the hypervisor edits the stage-2 tables for it; EL2 keeps every region mapped for
 * the OS and the processors' streams are bypass, so there is nothing to change and the call
 * only has to succeed.  The first argument points at the region (address, size), which is
 * logged: it says which memory each processor is going to use.
 */
static void __hyp pas_mem_assign(struct pt_regs *regs)
{
	u64 region[2] = { 0, 0 };

	hyp_phys_read(regs->regs[SCM_ARG_BASE], region, sizeof(region));
	hprintf("pas: SCM MEM_PROT_ASSIGN: %#llx + %#llx\n", region[0], region[1]);

	regs->regs[0] = 0;
	regs->regs[1] = 0;
	regs->regs[2] = 0;
	regs->regs[3] = 0;
}

/**
 * hyp_pas_smc() - answer a guest's SCM call for a processor EL2 owns
 * @regs: the guest's frame, the SMC being its x0..x7
 *
 * Return: false when the call is not one of ours and goes on to the secure world
 */
bool __hyp hyp_pas_smc(struct pt_regs *regs)
{
	u64 fid = regs->regs[0];
	u32 cmd = fid & 0xff, svc = (fid >> 8) & 0xff, owner = (fid >> 24) & 0x3f;
	u32 id = (u32)regs->regs[SCM_ARG_BASE];
	u64 arg1 = regs->regs[SCM_ARG_BASE + 1], arg2 = regs->regs[SCM_ARG_BASE + 2];
	unsigned long flags;
	struct pas_sub *s;
	long res = 0;

	if (owner == SCM_OWNER_SIP && svc == SCM_SVC_MP && cmd == MP_ASSIGN && !pas_passthrough) {
		pas_mem_assign(regs);
		return true;
	}

	if (owner != SCM_OWNER_SIP || svc != SCM_SVC_PIL || pas_passthrough)
		return false;

	/* PAS_SET_REMOTE_STATE takes its state first and the id second */
	if (cmd == PAS_SET_REMOTE_STATE)
		id = (u32)regs->regs[SCM_ARG_BASE + 1];

	s = pas_find(id);
	if (!s)
		return false;

	hprintf("pas: %s: SCM PIL call %#x, id %u, args %#llx %#llx\n", s->name, cmd, id, arg1, arg2);
	spin_lock_irqsave(&pas_lock, flags);

	switch (cmd) {
	case PAS_IS_SUPPORTED:
		res = 1;
		break;
	case PAS_SET_REMOTE_STATE:
		break;
	case PAS_INIT_IMAGE:
		if (id == s->id) {
			res = pas_entry(s, arg1, &s->entry);
			hprintf("pas: %s: INIT_IMAGE: entry %#llx (%ld)\n", s->name, s->entry, res);
		}
		break;
	case PAS_MEM_SETUP:
		if (id == s->id) {
			s->mem = arg1;
			s->size = arg2;
		}
		break;
	case PAS_AUTH_AND_RESET:
		if (id != s->id)
			break;
		if (!s->entry)
			s->entry = s->mem;
		res = pas_boot(s);
		s->running = !res;
		s->core_up = !res;
		break;
	case PAS_SHUTDOWN:
		if (id != s->id)
			break;
		if (pas_leave) {
			hprintf("pas: %s: SHUTDOWN: the core is left running, no register is touched\n", s->name);
			pas_scan(s);
			res = 0;
			s->running = false;
			s->entry = 0;
			break;
		}
		res = pas_halt(s);
		hprintf("pas: %s: SHUTDOWN (%ld)\n", s->name, res);
		pas_scan(s);
		s->running = false;
		s->entry = 0;
		break;
	case PAS_GET_RSCTABLE:
		/*
		 * The OS builds the processor's SMMU context from this table (its devmem entries become
		 * iommu_map() calls, and the OS's SMMU driver writes the translation table base): the
		 * table is the DT's `hypervisor-pas-rsctable`, made by scripts/mk-rsctable.py.  The call
		 * is (pas id, input table, its size, [output buffer, its size]), the last two through an
		 * array x5 points at.  Without a table the call is not supported, as before.
		 */
		if (id == s->id && s->rsc_len) {
			u64 ext[2] = { 0, 0 };

			hyp_phys_read(regs->regs[SCM_ARG_BASE + 3], ext, sizeof(ext));
			hprintf("pas: %s: GET_RSCTABLE: %u bytes for a buffer of %llu\n", s->name, s->rsc_len, ext[1]);
			if (ext[1] < s->rsc_len) {
				regs->regs[0] = (u64)-1;
				regs->regs[1] = 0;
				regs->regs[2] = PAS_RSCTABLE_BUFFER_NOT_SUFFICIENT;
				regs->regs[3] = s->rsc_len;
			} else {
				hyp_phys_write(ext[0], s->rsc, s->rsc_len);
				regs->regs[0] = 0;
				regs->regs[1] = 0;
				regs->regs[2] = 0;
				regs->regs[3] = s->rsc_len;
			}
			spin_unlock_irqrestore(&pas_lock, flags);
			return true;
		}
		/* fall through */
	default:
		/* GET_RSCTABLE and the rest: the OS asks only for the ones the firmware it was
		 * given needs, and EL2 has no table to offer */
		hprintf("pas: %s: SCM PIL call %#x is not supported\n", s->name, cmd);
		regs->regs[0] = (u64)-1;
		spin_unlock_irqrestore(&pas_lock, flags);
		return true;
	}

	spin_unlock_irqrestore(&pas_lock, flags);

	/* The call itself went through (x0 = 0); its own status is the first result */
	regs->regs[0] = 0;
	regs->regs[1] = (u64)res;
	regs->regs[2] = 0;
	regs->regs[3] = 0;

	return true;
}

#if IS_ENABLED(CONFIG_HYP_TEST)
/* Self-test hooks (hyp/test.c): a made-up processor whose registers are never touched */
void __hyp hyp_pas_test_inject(u32 id, u32 dtb_id)
{
	struct pas_sub *s = &subsys[PAS_MAX_SUBSYS - 1];

	memset(s, 0, sizeof(*s));
	s->valid = true;
	s->id = id;
	s->dtb_id = dtb_id;
	strlcpy(s->name, "selftest", sizeof(s->name));
}

void __hyp hyp_pas_test_clear(void)
{
	memset(&subsys[PAS_MAX_SUBSYS - 1], 0, sizeof(subsys[0]));
}

u64 __hyp hyp_pas_test_entry(u32 id)
{
	struct pas_sub *s = pas_find(id);

	return s ? s->entry : 0;
}
#endif

/* After the next processor start, print its SMMU streams and every fault for @ms milliseconds (`pas watch MS`) */
void __hyp hyp_pas_set_watch_ms(u32 ms)
{
	pas_watch_ms = ms;
}

/*
 * `pas stress-ropll`: what the stock firmware does to the always-on PLL, back to back as its core does
 * it (SetRegSettings: the nine CONFIG/TEST stores, then USER_CTL and USER_CTL_U), with the PLL running.  Every
 * store after the first restarts the PLL's calibration (MODE bit 9); if the board resets, the register
 * interface does not take stores back to back while it calibrates.
 */
void __hyp hyp_pas_ropll_stress(void)
{
	static const u32 regs[] = {
		0x60000F6A, 0x0001C808, 0, 0x04008174, 0, 0x0080C496, 0x40100180, 0x441001BC, 0x000003D8,
	};

	for (int i = 0; i < PAS_MAX_SUBSYS; i++) {
		struct pas_sub *s = &subsys[i];
		u64 pll;
		u32 mode = 0;
		int ret;

		if (!s->valid || !s->aon_cc)
			continue;
		pll = s->aon_cc + RO_PLL;
		ret = pas_ro_pll_on(s);
		pas_read(pll + PLL_MODE, &mode);
		hprintf("pas: stress-ropll: the PLL is up (%d), mode %#x\n", ret, mode);
		for (int k = 0; k < ARRAY_SIZE(regs); k++)
			pas_write(pll + RO_PLL_CONFIG_CTL + 4 * k, regs[k]);
		pas_write(pll + RO_PLL_USER_CTL, 0);
		pas_write(pll + RO_PLL_USER_CTL_U, 0x00E50302);
		pas_read(pll + PLL_MODE, &mode);
		hprintf("pas: stress-ropll: survived, mode %#x\n", mode);
		return;
	}
	hprintf("pas: stress-ropll: no processor set up\n");
}

/* After the next processor start, watch the entry tracer's ring at @pa for @ms milliseconds (`pas ring PA MS`) */
void __hyp hyp_pas_set_ring(u64 pa, u32 ms)
{
	pas_ring_pa = pa;
	pas_ring_ms = ms;
}

/* After the next processor start, sample the core's status for @ms milliseconds (`pas poll MS`) */
void __hyp hyp_pas_set_poll_ms(u32 ms)
{
	pas_poll_ms = ms;
}
