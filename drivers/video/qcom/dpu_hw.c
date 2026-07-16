/* SPDX-License-Identifier: GPL-2.0-only */

/* this file handles dynamic topology configuration
 * based on the display panel requirements.
 */

#define pr_fmt(fmt) "[DPU-HW] "fmt

#include <dm/device-internal.h>
#include <dm/read.h>
#include <linux/sizes.h>
#include <linux/delay.h>
#include <drm/display/drm_dsc_helper.h>

#include "dpu_hw.h"
#include "catalog/dpu_hw_catalog.h"

/*
 * DSC has a max width of 2048, so for dual 2160x2160 panels with DSC,
 * 4 mixers are required even though mixer max width is 2560.
 * this is a "QUADPIPE_DSCMERGE" topology
 *
 * for this configuration, we need to use 2 HW planes per plane because
 * 1) plane max width is 4096, lower than 4320
 * 2) a single plane can only be used by one mixer pair
 * 3) plane can only read 1 pixel/clock, but max DPU clock is 460Mhz
 *
 * 3) is also a problem with DP (2 mixer merge3d configuration),
 * because for example 3840x2160@60 needs more than 460Mhz DPU clock
 * so 2 HW plane mechanism is used with DP too to avoid overclocking
 */

/* These are SM8650 specific? May differ on other platforms */
#define PLANE_MAX_WIDTH 4096
#define DSC_MAX_WIDTH 2048

static inline void dump_request(const struct dpu_resource_request *req, enum dpu_enc_split_role split_role, bool err)
{
	log(LOGC_NONE, err ? LOGL_ERR : LOGL_DEBUG,
	    "Requested resources: role %s, dsc=%s, mode=%ux%u, intf_type %s\n",
	    split_role_str(split_role),
	    req->dsc ? "Yes" : "No", req->width, req->height, intf_type_str(req->intf_type));
}

static inline void dump_encoder_topology(struct dpu_phys_encoder *enc)
{
	struct drm_rect plane;
	printf("DPU: %s encoder: %s "DRM_RECT_FMT"\n", split_role_str(enc->split_role),
	       intf_type_str(enc->intf.cap->type), DRM_RECT_ARG(&enc->plane));
	printf("  [CTL_%d: %#09llx]\n", enc->ctl->idx - CTL_0, (u64)enc->ctl->hw.blk_addr);
	printf("  |\n");
	printf("  +-[SSPP_%s: %#09llx]\n", dpu_sspp_str(enc->sspp->idx), (u64)enc->sspp->hw.blk_addr);
	printf("  |\n");
	for (int i = 0; i < enc->num_mixer; i++)
		printf("%s[LM_%d : %#09llx]", i == 0 ? "  +-" : "---", enc->mixer[i].idx - LM_0, (u64)enc->mixer[i].hw.blk_addr);
	printf("\n  +");
	for (int i = 0; i < enc->num_mixer; i++) {
		struct dpu_hw_mixer *lm = &enc->mixer[i];
		memcpy(&plane, &enc->plane, sizeof(plane));
		if (enc->num_mixer > 1)
			plane.x1 += lm->cfg.right_mixer * lm->cfg.out_width;
		plane.x2 = plane.x1 + lm->cfg.out_width;
		printf("%s("DRM_RECT_FMT")", i == 0 ? "-" : "---", DRM_RECT_ARG(&plane));
	}
	printf("\n");

	for (int i = 0; i < enc->num_dsc; i++)
		printf("%s[DSC_%d: %#09llx]", i == 0 ? "  +-" : "---", enc->dsc[i].idx - DSC_0, (u64)enc->dsc[i].hw.blk_addr);
	printf("%s  |\n", enc->num_dsc ? "\n" : "");

	for (int i = 0; i < enc->num_pingpong; i++)
		printf("%s[PP_%d : %#09llx]", i == 0 ? "  +-" : "---", enc->pingpong[i].idx - PINGPONG_0, (u64)enc->pingpong[i].hw.blk_addr);
	printf("\n  \\\n");
	printf("   ---[INTF_%d: %#09llx]\n\n", enc->intf.idx - INTF_0, (u64)enc->intf.hw.blk_addr);
}

