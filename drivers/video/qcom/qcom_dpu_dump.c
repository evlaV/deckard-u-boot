
#include <asm/io.h>
#include <clk.h>
#include <dm/device.h>
#include <dm/device_compat.h>
#include <dm/device-internal.h>
#include <dm/read.h>
#include <dm/ofnode_graph.h>
#include <video.h>
#include <video_bridge.h>
#include <linux/sizes.h>
#include <linux/delay.h>
#include <video_console.h>
#include <lmb.h>
#include <drm/display/drm_dsc_helper.h>

#include "dpu_hw_mdss.h"
#include "dpu_hwio.h"
#include "catalog/dpu_hw_catalog.h"
#include "dpu_hw.h"

bool regdump_decode(u64 addr, u32 val);
bool regdump_read_decode(u64 addr);

// bool regdump_decode(u64 addr, u32 val) {
// 	printf("%#llx: %#x\n", addr, val);
// 	return true;
// }

// bool regdump_read_decode(u64 addr) {
// 	return regdump_decode(addr, readl(addr));
// }

static void qcom_dpu_dump_reg(ulong addr)
{
	u32 val = readl(addr);
	if (val) {
		printf("%#lx: ",addr);
		regdump_decode(addr, val);
	}
	
	// if (!regdump_read_decode(addr)) {
	// 	printf("\n");
	// }
		// printf("%#010x\n", readl(bbase + j));
}

static void qcom_dpu_dump_sspp(struct dpu_hw_sspp *sspp)
{
	ulong bbase = (ulong)sspp->hw.blk_addr;
	printf("====================SSPP_%s================\n", dpu_sspp_str(sspp->idx));

	for (int j = 0; j < sspp->cap->len; j+= 4) {
		qcom_dpu_dump_reg(bbase + j);
	}

	printf("\n");
}

static void qcom_dpu_dump_ctl(struct dpu_hw_ctl *ctl)
{
	ulong bbase = (ulong)ctl->hw.blk_addr;
	printf("====================CTL_%d================\n", ctl->idx - CTL_0);

	for (int j = 0; j < ctl->cap->len; j+= 4) {
		qcom_dpu_dump_reg(bbase + j);
	}

	printf("\n");
}

/* Dump the encoder state, for non-dsc_merge the ctl and sspp are shared so don't dump them twice */
static void qcom_dpu_dump_encoder_state(struct dpu_phys_encoder *enc, bool shared_sspp)
{
	printf("\n\n=== %s Encoder ===\n", split_role_str(enc->split_role));

	if (!shared_sspp)
		qcom_dpu_dump_sspp(enc->sspp);


	for (int i = 0; i < enc->num_mixer; i++) {
		struct dpu_hw_mixer *lm = &enc->mixer[i];
		ulong bbase = (ulong)lm->hw.blk_addr;
		printf("====================LM_%d================\n", lm->idx - LM_0);

		for (int j = 0; j < lm->cap->len; j+= 4) {
			qcom_dpu_dump_reg(bbase + j);
		}

		printf("\n");
	}

	for (int i = 0; i < enc->num_dsc; i++) {
		struct dpu_hw_dsc *dsc = &enc->dsc[i];
		ulong bbase = (ulong)dsc->hw.blk_addr;
		printf("====================DSC_%d================\n", dsc->idx - DSC_0);

		for (int j = 0; j < dsc->cap->len; j+= 4) {
			qcom_dpu_dump_reg(bbase + j);
		}

		printf("\n  ==== CTL ====\n");

		ulong sbbase = bbase + dsc->cap->sblk->ctl.base;
		for (int j = 0; j < dsc->cap->sblk->ctl.len; j+= 4) {
			qcom_dpu_dump_reg(sbbase + j);
		}

		printf("\n  ==== ENC ====\n");

		sbbase = bbase + dsc->cap->sblk->enc.base;
		for (int j = 0; j < dsc->cap->sblk->enc.len; j+= 4) {
			qcom_dpu_dump_reg(sbbase + j);

			if (j > 0x3c)
				break;
		}

		printf("\n");
	}

	for (int i = 0; i < enc->num_pingpong; i++) {
		struct dpu_hw_pingpong *pp = &enc->pingpong[i];
		ulong bbase = (ulong)pp->hw.blk_addr;
		printf("====================PINGPONG_%d================\n", pp->idx - PINGPONG_0);

		for (int j = 0; j < pp->cap->len; j+= 4) {
			qcom_dpu_dump_reg(bbase + j);
		}

		printf("\n");
	}

	{
		struct dpu_hw_intf *intf = &enc->intf;
		ulong bbase = (ulong)intf->hw.blk_addr;
		printf("====================INTF_%d================\n", intf->idx - INTF_0);

		for (int j = 0; j < intf->cap->len; j+= 4) {
			qcom_dpu_dump_reg(bbase + j);
		}

		printf("\n");
	}
}

void qcom_dpu_dump_topology_state(struct dpu_topology *topology)
{
	bool shared_sspp = topology->num_sspp < topology->num_encs;

	printf("\n\n=== Dumping DPU state ===\n");

	for (int i = 0; i < topology->num_encs; i++)
		qcom_dpu_dump_encoder_state(&topology->encs[i], shared_sspp);

	qcom_dpu_dump_ctl(&topology->ctl);

	if (!shared_sspp)
		return;

	qcom_dpu_dump_sspp(&topology->sspp[0]);
}
