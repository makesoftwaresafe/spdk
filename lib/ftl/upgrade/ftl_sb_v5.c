/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright 2023 Solidigm All Rights Reserved
 *   All rights reserved.
 */

#include "spdk/string.h"

#include "ftl_sb_v5.h"
#include "ftl_core.h"
#include "ftl_layout.h"
#include "ftl_band.h"
#include "upgrade/ftl_sb_prev.h"
#include "upgrade/ftl_sb_upgrade.h"
#include "upgrade/ftl_layout_upgrade.h"
#include "utils/ftl_layout_tracker_bdev.h"

typedef size_t (*blob_store_fn)(struct spdk_ftl_dev *dev, void *blob_buf, size_t blob_buf_sz);
typedef int (*blob_load_fn)(struct spdk_ftl_dev *dev, void *blob_buf, size_t blob_sz);

bool
ftl_superblock_v5_is_blob_area_empty(union ftl_superblock_ver *sb_ver)
{
	return sb_ver->v5.blob_area_end == 0;
}

static bool
validate_blob_area(struct ftl_superblock_v5_md_blob_hdr *sb_blob_hdr,
		   ftl_df_obj_id sb_blob_area_end)
{
	return sb_blob_hdr->df_id <= sb_blob_area_end &&
	       (sb_blob_hdr->df_id + sb_blob_hdr->blob_sz) <= sb_blob_area_end;
}

bool
ftl_superblock_v5_validate_blob_area(struct spdk_ftl_dev *dev)
{
	union ftl_superblock_ver *sb_ver = (union ftl_superblock_ver *)dev->sb;

	return validate_blob_area(&sb_ver->v5.md_layout_nvc, sb_ver->v5.blob_area_end) &&
	       validate_blob_area(&sb_ver->v5.md_layout_base, sb_ver->v5.blob_area_end) &&
	       validate_blob_area(&sb_ver->v5.layout_params, sb_ver->v5.blob_area_end);
}

static size_t
sb_blob_store(struct spdk_ftl_dev *dev, struct ftl_superblock_v5_md_blob_hdr *sb_blob_hdr,
	      blob_store_fn blob_store, void *sb_blob_area)
{
	struct ftl_superblock_v5 *sb = (struct ftl_superblock_v5 *)dev->sb;
	uintptr_t sb_end = ((uintptr_t)sb) + FTL_SUPERBLOCK_SIZE;
	size_t blob_sz = sb_end - (uintptr_t)sb_blob_area;

	/* Test SB blob area overflow */
	if ((uintptr_t)sb_blob_area < (uintptr_t)sb->blob_area) {
		ftl_bug(true);
		return 0;
	}
	if ((uintptr_t)sb_blob_area >= sb_end) {
		ftl_bug(true);
		return 0;
	}

	blob_sz = blob_store(dev, sb_blob_area, blob_sz);
	sb_blob_hdr->blob_sz = blob_sz;
	sb_blob_hdr->df_id = ftl_df_get_obj_id(sb->blob_area, sb_blob_area);
	return blob_sz;
}

static size_t
base_blob_store(struct spdk_ftl_dev *dev, void *blob_buf, size_t blob_buf_sz)
{
	return ftl_layout_tracker_bdev_blob_store(dev->base_layout_tracker, blob_buf, blob_buf_sz);
}

static size_t
nvc_blob_store(struct spdk_ftl_dev *dev, void *blob_buf, size_t blob_buf_sz)
{
	return ftl_layout_tracker_bdev_blob_store(dev->nvc_layout_tracker, blob_buf, blob_buf_sz);
}