static const struct dpu_sspp_cfg *find_sspp_cfg(const struct dpu_mdss_cfg *cat, enum dpu_sspp sspp_id)
{
	const struct dpu_sspp_cfg *sspp_cfg;
	int i = 0;

	/* Find the SSPP */
	for (sspp_cfg = &cat->sspp[0]; i < cat->sspp_count; sspp_cfg++, i++)
		if (sspp_cfg->id == sspp_id)
			return sspp_cfg;

	return NULL;
}

/*
 * This is a particularly basic DPU topology allocator, it comes with a few assumptions:
 *
 * - No merge_3d -> Only DSC merge supported, non-DSC panels can't be wider than 4096
 * - Multiple interfaces are only supported for dual DSI
 * - Only one SSPP is used unless doing quad pipe with DSC merge. Wide dual-dsi panels
 *   that don't use DSC aren't supported (unlikely to be an issue in practise?)
 *
 * FIXME: use the opaque struct dpu_hw_blk to de-dup a bunch of the logic here!
 */
static int dpu_hw_intf_init_resources(struct dpu_phys_encoder *rm,
				      const struct dpu_resource_request *req)
{
	const struct dpu_mdss_cfg *cat = req->cat;
	u32 width, height;
	/* Quad pipe: 4:4:2 topology */
	bool dsc_merge = false;

	dump_request(req, rm->split_role, false);

	width = req->width;
	height = req->height;

	if (rm->split_role != ENC_ROLE_SOLO)
		width /= 2;
	if (req->dsc && width > DSC_MAX_WIDTH)
		dsc_merge = true;

	rm->plane.y1 = 0;
	rm->plane.y2 = height;
	rm->plane.x1 = 0;
	rm->plane.x2 = width;
	/* Shift the plane for the secondary intf */
	if (rm->split_role == ENC_ROLE_PRIMARY) {
		rm->plane.x1 += width;
		rm->plane.x2 += width;
	}

	/* Pick the intf */
	{
		const struct dpu_intf_cfg *intf_cfg;
		bool skipped_primary = false;
		int i = 0;

		for (intf_cfg = &cat->intf[0]; i < cat->intf_count; i++, intf_cfg++) {
			if (intf_cfg->type != req->intf_type)
				continue;
			if (enc_is_primary(rm) || skipped_primary) {
				rm->intf.cap = intf_cfg;
				break;
			} else {
				/* Skip the first matching intf to get the second one. */
				skipped_primary = true;
			}
		}
		if (!rm->intf.cap) {
			pr_err("Couldn't find suitable INTF!\n");
			dump_request(req, rm->split_role, true);
			return -ENOENT;
		}

		rm->intf.hw.blk_addr = req->dpu_base + intf_cfg->base;
		rm->intf.mdss_ver = cat->mdss_ver;
		rm->intf.idx = intf_cfg->id;
		pr_debug("Using %s INTF_%d\n", intf_type_str(intf_cfg->type), intf_cfg->id - INTF_0);
	}

	/* Pick the DSC block(s) */
	if (req->dsc) {
		const struct dpu_dsc_cfg *dsc_cfg;
		int i = 0, dsc = 0;

		rm->num_dsc = 1;
		/* QUAD PIPE with DSC merge */
		if (dsc_merge)
			rm->num_dsc = 2;

		if (!enc_is_primary(rm))
			i += rm->num_dsc;

		for (dsc_cfg = &cat->dsc[i]; i < cat->dsc_count && dsc < rm->num_dsc; i++, dsc++, dsc_cfg++) {
			rm->dsc[dsc].idx = dsc_cfg->id;
			rm->dsc[dsc].cap = dsc_cfg;
			rm->dsc[dsc].hw.blk_addr = req->dpu_base + dsc_cfg->base;
		}

		/* Validation */
		for (dsc = 0; dsc < rm->num_dsc; dsc++) {
			if (rm->dsc[dsc].idx > DSC_NONE) {
				pr_debug("Using DSC_%d (%s)\n", rm->dsc[dsc].idx - DSC_NONE, rm->dsc[dsc].cap->name);
				continue;
			}

			int start = enc_is_primary(rm) ? 0 : 2;
			pr_err("Didn't get DSC_%d! num_dsc=%d start=DSC_%d\n", start + dsc, rm->num_dsc,
			       start);
			dump_request(req, rm->split_role, true);
			return -ENOENT;
		}
	} else if (width > PLANE_MAX_WIDTH) {
		pr_err("Panels wider than %u are unsupported unless using DSC merge!\n", PLANE_MAX_WIDTH);
		dump_request(req, rm->split_role, true);
		return -EINVAL;
	}

	/*
	 * Pick the Pingpong block(s).
	 *
	 * If using DSC then support DSC merge with two PP blocks
	 * otherwise we only use one PP per INTF. We don't support
	 * 3d merge.
	 */
	if (req->dsc)
		rm->num_pingpong = rm->num_dsc;
	else
		rm->num_pingpong = 1;
	
	{
		const struct dpu_pingpong_cfg *pp_cfg;
		int i = 0, pp = 0;

		if (!enc_is_primary(rm))
			i += rm->num_pingpong;

		for (pp_cfg = &cat->pingpong[i]; i < cat->pingpong_count && pp < rm->num_pingpong; i++, pp++, pp_cfg++) {
			rm->pingpong[pp].idx = pp_cfg->id;
			rm->pingpong[pp].cap = pp_cfg;
			rm->pingpong[pp].hw.blk_addr = req->dpu_base + pp_cfg->base;
		}

		/* Validation */
		for (pp = 0; pp < rm->num_pingpong; pp++) {
			if (rm->pingpong[pp].idx > PINGPONG_NONE) {
				pr_debug("Using PINGPONG_%d!\n", rm->pingpong[pp].idx - PINGPONG_0);
				continue;
			}

			int start = enc_is_primary(rm) ? 0 : 2;
			pr_err("Didn't get PINGPONG_%d! num_pp=%d start=PINGPONG_%d\n", start + pp, rm->num_pingpong,
			       start);
			dump_request(req, rm->split_role, true);
			return -ENOENT;
		}
	}

	/* Pick the CTL index, CTL_0 for primary, CTL_1 for secondary. */
	enum dpu_ctl ctl_id = CTL_0;
	const struct dpu_ctl_cfg *ctl_cfg;

	/*
	 * Determine the CTL configuration
	 *
	 * We only support the newer "active" CTLs
	 * In the dual-DSI case we use a single CTL for both INTFs
	 * but for Quad pipe we need a CTL per INTF.
	 * So skip configuring the CTL if it's shared with the primary intf
	 */
	// if (dsc_merge && !enc_is_primary(rm))
	// 	ctl_id += 1;

	ctl_cfg = &cat->ctl[ctl_id - CTL_0];

	/*
	 * Only configure the CTL for the primary encoder or in the
	 * quad pipe case where we use a CTL per INTF.
	 */
	if (enc_is_primary(rm)) {
		rm->ctl->hw.blk_addr = req->dpu_base + ctl_cfg->base;
		rm->ctl->idx = ctl_id;
		rm->ctl->cap = ctl_cfg;
		rm->ctl->mixer_count = cat->mixer_count;
		rm->ctl->mixer_hw_caps = cat->mixer;
		rm->ctl->mdss_ver = cat->mdss_ver;
	}

	/* Pick the mixer(s) - usually 1 but 2 for quad pipe with DSC merge */
	rm->num_mixer = rm->num_pingpong;
	{
		const struct dpu_lm_cfg *lm_cfg;
		int i = 0, lm = 0;

		if (!enc_is_primary(rm))
			i += rm->num_mixer;

		for (; lm < rm->num_mixer; i++, lm++) {
			lm_cfg = &cat->mixer[i];
			/* If we're using two mixer ensure they are a matching pair! */
			if (lm == 1 && lm_cfg->lm_pair != rm->mixer[0].idx) {
				pr_err("Couldn't find mixer pair for LM_%d, catalog error?\n", i);
				dump_request(req, rm->split_role, true);
				return -EINVAL;
			}
			rm->mixer[lm].hw.blk_addr = req->dpu_base + lm_cfg->base;
			rm->mixer[lm].mdp = cat->mdp;
			/* If we have 2 mixers they both use the same CTL */
			rm->mixer[lm].ctl = ctl_cfg;
			rm->mixer[lm].idx = lm_cfg->id;
			rm->mixer[lm].cap = lm_cfg;

			/* Mixer CFG, half width for dsc_merge */
			rm->mixer[lm].cfg = (struct dpu_hw_mixer_cfg) {
				.out_width = width / rm->num_mixer,
				.out_height = height,
				/* Odd mixers on the right */
				.right_mixer = (lm_cfg->id - LM_0) & 0x1,
			};
		}

		/* Validation! */
		for (lm = 0; lm < rm->num_mixer; lm++) {
			pr_debug("Using LM_%d %s (%ux%u)\n", rm->mixer[lm].idx - LM_0, dsc_merge ? (rm->mixer[lm].cfg.right_mixer ? "right" : "left") : "solo",
				 rm->mixer[lm].cfg.out_width, rm->mixer[lm].cfg.out_height);
		}
	}

	/*
	 * Pick the SSPP(s). We don't do anything fancy so just use the DMA SSPPs.
	 * For dual DSI we typically share a single SSPP except for the quad pipe
	 * DSC merge case.
	 */
	if (dsc_merge || enc_is_primary(rm)) {
		enum dpu_sspp sspp_id = SSPP_DMA0; // SSPP_VIG0;
		const struct dpu_sspp_cfg *sspp_cfg;

		/* Like with the CTL, we use a single SSPP unless we're doing quad pipe */
		if (dsc_merge && !enc_is_primary(rm))
			sspp_id += 1;

		sspp_cfg = find_sspp_cfg(cat, sspp_id);
		if (!sspp_cfg) {
			pr_err("Couldn't find SSPP %u\n", sspp_id);
			return -ENOENT;
		}

		rm->sspp->hw.blk_addr = req->dpu_base + sspp_cfg->base;
		rm->sspp->idx = sspp_id;
		rm->sspp->cap = sspp_cfg;
		rm->sspp->mdss_ver = cat->mdss_ver;
	}

	return 0;
}

