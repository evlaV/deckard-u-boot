/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __QCOM_MDT_LOADER_H__
#define __QCOM_MDT_LOADER_H__

#include <linux/types.h>

#define QCOM_MDT_TYPE_MASK	(7 << 24)
#define QCOM_MDT_TYPE_HASH	(2 << 24)
#define QCOM_MDT_RELOCATABLE	BIT(27)

/**
 * struct qcom_mdt_firmware - A place for storing firmware and its attribute data.
 *
 * This holds information about a firmware and its content.
 *
 * @size: Size of a file
 * @data: Buffer for file
 */
struct qcom_mdt_firmware {
	size_t size;
	const u8 *data;
};

#if IS_ENABLED(CONFIG_QCOM_MDT_LOADER)

ssize_t qcom_mdt_get_size(const struct qcom_mdt_firmware *fw);
int qcom_mdt_pas_init(const struct qcom_mdt_firmware *fw,
		      const char *fw_name, int pas_id, phys_addr_t mem_phys,
		      void *pas_metadata_ctx);
int qcom_mdt_load(const struct qcom_mdt_firmware *fw,
		  const char *fw_name, int pas_id, void *mem_region,
		  phys_addr_t mem_phys, size_t mem_size,
		  phys_addr_t *reloc_base);

int qcom_mdt_load_no_init(const struct qcom_mdt_firmware *fw,
			  const char *fw_name, void *mem_region,
			  phys_addr_t mem_phys, size_t mem_size,
			  phys_addr_t *reloc_base);
void *qcom_mdt_read_metadata(const struct qcom_mdt_firmware *fw, size_t *data_len,
			     const char *fw_name);

#else /* !IS_ENABLED(CONFIG_QCOM_MDT_LOADER) */

static inline ssize_t qcom_mdt_get_size(const struct qcom_mdt_firmware *fw)
{
	return -ENODEV;
}

static inline int qcom_mdt_pas_init(const struct qcom_mdt_firmware *fw,
				    const char *fw_name, int pas_id, phys_addr_t mem_phys,
				    void *pas_metadata_ctx)
{
	return -ENODEV;
}

static inline int qcom_mdt_load(const struct qcom_mdt_firmware *fw,
				const char *fw_name, int pas_id,
				void *mem_region, phys_addr_t mem_phys,
				size_t mem_size, phys_addr_t *reloc_base)
{
	return -ENODEV;
}

static inline int qcom_mdt_load_no_init(const struct qcom_mdt_firmware *fw,
					const char *fw_name, void *mem_region,
					phys_addr_t mem_phys, size_t mem_size,
					phys_addr_t *reloc_base)
{
	return -ENODEV;
}

static inline void *qcom_mdt_read_metadata(const struct qcom_mdt_firmware *fw,
					   size_t *data_len, const char *fw_name)
{
	return ERR_PTR(-ENODEV);
}

#endif /* !IS_ENABLED(CONFIG_QCOM_MDT_LOADER) */

#endif