int
ftl_superblock_v5_store_blob_area(struct spdk_ftl_dev *dev)
{
	struct ftl_superblock_v5 *sb = (struct ftl_superblock_v5 *)dev->sb;
	void *sb_blob_area;
	size_t blob_sz;

	/* Store the NVC-backed FTL MD layout info */
	sb_blob_area = ftl_df_get_obj_ptr(sb->blob_area, 0);
	spdk_strcpy_pad(sb->nvc_dev_name, dev->nv_cache.nvc_desc->name,
			SPDK_COUNTOF(sb->nvc_dev_name), '\0');
	blob_sz = sb_blob_store(dev, &sb->md_layout_nvc, nvc_blob_store, sb_blob_area);
	FTL_NOTICELOG(dev, "nvc layout blob store 0x%"PRIx64" bytes\n", blob_sz);
	if (!blob_sz) {
		return -1;
	}

	/* Store the base dev-backed FTL MD layout info */
	sb_blob_area += blob_sz;
	spdk_strcpy_pad(sb->base_dev_name, dev->base_type->name, SPDK_COUNTOF(sb->base_dev_name), '\0');
	blob_sz = sb_blob_store(dev, &sb->md_layout_base, base_blob_store, sb_blob_area);
	FTL_NOTICELOG(dev, "base layout blob store 0x%"PRIx64" bytes\n", blob_sz);
	if (!blob_sz) {
		return -1;
	}

	/* Store the region props */
	sb_blob_area += blob_sz;
	blob_sz = sb_blob_store(dev, &sb->layout_params, ftl_layout_blob_store, sb_blob_area);
	FTL_NOTICELOG(dev, "layout blob store 0x%"PRIx64" bytes\n", blob_sz);
	if (!blob_sz) {
		return -1;
	}

	/* Update the blob area end */
	sb_blob_area += blob_sz;
	sb->blob_area_end = ftl_df_get_obj_id(sb->blob_area, sb_blob_area);

	return 0;
}

typedef const struct ftl_layout_tracker_bdev_region_props *(*sb_md_layout_find_fn)(
	struct spdk_ftl_dev *dev, struct ftl_layout_tracker_bdev *layout_tracker,
	enum ftl_layout_region_type reg_type, void *find_filter);

static int
sb_blob_load(struct spdk_ftl_dev *dev, struct ftl_superblock_v5_md_blob_hdr *sb_blob_hdr,
	     blob_load_fn blob_load)
{
	struct ftl_superblock_v5 *sb = (struct ftl_superblock_v5 *)dev->sb;
	uintptr_t sb_end = ((uintptr_t)sb) + FTL_SUPERBLOCK_SIZE;
	void *blob_area;

	if (sb_blob_hdr->df_id == FTL_DF_OBJ_ID_INVALID) {
		/* Uninitialized blob */
		return -1;
	}

	blob_area = ftl_df_get_obj_ptr(sb->blob_area, sb_blob_hdr->df_id);

	/* Test SB blob area overflow */
	if ((uintptr_t)blob_area < (uintptr_t)sb->blob_area) {
		ftl_bug(true);
		return -1;
	}
	if ((uintptr_t)blob_area + sb_blob_hdr->blob_sz >= sb_end) {
		ftl_bug(true);
		return -1;
	}

	return blob_load(dev, blob_area, sb_blob_hdr->blob_sz);
}

static int
base_blob_load(struct spdk_ftl_dev *dev, void *blob_buf, size_t blob_sz)
{
	return ftl_layout_tracker_bdev_blob_load(dev->base_layout_tracker, blob_buf, blob_sz);
}

static int
nvc_blob_load(struct spdk_ftl_dev *dev, void *blob_buf, size_t blob_sz)
{
	return ftl_layout_tracker_bdev_blob_load(dev->nvc_layout_tracker, blob_buf, blob_sz);
}

