// SPDX-License-Identifier: GPL-2.0

/* Copyright (c) 2016-2018, The Linux Foundation. All rights reserved.
 * Copyright (C) 2018 Linaro Ltd.
 */
#ifndef _IPAHAL_FLTRT_H_
#define _IPAHAL_FLTRT_H_

#include "ipa_common_i.h"

/* Get the H/W table (flt/rt) header width */
u32 ipahal_get_hw_tbl_hdr_width(void);

/* ipahal_rt_generate_empty_img() - Generate empty route image
 *  Creates routing header buffer for the given tables number.
 * For each table, make it point to the empty table on DDR.
 * @tbls_num: Number of tables. For each will have an entry in the header
 * @mem: mem object that points to DMA mem representing the hdr structure
 */
int ipahal_rt_generate_empty_img(u32 tbls_num, struct ipa_mem_buffer *mem);

/* ipahal_flt_generate_empty_img() - Generate empty filter image
 *  Creates filter header buffer for the given tables number.
 *  For each table, make it point to the empty table on DDR.
 * @tbls_num: Number of tables. For each will have an entry in the header
 * @ep_bitmap: Bitmap representing the EP that has flt tables. The format
 *  should be: bit0->EP0, bit1->EP1
 * @mem: mem object that points to DMA mem representing the hdr structure
 */
int ipahal_flt_generate_empty_img(u32 tbls_num, u64 ep_bitmap,
				  struct ipa_mem_buffer *mem);

/* ipahal_free_empty_img() - free empty filter or route image
 */
void ipahal_free_empty_img(struct ipa_mem_buffer *mem);

#endif /* _IPAHAL_FLTRT_H_ */