/*
 * We only support configuring two INTFs in dual-DSI. If
 * dual_dsi is true then encs is expected to be an array of
 * two encoders.
 */
int dpu_hw_get_encoder_topology(const struct dpu_resource_request *req, bool dual_dsi, struct dpu_topology *topology)
{
	struct dpu_hw_sspp *sspp = &topology->sspp[0];
	struct dpu_hw_ctl *ctl = &topology->ctl[0];
	int ret;

	/* Check if we need more than 1 DSC encoder per panel (accounting with width being the total width in dual-DSI case) */
	if (req->dsc && req->width > DSC_MAX_WIDTH * (dual_dsi + 1))
		topology->dsc_merge = true;

	topology->num_sspp = 1;
	topology->num_encs = 1;
	topology->num_ctl = 1;

	if (dual_dsi) {
		topology->num_encs = 2;
		topology->encs[0].split_role = ENC_ROLE_PRIMARY;
		topology->encs[1].split_role = ENC_ROLE_SECONDARY;

		/* Figure out if we need to use quad pipe topology with DSC merge */
		if (req->dsc && topology->dsc_merge)
			topology->num_sspp = 2;
	} else {
		topology->encs[0].split_role = ENC_ROLE_SOLO;
	}

	topology->encs[0].ctl = ctl;
	topology->encs[0].sspp = sspp;

	ret = dpu_hw_intf_init_resources(&topology->encs[0], req);
	if (ret)
		return ret;

	if (!dual_dsi) {
		dump_encoder_topology(&topology->encs[0]);
		return 0;
	}

	/* If quad pipe, configure a second CTL and SSPP for the second encoder */
	if (topology->dsc_merge) {
		// ctl = &topology->ctl[1];
		sspp = &topology->sspp[1];
	}

	/* Set the CTL and SSPP for the second encoder */
	topology->encs[1].ctl = ctl;
	topology->encs[1].sspp = sspp;

	/* Configure the topology for the second encoder */
	ret = dpu_hw_intf_init_resources(&topology->encs[1], req);
	if (ret)
		return ret;

	printf("\nDPU Topology%s:\n", topology->dsc_merge ? " (DSC Merge)" : "");
	dump_encoder_topology(&topology->encs[0]);
	dump_encoder_topology(&topology->encs[1]);

	// FIXME: assert num_ctl/sspp/enc = 2 for dsc_merge

	return 0;
}