int
ftl_superblock_v5_load_blob_area(struct spdk_ftl_dev *dev)
{
	struct ftl_superblock_v5 *sb = (struct ftl_superblock_v5 *)dev->sb;

	/* Load the NVC-backed FTL MD layout info */
	if (strncmp(sb->nvc_dev_name, dev->nv_cache.nvc_desc->name, SPDK_COUNTOF(sb->nvc_dev_name))) {
		return -1;
	}
	FTL_NOTICELOG(dev, "nvc layout blob load 0x%"PRIx64" bytes\n", (uint64_t)sb->md_layout_nvc.blob_sz);
	if (sb_blob_load(dev, &sb->md_layout_nvc, nvc_blob_load)) {
		return -1;
	}

	/* Load the base dev-backed FTL MD layout info */
	if (strncmp(sb->base_dev_name, dev->base_type->name, SPDK_COUNTOF(sb->base_dev_name))) {
		return -1;
	}
	FTL_NOTICELOG(dev, "base layout blob load 0x%"PRIx64" bytes\n",
		      (uint64_t)sb->md_layout_base.blob_sz);
	if (sb_blob_load(dev, &sb->md_layout_base, base_blob_load)) {
		return -1;
	}

	/* Load the region props */
	FTL_NOTICELOG(dev, "layout blob load 0x%"PRIx64" bytes\n", (uint64_t)sb->layout_params.blob_sz);
	if (sb_blob_load(dev, &sb->layout_params, ftl_layout_blob_load)) {
		return -1;
	}

	return 0;
}

void
ftl_superblock_v5_md_layout_dump(struct spdk_ftl_dev *dev)
{
	struct ftl_layout_tracker_bdev *nvc_layout_tracker = dev->nvc_layout_tracker;
	struct ftl_layout_tracker_bdev *base_layout_tracker = dev->base_layout_tracker;
	const struct ftl_layout_tracker_bdev_region_props *reg_search_ctx = NULL;

	FTL_NOTICELOG(dev, "SB metadata layout - nvc:\n");
	while (true) {
		ftl_layout_tracker_bdev_find_next_region(nvc_layout_tracker, FTL_LAYOUT_REGION_TYPE_INVALID,
				&reg_search_ctx);
		if (!reg_search_ctx) {
			break;
		}

		FTL_NOTICELOG(dev,
			      "Region type:0x%"PRIx32" ver:%"PRIu32" blk_offs:0x%"PRIx64" blk_sz:0x%"PRIx64"\n",
			      reg_search_ctx->type, reg_search_ctx->ver, reg_search_ctx->blk_offs, reg_search_ctx->blk_sz);
	}

	reg_search_ctx = NULL;
	FTL_NOTICELOG(dev, "SB metadata layout - base dev:\n");
	while (true) {
		ftl_layout_tracker_bdev_find_next_region(base_layout_tracker, FTL_LAYOUT_REGION_TYPE_INVALID,
				&reg_search_ctx);
		if (!reg_search_ctx) {
			break;
		}

		FTL_NOTICELOG(dev,
			      "Region type:0x%"PRIx32" ver:%"PRIu32" blk_offs:0x%"PRIx64" blk_sz:0x%"PRIx64"\n",
			      reg_search_ctx->type, reg_search_ctx->ver, reg_search_ctx->blk_offs, reg_search_ctx->blk_sz);
	}
}

static int
layout_apply_from_sb_blob(struct spdk_ftl_dev *dev, struct ftl_layout_tracker_bdev *layout_tracker,
			  int (*filter_region_type_fn)(enum ftl_layout_region_type))
{
	struct ftl_layout_region *reg;
	const struct ftl_layout_tracker_bdev_region_props *reg_search_ctx = NULL;

	while (true) {
		ftl_layout_tracker_bdev_find_next_region(layout_tracker, FTL_LAYOUT_REGION_TYPE_INVALID,
				&reg_search_ctx);
		if (!reg_search_ctx) {
			break;
		}
		if (reg_search_ctx->type == FTL_LAYOUT_REGION_TYPE_FREE) {
			continue;
		}
		if (filter_region_type_fn(reg_search_ctx->type)) {
			FTL_ERRLOG(dev, "Unknown region found in layout blob: type 0x%"PRIx32"\n", reg_search_ctx->type);
			return -1;
		}

		reg = &dev->layout.region[reg_search_ctx->type];

		/* First region of a given type found */
		if (reg->type == FTL_LAYOUT_REGION_TYPE_INVALID) {
			reg->type = reg_search_ctx->type;
			reg->current.version = reg_search_ctx->ver;
			reg->current.offset = reg_search_ctx->blk_offs;
			reg->current.blocks = reg_search_ctx->blk_sz;
			continue;
		}

		/* Update to the oldest region version found */
		if (reg_search_ctx->ver < reg->current.version) {
			reg->current.version = reg_search_ctx->ver;
			reg->current.offset = reg_search_ctx->blk_offs;
			reg->current.blocks = reg_search_ctx->blk_sz;
			continue;
		}

		/* Skip newer region versions */
		if (reg_search_ctx->ver > reg->current.version) {
			continue;
		}

		/* Current region version already found */
		assert(reg_search_ctx->ver == reg->current.version);
		if (reg->current.offset != reg_search_ctx->blk_offs ||
		    reg->current.blocks != reg_search_ctx->blk_sz) {
			FTL_ERRLOG(dev, "Corrupted layout blob: reg type 0x%"PRIx32"\n", reg_search_ctx->type);
			return -1;
		}
	}
	return 0;
}

