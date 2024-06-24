/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2010-2015, 2018-2019 The Linux Foundation. All rights reserved.
 * Copyright (C) 2015 Linaro Ltd.
 */
#ifndef __QCOM_SCM_H
#define __QCOM_SCM_H

#include <linux/err.h>
#include <linux/types.h>

#include <dt-bindings/firmware/qcom,scm.h>

#define QCOM_SCM_VERSION(major, minor)	(((major) << 16) | ((minor) & 0xFF))

struct qcom_scm_pas_metadata {
	void *ptr;
	dma_addr_t phys;
	ssize_t size;
};

int qcom_scm_pas_init_image(u32 peripheral, const void *metadata, size_t size,
			    struct qcom_scm_pas_metadata *ctx);
void qcom_scm_pas_metadata_release(struct qcom_scm_pas_metadata *ctx);
int qcom_scm_pas_mem_setup(u32 peripheral, phys_addr_t addr, phys_addr_t size);
int qcom_scm_pas_auth_and_reset(u32 peripheral);
int qcom_scm_pas_shutdown(u32 peripheral);
bool qcom_scm_pas_supported(u32 peripheral);

#endif
