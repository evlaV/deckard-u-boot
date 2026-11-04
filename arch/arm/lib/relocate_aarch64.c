/* reloc_aarch64.c - position independent x86 ELF shared object relocator
   Copyright (C) 2014 Linaro Ltd. <ard.biesheuvel@linaro.org>
   Copyright (C) 1999 Hewlett-Packard Co.
	Contributed by David Mosberger <davidm@hpl.hp.com>.

    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions
    are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above
      copyright notice, this list of conditions and the following
      disclaimer in the documentation and/or other materials
      provided with the distribution.
    * Neither the name of Hewlett-Packard Co. nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
    CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
    INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
    MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
    DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
    BE LIABLE FOR ANYDIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY,
    OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
    PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
    PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
    THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
    TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
    THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
    SUCH DAMAGE.
*/

#include <asm-generic/sections.h>
#include <asm/system.h>
#include <asm-offsets.h>
#include <elf.h>
#include <linux/linkage.h>
#include <stdio.h>
#include <hypervisor.h>

void relocate_code(u64 relocaddr)
{
	u64 *addr, *target = (u64 *)relocaddr;
	u64 offset, pie_offset, sctlr;
	u64 image_size;

	/* If we're already relocated then we have nothing to do */
	if ((u64)__image_copy_start == relocaddr)
		return;

	/* Calculate relocation offset */
	offset = relocaddr - (u64)CONFIG_TEXT_BASE;
	pie_offset = (u64)__image_copy_start - CONFIG_TEXT_BASE;

	image_size = __image_copy_end - __image_copy_start;

	/* Copy U-Boot code */
	printf("Relocating U-Boot %#llx -> %#llx\n", (u64)__image_copy_start, relocaddr);
	for (addr = (u64 *)__image_copy_start; (u64)addr < (u64)__image_copy_end; addr++)
		*target++ = *addr;

	/* Fix .rela.dyn relocations */
	int skipped = 0;
	u64 hyp_start = (u64)__hyp_start;
	u64 hyp_stop = (u64)__hyp_stop;
	printf("Fixup .rela.dyn! offset=%#llx (pie_offset=%#llx)\n", offset, pie_offset);
	for (const Elf64_Rela *rela = (Elf64_Rela *)__rel_dyn_start; rela < (Elf64_Rela *)__rel_dyn_end; rela++) {
		if (ELF64_R_TYPE(rela->r_info) != R_AARCH64_RELATIVE)
			continue;
		if (rela->r_offset >= (u64)hyp_start && rela->r_offset < (u64)hyp_stop) {
			skipped++;
			continue;
		}
		*(u64 *)(rela->r_offset + offset) = rela->r_addend + offset;
	}
	printf("Skipped %d .hyp relocations\n", skipped);

	/* Flush caches */
	sctlr = get_sctlr();
	if (sctlr & CR_I)
		asm volatile("ic iallu\nisb sy" : : : "memory");
	if (sctlr & CR_C) {
		__asm_flush_dcache_range(relocaddr, relocaddr + image_size);
		__asm_flush_l3_dcache();
		printf("[RELA] Flushed dcache!\n");
	}

	/* Jump to hv start without branching */
#if CONFIG_IS_ENABLED(HYPERVISOR)
	// asm volatile(
		// "mov x1, sp\n"
		// "msr sp_el1, x1\n"
		// "dsb sy\n"
		// "msr elr_el2, lr\n");
	hv_start_vm(0x0, offset);
	// asm volatile("eret");
#endif

	// hv_start_vm();
}
