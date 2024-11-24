// SPDX-License-Identifier: GPL-2.0

#ifndef __QCOM_PRIV_H__
#define __QCOM_PRIV_H__

#include <stdbool.h>

#if IS_ENABLED(CONFIG_EFI_HAVE_CAPSULE_SUPPORT)
void qcom_configure_capsule_updates(void);
#else
void qcom_configure_capsule_updates(void) {}
#endif /* EFI_HAVE_CAPSULE_SUPPORT */

int qcom_parse_memory(const void *fdt, bool fdt_is_internal);

#endif /* __QCOM_PRIV_H__ */