static int
layout_region_verify(struct spdk_ftl_dev *dev, enum ftl_layout_region_type reg_type,
		     uint32_t reg_ver)
{
	struct ftl_layout_region *reg = ftl_layout_region_get(dev, reg_type);

	if (!reg) {
		FTL_ERRLOG(dev, "Region not found in nvc layout blob: reg type 0x%"PRIx32"\n", reg_type);
		return -1;
	}

	/* Unknown version found in the blob */
	if (reg->current.version > reg_ver) {
		FTL_ERRLOG(dev, "Unknown region version found in layout blob: reg type 0x%"PRIx32"\n",
			   reg_type);
		return -1;
	}

	return 0;
}

static int
layout_fixup_base(struct spdk_ftl_dev *dev)
{
	struct ftl_layout_region_descr {
		enum ftl_layout_region_type type;
		uint32_t ver;
		int (*on_reg_miss)(struct spdk_ftl_dev *dev);
	};
	struct ftl_layout_region_descr *reg_descr;
	static struct ftl_layout_region_descr nvc_regs[] = {
		{ .type = FTL_LAYOUT_REGION_TYPE_SB_BASE, .ver = FTL_SB_VERSION_CURRENT },
		{ .type = FTL_LAYOUT_REGION_TYPE_DATA_BASE, .ver = 0 },
		{ .type = FTL_LAYOUT_REGION_TYPE_VALID_MAP, .ver = 0 },
		{ .type = FTL_LAYOUT_REGION_TYPE_INVALID, .ver = 0 },
	};

	for (reg_descr = nvc_regs; reg_descr->type != FTL_LAYOUT_REGION_TYPE_INVALID; reg_descr++) {
		struct ftl_layout_region *region;

		if (layout_region_verify(dev, reg_descr->type, reg_descr->ver) &&
		    reg_descr->on_reg_miss && reg_descr->on_reg_miss(dev)) {
			return -1;
		}

		region = &dev->layout.region[reg_descr->type];
		region->type = reg_descr->type;
		region->mirror_type = FTL_LAYOUT_REGION_TYPE_INVALID;
		region->name = ftl_md_region_name(reg_descr->type);

		region->bdev_desc = dev->base_bdev_desc;
		region->ioch = dev->base_ioch;
		region->vss_blksz = 0;
	}

	return 0;
}

