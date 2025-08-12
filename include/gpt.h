// SPDX-License-Identifier: GPL-2.0+
/*
 * gpt.c -- GPT (GUID Partition Table)
 *
 * Copyright (C) 2015
 * Lukasz Majewski <l.majewski@majess.pl>
 *
 * Copyright (C) 2012 Samsung Electronics
 * author: Lukasz Majewski <l.majewski@samsung.com>
 * author: Piotr Wilczek <p.wilczek@samsung.com>
 */


void print_gpt_info(void);
int do_get_gpt_info(struct blk_desc *dev_desc, char * const namestr);
int gpt_repair(struct blk_desc *blk_dev_desc);
int gpt_verify(struct blk_desc *blk_dev_desc, const char *str_part);
int gpt_enumerate(struct blk_desc *desc);
int gpt_setenv_part_variables(struct disk_partition *pinfo, int i);
int gpt_setenv(struct blk_desc *desc, const char *name);
int do_disk_guid(struct blk_desc *dev_desc, char * const namestr);
int do_rename_gpt_parts(struct blk_desc *dev_desc, char *subcomm, char *name1, char *name2);
int gpt_set_bootable(struct blk_desc *blk_dev_desc, char *const part_list);
int gpt_set_xbl_partition_bootable_partfstype(struct blk_desc *blk_dev_desc, char *const partname);
int gpt_set_xbl_partition_bootable_by_slot_partfstype(struct blk_desc *blk_dev_desc, char *const slotname);
char gpt_get_xbl_active_partition_slot(struct blk_desc *blk_dev_desc, char *foundslotname);