static int
layout_fixup_nvc(struct spdk_ftl_dev *dev)
{
	struct ftl_layout_region_descr {
		enum ftl_layout_region_type type;
		uint32_t ver;
		enum ftl_layout_region_type mirror_type;
	};
	struct ftl_layout_region_descr *reg_descr;
	static struct ftl_layout_region_descr nvc_regs[] = {
		{ .type = FTL_LAYOUT_REGION_TYPE_SB, .ver = FTL_SB_VERSION_CURRENT, .mirror_type = FTL_LAYOUT_REGION_TYPE_SB_BASE },
		{ .type = FTL_LAYOUT_REGION_TYPE_L2P, .ver = 0 },
		{ .type = FTL_LAYOUT_REGION_TYPE_BAND_MD, .ver = FTL_BAND_VERSION_CURRENT, .mirror_type = FTL_LAYOUT_REGION_TYPE_BAND_MD_MIRROR },
		{ .type = FTL_LAYOUT_REGION_TYPE_BAND_MD_MIRROR, .ver = FTL_BAND_VERSION_CURRENT },
		{ .type = FTL_LAYOUT_REGION_TYPE_TRIM_MD, .ver = 0, .mirror_type = FTL_LAYOUT_REGION_TYPE_TRIM_MD_MIRROR },
		{ .type = FTL_LAYOUT_REGION_TYPE_TRIM_MD_MIRROR, .ver = 0 },
		{ .type = FTL_LAYOUT_REGION_TYPE_NVC_MD, .ver = FTL_NVC_VERSION_CURRENT, .mirror_type = FTL_LAYOUT_REGION_TYPE_NVC_MD_MIRROR },
		{ .type = FTL_LAYOUT_REGION_TYPE_NVC_MD_MIRROR, .ver = FTL_NVC_VERSION_CURRENT },
		{ .type = FTL_LAYOUT_REGION_TYPE_DATA_NVC, .ver = 0 },
		{ .type = FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC, .ver = FTL_P2L_VERSION_CURRENT },
		{ .type = FTL_LAYOUT_REGION_TYPE_P2L_CKPT_GC_NEXT, .ver = FTL_P2L_VERSION_CURRENT },
		{ .type = FTL_LAYOUT_REGION_TYPE_P2L_CKPT_COMP, .ver = FTL_P2L_VERSION_CURRENT },
		{ .type = FTL_LAYOUT_REGION_TYPE_P2L_CKPT_COMP_NEXT, .ver = FTL_P2L_VERSION_CURRENT },
		{ .type = FTL_LAYOUT_REGION_TYPE_INVALID, .ver = 0 },
	};

	for (reg_descr = nvc_regs; reg_descr->type != FTL_LAYOUT_REGION_TYPE_INVALID; reg_descr++) {
		struct ftl_layout_region *region;

		if (layout_region_verify(dev, reg_descr->type, reg_descr->ver)) {
			return -1;
		}

		region = &dev->layout.region[reg_descr->type];
		region->type = reg_descr->type;
		region->mirror_type = FTL_LAYOUT_REGION_TYPE_INVALID;
		region->name = ftl_md_region_name(reg_descr->type);

		region->bdev_desc = dev->nv_cache.bdev_desc;
		region->ioch = dev->nv_cache.cache_ioch;
		region->vss_blksz = dev->nv_cache.md_size;

		if (reg_descr->mirror_type) {
			dev->layout.region[reg_descr->type].mirror_type = reg_descr->mirror_type;
		}
	}

	return 0;
}

static int
filter_region_type_base(enum ftl_layout_region_type reg_type)
{
	switch (reg_type) {
	case FTL_LAYOUT_REGION_TYPE_SB_BASE:
	case FTL_LAYOUT_REGION_TYPE_DATA_BASE:
	case FTL_LAYOUT_REGION_TYPE_VALID_MAP:
		return 0;

	default:
		return 1;
	}
}

static int
filter_region_type_nvc(enum ftl_layout_region_type reg_type)
{
	return filter_region_type_base(reg_type) ? 0 : 1;
}

static int
layout_apply_nvc(struct spdk_ftl_dev *dev)
{
	if (layout_apply_from_sb_blob(dev, dev->nvc_layout_tracker, filter_region_type_nvc) ||
	    layout_fixup_nvc(dev)) {
		return -1;
	}
	return 0;
}

static int
layout_apply_base(struct spdk_ftl_dev *dev)
{
	if (layout_apply_from_sb_blob(dev, dev->base_layout_tracker, filter_region_type_base) ||
	    layout_fixup_base(dev)) {
		return -1;
	}
	return 0;
}

int
ftl_superblock_v5_md_layout_apply(struct spdk_ftl_dev *dev)
{
	if (layout_apply_nvc(dev) || layout_apply_base(dev)) {
		return -1;
	}
	return 0;
}
