/*
 * Copyright (C) 2007 Oracle.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#define _XOPEN_SOURCE 500
#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <getopt.h>
#include <uuid/uuid.h>
#include "kerncompat.h"
#include "ctree.h"
#include "volumes.h"
#include "repair.h"
#include "disk-io.h"
#include "print-tree.h"
#include "transaction.h"
#include "list.h"
#include "version.h"
#include "utils.h"
#include "commands.h"
#include "free-space-cache.h"

static u64 bytes_used = 0;
static u64 total_csum_bytes = 0;
static u64 total_btree_bytes = 0;
static u64 total_fs_tree_bytes = 0;
static u64 total_extent_tree_bytes = 0;
static u64 btree_space_waste = 0;
static u64 data_bytes_allocated = 0;
static u64 data_bytes_referenced = 0;
static int found_old_backref = 0;

struct extent_backref {
	struct list_head list;
	unsigned int is_data:1;
	unsigned int found_extent_tree:1;
	unsigned int full_backref:1;
	unsigned int found_ref:1;
};

struct data_backref {
	struct extent_backref node;
	union {
		u64 parent;
		u64 root;
	};
	u64 owner;
	u64 offset;
	u64 bytes;
	u32 num_refs;
	u32 found_ref;
};

struct tree_backref {
	struct extent_backref node;
	union {
		u64 parent;
		u64 root;
	};
};

struct extent_record {
	struct list_head backrefs;
	struct cache_extent cache;
	struct btrfs_disk_key parent_key;
	u64 start;
	u64 max_size;
	u64 nr;
	u64 refs;
	u64 extent_item_refs;
	u64 generation;
	u64 info_objectid;
	u8 info_level;
	unsigned int content_checked:1;
	unsigned int owner_ref_checked:1;
	unsigned int is_root:1;
	unsigned int metadata:1;
};

struct inode_backref {
	struct list_head list;
	unsigned int found_dir_item:1;
	unsigned int found_dir_index:1;
	unsigned int found_inode_ref:1;
	unsigned int filetype:8;
	int errors;
	unsigned int ref_type;
	u64 dir;
	u64 index;
	u16 namelen;
	char name[0];
};

#define REF_ERR_NO_DIR_ITEM		(1 << 0)
#define REF_ERR_NO_DIR_INDEX		(1 << 1)
#define REF_ERR_NO_INODE_REF		(1 << 2)
#define REF_ERR_DUP_DIR_ITEM		(1 << 3)
#define REF_ERR_DUP_DIR_INDEX		(1 << 4)
#define REF_ERR_DUP_INODE_REF		(1 << 5)
#define REF_ERR_INDEX_UNMATCH		(1 << 6)
#define REF_ERR_FILETYPE_UNMATCH	(1 << 7)
#define REF_ERR_NAME_TOO_LONG		(1 << 8) // 100
#define REF_ERR_NO_ROOT_REF		(1 << 9)
#define REF_ERR_NO_ROOT_BACKREF		(1 << 10)
#define REF_ERR_DUP_ROOT_REF		(1 << 11)
#define REF_ERR_DUP_ROOT_BACKREF	(1 << 12)

struct inode_record {
	struct list_head backrefs;
	unsigned int checked:1;
	unsigned int merging:1;
	unsigned int found_inode_item:1;
	unsigned int found_dir_item:1;
	unsigned int found_file_extent:1;
	unsigned int found_csum_item:1;
	unsigned int some_csum_missing:1;
	unsigned int nodatasum:1;
	int errors;

	u64 ino;
	u32 nlink;
	u32 imode;
	u64 isize;
	u64 nbytes;

	u32 found_link;
	u64 found_size;
	u64 extent_start;
	u64 extent_end;
	u64 first_extent_gap;

	u32 refs;
};

#define I_ERR_NO_INODE_ITEM		(1 << 0)
#define I_ERR_NO_ORPHAN_ITEM		(1 << 1)
#define I_ERR_DUP_INODE_ITEM		(1 << 2)
#define I_ERR_DUP_DIR_INDEX		(1 << 3)
#define I_ERR_ODD_DIR_ITEM		(1 << 4)
#define I_ERR_ODD_FILE_EXTENT		(1 << 5)
#define I_ERR_BAD_FILE_EXTENT		(1 << 6)
#define I_ERR_FILE_EXTENT_OVERLAP	(1 << 7)
#define I_ERR_FILE_EXTENT_DISCOUNT	(1 << 8) // 100
#define I_ERR_DIR_ISIZE_WRONG		(1 << 9)
#define I_ERR_FILE_NBYTES_WRONG		(1 << 10) // 400
#define I_ERR_ODD_CSUM_ITEM		(1 << 11)
#define I_ERR_SOME_CSUM_MISSING		(1 << 12)
#define I_ERR_LINK_COUNT_WRONG		(1 << 13)

struct root_backref {
	struct list_head list;
	unsigned int found_dir_item:1;
	unsigned int found_dir_index:1;
	unsigned int found_back_ref:1;
	unsigned int found_forward_ref:1;
	unsigned int reachable:1;
	int errors;
	u64 ref_root;
	u64 dir;
	u64 index;
	u16 namelen;
	char name[0];
};

struct root_record {
	struct list_head backrefs;
	struct cache_extent cache;
	unsigned int found_root_item:1;
	u64 objectid;
	u32 found_ref;
};

struct ptr_node {
	struct cache_extent cache;
	void *data;
};

struct shared_node {
	struct cache_extent cache;
	struct cache_tree root_cache;
	struct cache_tree inode_cache;
	struct inode_record *current;
	u32 refs;
};

struct block_info {
	u64 start;
	u32 size;
};

struct walk_control {
	struct cache_tree shared;
	struct shared_node *nodes[BTRFS_MAX_LEVEL];
	int active_node;
	int root_level;
};

static u8 imode_to_type(u32 imode)
{
#define S_SHIFT 12
	static unsigned char btrfs_type_by_mode[S_IFMT >> S_SHIFT] = {
		[S_IFREG >> S_SHIFT]	= BTRFS_FT_REG_FILE,
		[S_IFDIR >> S_SHIFT]	= BTRFS_FT_DIR,
		[S_IFCHR >> S_SHIFT]	= BTRFS_FT_CHRDEV,
		[S_IFBLK >> S_SHIFT]	= BTRFS_FT_BLKDEV,
		[S_IFIFO >> S_SHIFT]	= BTRFS_FT_FIFO,
		[S_IFSOCK >> S_SHIFT]	= BTRFS_FT_SOCK,
		[S_IFLNK >> S_SHIFT]	= BTRFS_FT_SYMLINK,
	};

	return btrfs_type_by_mode[(imode & S_IFMT) >> S_SHIFT];
#undef S_SHIFT
}

static struct inode_record *clone_inode_rec(struct inode_record *orig_rec)
{
	struct inode_record *rec;
	struct inode_backref *backref;
	struct inode_backref *orig;
	size_t size;

	rec = malloc(sizeof(*rec));
	memcpy(rec, orig_rec, sizeof(*rec));
	rec->refs = 1;
	INIT_LIST_HEAD(&rec->backrefs);

	list_for_each_entry(orig, &orig_rec->backrefs, list) {
		size = sizeof(*orig) + orig->namelen + 1;
		backref = malloc(size);
		memcpy(backref, orig, size);
		list_add_tail(&backref->list, &rec->backrefs);
	}
	return rec;
}

static struct inode_record *get_inode_rec(struct cache_tree *inode_cache,
					  u64 ino, int mod)
{
	struct ptr_node *node;
	struct cache_extent *cache;
	struct inode_record *rec = NULL;
	int ret;

	cache = find_cache_extent(inode_cache, ino, 1);
	if (cache) {
		node = container_of(cache, struct ptr_node, cache);
		rec = node->data;
		if (mod && rec->refs > 1) {
			node->data = clone_inode_rec(rec);
			rec->refs--;
			rec = node->data;
		}
	} else if (mod) {
		rec = calloc(1, sizeof(*rec));
		rec->ino = ino;
		rec->extent_start = (u64)-1;
		rec->first_extent_gap = (u64)-1;
		rec->refs = 1;
		INIT_LIST_HEAD(&rec->backrefs);

		node = malloc(sizeof(*node));
		node->cache.start = ino;
		node->cache.size = 1;
		node->data = rec;

		if (ino == BTRFS_FREE_INO_OBJECTID)
			rec->found_link = 1;

		ret = insert_existing_cache_extent(inode_cache, &node->cache);
		BUG_ON(ret);
	}
	return rec;
}

static void free_inode_rec(struct inode_record *rec)
{
	struct inode_backref *backref;

	if (--rec->refs > 0)
		return;

	while (!list_empty(&rec->backrefs)) {
		backref = list_entry(rec->backrefs.next,
				     struct inode_backref, list);
		list_del(&backref->list);
		free(backref);
	}
	free(rec);
}

static int can_free_inode_rec(struct inode_record *rec)
{
	if (!rec->errors && rec->checked && rec->found_inode_item &&
	    rec->nlink == rec->found_link && list_empty(&rec->backrefs))
		return 1;
	return 0;
}

static void maybe_free_inode_rec(struct cache_tree *inode_cache,
				 struct inode_record *rec)
{
	struct cache_extent *cache;
	struct inode_backref *tmp, *backref;
	struct ptr_node *node;
	unsigned char filetype;

	if (!rec->found_inode_item)
		return;

	filetype = imode_to_type(rec->imode);
	list_for_each_entry_safe(backref, tmp, &rec->backrefs, list) {
		if (backref->found_dir_item && backref->found_dir_index) {
			if (backref->filetype != filetype)
				backref->errors |= REF_ERR_FILETYPE_UNMATCH;
			if (!backref->errors && backref->found_inode_ref) {
				list_del(&backref->list);
				free(backref);
			}
		}
	}

	if (!rec->checked || rec->merging)
		return;

	if (S_ISDIR(rec->imode)) {
		if (rec->found_size != rec->isize)
			rec->errors |= I_ERR_DIR_ISIZE_WRONG;
		if (rec->found_file_extent)
			rec->errors |= I_ERR_ODD_FILE_EXTENT;
	} else if (S_ISREG(rec->imode) || S_ISLNK(rec->imode)) {
		if (rec->found_dir_item)
			rec->errors |= I_ERR_ODD_DIR_ITEM;
		if (rec->found_size != rec->nbytes)
			rec->errors |= I_ERR_FILE_NBYTES_WRONG;
		if (rec->extent_start == (u64)-1 || rec->extent_start > 0)
			rec->first_extent_gap = 0;
		if (rec->nlink > 0 && (rec->extent_end < rec->isize ||
		    rec->first_extent_gap < rec->isize))
			rec->errors |= I_ERR_FILE_EXTENT_DISCOUNT;
	}

	if (S_ISREG(rec->imode) || S_ISLNK(rec->imode)) {
		if (rec->found_csum_item && rec->nodatasum)
			rec->errors |= I_ERR_ODD_CSUM_ITEM;
		if (rec->some_csum_missing && !rec->nodatasum)
			rec->errors |= I_ERR_SOME_CSUM_MISSING;
	}

	BUG_ON(rec->refs != 1);
	if (can_free_inode_rec(rec)) {
		cache = find_cache_extent(inode_cache, rec->ino, 1);
		node = container_of(cache, struct ptr_node, cache);
		BUG_ON(node->data != rec);
		remove_cache_extent(inode_cache, &node->cache);
		free(node);
		free_inode_rec(rec);
	}
}

static int check_orphan_item(struct btrfs_root *root, u64 ino)
{
	struct btrfs_path path;
	struct btrfs_key key;
	int ret;

	key.objectid = BTRFS_ORPHAN_OBJECTID;
	key.type = BTRFS_ORPHAN_ITEM_KEY;
	key.offset = ino;

	btrfs_init_path(&path);
	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	btrfs_release_path(root, &path);
	if (ret > 0)
		ret = -ENOENT;
	return ret;
}

static int process_inode_item(struct extent_buffer *eb,
			      int slot, struct btrfs_key *key,
			      struct shared_node *active_node)
{
	struct inode_record *rec;
	struct btrfs_inode_item *item;

	rec = active_node->current;
	BUG_ON(rec->ino != key->objectid || rec->refs > 1);
	if (rec->found_inode_item) {
		rec->errors |= I_ERR_DUP_INODE_ITEM;
		return 1;
	}
	item = btrfs_item_ptr(eb, slot, struct btrfs_inode_item);
	rec->nlink = btrfs_inode_nlink(eb, item);
	rec->isize = btrfs_inode_size(eb, item);
	rec->nbytes = btrfs_inode_nbytes(eb, item);
	rec->imode = btrfs_inode_mode(eb, item);
	if (btrfs_inode_flags(eb, item) & BTRFS_INODE_NODATASUM)
		rec->nodatasum = 1;
	rec->found_inode_item = 1;
	if (rec->nlink == 0)
		rec->errors |= I_ERR_NO_ORPHAN_ITEM;
	maybe_free_inode_rec(&active_node->inode_cache, rec);
	return 0;
}

static struct inode_backref *get_inode_backref(struct inode_record *rec,
						const char *name,
						int namelen, u64 dir)
{
	struct inode_backref *backref;

	list_for_each_entry(backref, &rec->backrefs, list) {
		if (backref->dir != dir || backref->namelen != namelen)
			continue;
		if (memcmp(name, backref->name, namelen))
			continue;
		return backref;
	}

	backref = malloc(sizeof(*backref) + namelen + 1);
	memset(backref, 0, sizeof(*backref));
	backref->dir = dir;
	backref->namelen = namelen;
	memcpy(backref->name, name, namelen);
	backref->name[namelen] = '\0';
	list_add_tail(&backref->list, &rec->backrefs);
	return backref;
}

static int add_inode_backref(struct cache_tree *inode_cache,
			     u64 ino, u64 dir, u64 index,
			     const char *name, int namelen,
			     int filetype, int itemtype, int errors)
{
	struct inode_record *rec;
	struct inode_backref *backref;

	rec = get_inode_rec(inode_cache, ino, 1);
	backref = get_inode_backref(rec, name, namelen, dir);
	if (errors)
		backref->errors |= errors;
	if (itemtype == BTRFS_DIR_INDEX_KEY) {
		if (backref->found_dir_index)
			backref->errors |= REF_ERR_DUP_DIR_INDEX;
		if (backref->found_inode_ref && backref->index != index)
			backref->errors |= REF_ERR_INDEX_UNMATCH;
		if (backref->found_dir_item && backref->filetype != filetype)
			backref->errors |= REF_ERR_FILETYPE_UNMATCH;

		backref->index = index;
		backref->filetype = filetype;
		backref->found_dir_index = 1;
	} else if (itemtype == BTRFS_DIR_ITEM_KEY) {
		rec->found_link++;
		if (backref->found_dir_item)
			backref->errors |= REF_ERR_DUP_DIR_ITEM;
		if (backref->found_dir_index && backref->filetype != filetype)
			backref->errors |= REF_ERR_FILETYPE_UNMATCH;

		backref->filetype = filetype;
		backref->found_dir_item = 1;
	} else if ((itemtype == BTRFS_INODE_REF_KEY) ||
		   (itemtype == BTRFS_INODE_EXTREF_KEY)) {
		if (backref->found_inode_ref)
			backref->errors |= REF_ERR_DUP_INODE_REF;
		if (backref->found_dir_index && backref->index != index)
			backref->errors |= REF_ERR_INDEX_UNMATCH;

		backref->ref_type = itemtype;
		backref->index = index;
		backref->found_inode_ref = 1;
	} else {
		BUG_ON(1);
	}

	maybe_free_inode_rec(inode_cache, rec);
	return 0;
}

static int merge_inode_recs(struct inode_record *src, struct inode_record *dst,
			    struct cache_tree *dst_cache)
{
	struct inode_backref *backref;
	u32 dir_count = 0;

	dst->merging = 1;
	list_for_each_entry(backref, &src->backrefs, list) {
		if (backref->found_dir_index) {
			add_inode_backref(dst_cache, dst->ino, backref->dir,
					backref->index, backref->name,
					backref->namelen, backref->filetype,
					BTRFS_DIR_INDEX_KEY, backref->errors);
		}
		if (backref->found_dir_item) {
			dir_count++;
			add_inode_backref(dst_cache, dst->ino,
					backref->dir, 0, backref->name,
					backref->namelen, backref->filetype,
					BTRFS_DIR_ITEM_KEY, backref->errors);
		}
		if (backref->found_inode_ref) {
			add_inode_backref(dst_cache, dst->ino,
					backref->dir, backref->index,
					backref->name, backref->namelen, 0,
					backref->ref_type, backref->errors);
		}
	}

	if (src->found_dir_item)
		dst->found_dir_item = 1;
	if (src->found_file_extent)
		dst->found_file_extent = 1;
	if (src->found_csum_item)
		dst->found_csum_item = 1;
	if (src->some_csum_missing)
		dst->some_csum_missing = 1;
	if (dst->first_extent_gap > src->first_extent_gap)
		dst->first_extent_gap = src->first_extent_gap;

	BUG_ON(src->found_link < dir_count);
	dst->found_link += src->found_link - dir_count;
	dst->found_size += src->found_size;
	if (src->extent_start != (u64)-1) {
		if (dst->extent_start == (u64)-1) {
			dst->extent_start = src->extent_start;
			dst->extent_end = src->extent_end;
		} else {
			if (dst->extent_end > src->extent_start)
				dst->errors |= I_ERR_FILE_EXTENT_OVERLAP;
			else if (dst->extent_end < src->extent_start &&
				 dst->extent_end < dst->first_extent_gap)
				dst->first_extent_gap = dst->extent_end;
			if (dst->extent_end < src->extent_end)
				dst->extent_end = src->extent_end;
		}
	}

	dst->errors |= src->errors;
	if (src->found_inode_item) {
		if (!dst->found_inode_item) {
			dst->nlink = src->nlink;
			dst->isize = src->isize;
			dst->nbytes = src->nbytes;
			dst->imode = src->imode;
			dst->nodatasum = src->nodatasum;
			dst->found_inode_item = 1;
		} else {
			dst->errors |= I_ERR_DUP_INODE_ITEM;
		}
	}
	dst->merging = 0;

	return 0;
}

static int splice_shared_node(struct shared_node *src_node,
			      struct shared_node *dst_node)
{
	struct cache_extent *cache;
	struct ptr_node *node, *ins;
	struct cache_tree *src, *dst;
	struct inode_record *rec, *conflict;
	u64 current_ino = 0;
	int splice = 0;
	int ret;

	if (--src_node->refs == 0)
		splice = 1;
	if (src_node->current)
		current_ino = src_node->current->ino;

	src = &src_node->root_cache;
	dst = &dst_node->root_cache;
again:
	cache = find_first_cache_extent(src, 0);
	while (cache) {
		node = container_of(cache, struct ptr_node, cache);
		rec = node->data;
		cache = next_cache_extent(cache);

		if (splice) {
			remove_cache_extent(src, &node->cache);
			ins = node;
		} else {
			ins = malloc(sizeof(*ins));
			ins->cache.start = node->cache.start;
			ins->cache.size = node->cache.size;
			ins->data = rec;
			rec->refs++;
		}
		ret = insert_existing_cache_extent(dst, &ins->cache);
		if (ret == -EEXIST) {
			conflict = get_inode_rec(dst, rec->ino, 1);
			merge_inode_recs(rec, conflict, dst);
			if (rec->checked) {
				conflict->checked = 1;
				if (dst_node->current == conflict)
					dst_node->current = NULL;
			}
			maybe_free_inode_rec(dst, conflict);
			free_inode_rec(rec);
			free(ins);
		} else {
			BUG_ON(ret);
		}
	}

	if (src == &src_node->root_cache) {
		src = &src_node->inode_cache;
		dst = &dst_node->inode_cache;
		goto again;
	}

	if (current_ino > 0 && (!dst_node->current ||
	    current_ino > dst_node->current->ino)) {
		if (dst_node->current) {
			dst_node->current->checked = 1;
			maybe_free_inode_rec(dst, dst_node->current);
		}
		dst_node->current = get_inode_rec(dst, current_ino, 1);
	}
	return 0;
}

static void free_inode_recs(struct cache_tree *inode_cache)
{
	struct cache_extent *cache;
	struct ptr_node *node;
	struct inode_record *rec;

	while (1) {
		cache = find_first_cache_extent(inode_cache, 0);
		if (!cache)
			break;
		node = container_of(cache, struct ptr_node, cache);
		rec = node->data;
		remove_cache_extent(inode_cache, &node->cache);
		free(node);
		free_inode_rec(rec);
	}
}

static struct shared_node *find_shared_node(struct cache_tree *shared,
					    u64 bytenr)
{
	struct cache_extent *cache;
	struct shared_node *node;

	cache = find_cache_extent(shared, bytenr, 1);
	if (cache) {
		node = container_of(cache, struct shared_node, cache);
		return node;
	}
	return NULL;
}

static int add_shared_node(struct cache_tree *shared, u64 bytenr, u32 refs)
{
	int ret;
	struct shared_node *node;

	node = calloc(1, sizeof(*node));
	node->cache.start = bytenr;
	node->cache.size = 1;
	cache_tree_init(&node->root_cache);
	cache_tree_init(&node->inode_cache);
	node->refs = refs;

	ret = insert_existing_cache_extent(shared, &node->cache);
	BUG_ON(ret);
	return 0;
}

static int enter_shared_node(struct btrfs_root *root, u64 bytenr, u32 refs,
			     struct walk_control *wc, int level)
{
	struct shared_node *node;
	struct shared_node *dest;

	if (level == wc->active_node)
		return 0;

	BUG_ON(wc->active_node <= level);
	node = find_shared_node(&wc->shared, bytenr);
	if (!node) {
		add_shared_node(&wc->shared, bytenr, refs);
		node = find_shared_node(&wc->shared, bytenr);
		wc->nodes[level] = node;
		wc->active_node = level;
		return 0;
	}

	if (wc->root_level == wc->active_node &&
	    btrfs_root_refs(&root->root_item) == 0) {
		if (--node->refs == 0) {
			free_inode_recs(&node->root_cache);
			free_inode_recs(&node->inode_cache);
			remove_cache_extent(&wc->shared, &node->cache);
			free(node);
		}
		return 1;
	}

	dest = wc->nodes[wc->active_node];
	splice_shared_node(node, dest);
	if (node->refs == 0) {
		remove_cache_extent(&wc->shared, &node->cache);
		free(node);
	}
	return 1;
}

static int leave_shared_node(struct btrfs_root *root,
			     struct walk_control *wc, int level)
{
	struct shared_node *node;
	struct shared_node *dest;
	int i;

	if (level == wc->root_level)
		return 0;

	for (i = level + 1; i < BTRFS_MAX_LEVEL; i++) {
		if (wc->nodes[i])
			break;
	}
	BUG_ON(i >= BTRFS_MAX_LEVEL);

	node = wc->nodes[wc->active_node];
	wc->nodes[wc->active_node] = NULL;
	wc->active_node = i;

	dest = wc->nodes[wc->active_node];
	if (wc->active_node < wc->root_level ||
	    btrfs_root_refs(&root->root_item) > 0) {
		BUG_ON(node->refs <= 1);
		splice_shared_node(node, dest);
	} else {
		BUG_ON(node->refs < 2);
		node->refs--;
	}
	return 0;
}

static int is_child_root(struct btrfs_root *root, u64 parent_root_id,
			 u64 child_root_id)
{
	struct btrfs_path path;
	struct btrfs_key key;
	struct extent_buffer *leaf;
	int has_parent = 0;
	int ret;

	btrfs_init_path(&path);

	key.objectid = parent_root_id;
	key.type = BTRFS_ROOT_REF_KEY;
	key.offset = child_root_id;
	ret = btrfs_search_slot(NULL, root->fs_info->tree_root, &key, &path,
				0, 0);
	BUG_ON(ret < 0);
	btrfs_release_path(root, &path);
	if (!ret)
		return 1;

	key.objectid = child_root_id;
	key.type = BTRFS_ROOT_BACKREF_KEY;
	key.offset = 0;
	ret = btrfs_search_slot(NULL, root->fs_info->tree_root, &key, &path,
				0, 0);
	BUG_ON(ret <= 0);

	while (1) {
		leaf = path.nodes[0];
		if (path.slots[0] >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(root->fs_info->tree_root, &path);
			BUG_ON(ret < 0);

			if (ret > 0)
				break;
			leaf = path.nodes[0];
		}

		btrfs_item_key_to_cpu(leaf, &key, path.slots[0]);
		if (key.objectid != child_root_id ||
		    key.type != BTRFS_ROOT_BACKREF_KEY)
			break;

		has_parent = 1;

		if (key.offset == parent_root_id) {
			btrfs_release_path(root, &path);
			return 1;
		}

		path.slots[0]++;
	}

	btrfs_release_path(root, &path);
	return has_parent? 0 : -1;
}

static int process_dir_item(struct btrfs_root *root,
			    struct extent_buffer *eb,
			    int slot, struct btrfs_key *key,
			    struct shared_node *active_node)
{
	u32 total;
	u32 cur = 0;
	u32 len;
	u32 name_len;
	u32 data_len;
	int error;
	int nritems = 0;
	int filetype;
	struct btrfs_dir_item *di;
	struct inode_record *rec;
	struct cache_tree *root_cache;
	struct cache_tree *inode_cache;
	struct btrfs_key location;
	char namebuf[BTRFS_NAME_LEN];

	root_cache = &active_node->root_cache;
	inode_cache = &active_node->inode_cache;
	rec = active_node->current;
	rec->found_dir_item = 1;

	di = btrfs_item_ptr(eb, slot, struct btrfs_dir_item);
	total = btrfs_item_size_nr(eb, slot);
	while (cur < total) {
		nritems++;
		btrfs_dir_item_key_to_cpu(eb, di, &location);
		name_len = btrfs_dir_name_len(eb, di);
		data_len = btrfs_dir_data_len(eb, di);
		filetype = btrfs_dir_type(eb, di);

		rec->found_size += name_len;
		if (name_len <= BTRFS_NAME_LEN) {
			len = name_len;
			error = 0;
		} else {
			len = BTRFS_NAME_LEN;
			error = REF_ERR_NAME_TOO_LONG;
		}
		read_extent_buffer(eb, namebuf, (unsigned long)(di + 1), len);

		if (location.type == BTRFS_INODE_ITEM_KEY) {
			add_inode_backref(inode_cache, location.objectid,
					  key->objectid, key->offset, namebuf,
					  len, filetype, key->type, error);
		} else if (location.type == BTRFS_ROOT_ITEM_KEY) {
			add_inode_backref(root_cache, location.objectid,
					  key->objectid, key->offset,
					  namebuf, len, filetype,
					  key->type, error);
		} else {
			fprintf(stderr, "warning line %d\n", __LINE__);
		}

		len = sizeof(*di) + name_len + data_len;
		di = (struct btrfs_dir_item *)((char *)di + len);
		cur += len;
	}
	if (key->type == BTRFS_DIR_INDEX_KEY && nritems > 1)
		rec->errors |= I_ERR_DUP_DIR_INDEX;

	return 0;
}

static int process_inode_ref(struct extent_buffer *eb,
			     int slot, struct btrfs_key *key,
			     struct shared_node *active_node)
{
	u32 total;
	u32 cur = 0;
	u32 len;
	u32 name_len;
	u64 index;
	int error;
	struct cache_tree *inode_cache;
	struct btrfs_inode_ref *ref;
	char namebuf[BTRFS_NAME_LEN];

	inode_cache = &active_node->inode_cache;

	ref = btrfs_item_ptr(eb, slot, struct btrfs_inode_ref);
	total = btrfs_item_size_nr(eb, slot);
	while (cur < total) {
		name_len = btrfs_inode_ref_name_len(eb, ref);
		index = btrfs_inode_ref_index(eb, ref);
		if (name_len <= BTRFS_NAME_LEN) {
			len = name_len;
			error = 0;
		} else {
			len = BTRFS_NAME_LEN;
			error = REF_ERR_NAME_TOO_LONG;
		}
		read_extent_buffer(eb, namebuf, (unsigned long)(ref + 1), len);
		add_inode_backref(inode_cache, key->objectid, key->offset,
				  index, namebuf, len, 0, key->type, error);

		len = sizeof(*ref) + name_len;
		ref = (struct btrfs_inode_ref *)((char *)ref + len);
		cur += len;
	}
	return 0;
}

static int process_inode_extref(struct extent_buffer *eb,
				int slot, struct btrfs_key *key,
				struct shared_node *active_node)
{
	u32 total;
	u32 cur = 0;
	u32 len;
	u32 name_len;
	u64 index;
	u64 parent;
	int error;
	struct cache_tree *inode_cache;
	struct btrfs_inode_extref *extref;
	char namebuf[BTRFS_NAME_LEN];

	inode_cache = &active_node->inode_cache;

	extref = btrfs_item_ptr(eb, slot, struct btrfs_inode_extref);
	total = btrfs_item_size_nr(eb, slot);
	while (cur < total) {
		name_len = btrfs_inode_extref_name_len(eb, extref);
		index = btrfs_inode_extref_index(eb, extref);
		parent = btrfs_inode_extref_parent(eb, extref);
		if (name_len <= BTRFS_NAME_LEN) {
			len = name_len;
			error = 0;
		} else {
			len = BTRFS_NAME_LEN;
			error = REF_ERR_NAME_TOO_LONG;
		}
		read_extent_buffer(eb, namebuf,
				   (unsigned long)(extref + 1), len);
		add_inode_backref(inode_cache, key->objectid, parent,
				  index, namebuf, len, 0, key->type, error);

		len = sizeof(*extref) + name_len;
		extref = (struct btrfs_inode_extref *)((char *)extref + len);
		cur += len;
	}
	return 0;

}

static u64 count_csum_range(struct btrfs_root *root, u64 start, u64 len)
{
	struct btrfs_key key;
	struct btrfs_path path;
	struct extent_buffer *leaf;
	int ret ;
	size_t size;
	u64 found = 0;
	u64 csum_end;
	u16 csum_size = btrfs_super_csum_size(root->fs_info->super_copy);

	btrfs_init_path(&path);

	key.objectid = BTRFS_EXTENT_CSUM_OBJECTID;
	key.offset = start;
	key.type = BTRFS_EXTENT_CSUM_KEY;

	ret = btrfs_search_slot(NULL, root->fs_info->csum_root,
				&key, &path, 0, 0);
	BUG_ON(ret < 0);
	if (ret > 0 && path.slots[0] > 0) {
		leaf = path.nodes[0];
		btrfs_item_key_to_cpu(leaf, &key, path.slots[0] - 1);
		if (key.objectid == BTRFS_EXTENT_CSUM_OBJECTID &&
		    key.type == BTRFS_EXTENT_CSUM_KEY)
			path.slots[0]--;
	}

	while (len > 0) {
		leaf = path.nodes[0];
		if (path.slots[0] >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(root->fs_info->csum_root, &path);
			BUG_ON(ret < 0);
			if (ret > 0)
				break;
			leaf = path.nodes[0];
		}

		btrfs_item_key_to_cpu(leaf, &key, path.slots[0]);
		if (key.objectid != BTRFS_EXTENT_CSUM_OBJECTID ||
		    key.type != BTRFS_EXTENT_CSUM_KEY)
			break;

		btrfs_item_key_to_cpu(leaf, &key, path.slots[0]);
		if (key.offset >= start + len)
			break;

		if (key.offset > start)
			start = key.offset;

		size = btrfs_item_size_nr(leaf, path.slots[0]);
		csum_end = key.offset + (size / csum_size) * root->sectorsize;
		if (csum_end > start) {
			size = min(csum_end - start, len);
			len -= size;
			start += size;
			found += size;
		}

		path.slots[0]++;
	}
	btrfs_release_path(root->fs_info->csum_root, &path);
	return found;
}

static int process_file_extent(struct btrfs_root *root,
				struct extent_buffer *eb,
				int slot, struct btrfs_key *key,
				struct shared_node *active_node)
{
	struct inode_record *rec;
	struct btrfs_file_extent_item *fi;
	u64 num_bytes = 0;
	u64 disk_bytenr = 0;
	u64 extent_offset = 0;
	u64 mask = root->sectorsize - 1;
	int extent_type;

	rec = active_node->current;
	BUG_ON(rec->ino != key->objectid || rec->refs > 1);
	rec->found_file_extent = 1;

	if (rec->extent_start == (u64)-1) {
		rec->extent_start = key->offset;
		rec->extent_end = key->offset;
	}

	if (rec->extent_end > key->offset)
		rec->errors |= I_ERR_FILE_EXTENT_OVERLAP;
	else if (rec->extent_end < key->offset &&
		 rec->extent_end < rec->first_extent_gap)
		rec->first_extent_gap = rec->extent_end;

	fi = btrfs_item_ptr(eb, slot, struct btrfs_file_extent_item);
	extent_type = btrfs_file_extent_type(eb, fi);

	if (extent_type == BTRFS_FILE_EXTENT_INLINE) {
		num_bytes = btrfs_file_extent_inline_len(eb, fi);
		if (num_bytes == 0)
			rec->errors |= I_ERR_BAD_FILE_EXTENT;
		rec->found_size += num_bytes;
		num_bytes = (num_bytes + mask) & ~mask;
	} else if (extent_type == BTRFS_FILE_EXTENT_REG ||
		   extent_type == BTRFS_FILE_EXTENT_PREALLOC) {
		num_bytes = btrfs_file_extent_num_bytes(eb, fi);
		disk_bytenr = btrfs_file_extent_disk_bytenr(eb, fi);
		extent_offset = btrfs_file_extent_offset(eb, fi);
		if (num_bytes == 0 || (num_bytes & mask))
			rec->errors |= I_ERR_BAD_FILE_EXTENT;
		if (num_bytes + extent_offset >
		    btrfs_file_extent_ram_bytes(eb, fi))
			rec->errors |= I_ERR_BAD_FILE_EXTENT;
		if (extent_type == BTRFS_FILE_EXTENT_PREALLOC &&
		    (btrfs_file_extent_compression(eb, fi) ||
		     btrfs_file_extent_encryption(eb, fi) ||
		     btrfs_file_extent_other_encoding(eb, fi)))
			rec->errors |= I_ERR_BAD_FILE_EXTENT;
		if (disk_bytenr > 0)
			rec->found_size += num_bytes;
	} else {
		rec->errors |= I_ERR_BAD_FILE_EXTENT;
	}
	rec->extent_end = key->offset + num_bytes;

	if (disk_bytenr > 0) {
		u64 found;
		if (btrfs_file_extent_compression(eb, fi))
			num_bytes = btrfs_file_extent_disk_num_bytes(eb, fi);
		else
			disk_bytenr += extent_offset;

		found = count_csum_range(root, disk_bytenr, num_bytes);
		if (extent_type == BTRFS_FILE_EXTENT_REG) {
			if (found > 0)
				rec->found_csum_item = 1;
			if (found < num_bytes)
				rec->some_csum_missing = 1;
		} else if (extent_type == BTRFS_FILE_EXTENT_PREALLOC) {
			if (found > 0)
				rec->errors |= I_ERR_ODD_CSUM_ITEM;
		}
	}
	return 0;
}

static int process_one_leaf(struct btrfs_root *root, struct extent_buffer *eb,
			    struct walk_control *wc)
{
	struct btrfs_key key;
	u32 nritems;
	int i;
	int ret = 0;
	struct cache_tree *inode_cache;
	struct shared_node *active_node;

	if (wc->root_level == wc->active_node &&
	    btrfs_root_refs(&root->root_item) == 0)
		return 0;

	active_node = wc->nodes[wc->active_node];
	inode_cache = &active_node->inode_cache;
	nritems = btrfs_header_nritems(eb);
	for (i = 0; i < nritems; i++) {
		btrfs_item_key_to_cpu(eb, &key, i);

		if (key.objectid == BTRFS_FREE_SPACE_OBJECTID)
			continue;

		if (active_node->current == NULL ||
		    active_node->current->ino < key.objectid) {
			if (active_node->current) {
				active_node->current->checked = 1;
				maybe_free_inode_rec(inode_cache,
						     active_node->current);
			}
			active_node->current = get_inode_rec(inode_cache,
							     key.objectid, 1);
		}
		switch (key.type) {
		case BTRFS_DIR_ITEM_KEY:
		case BTRFS_DIR_INDEX_KEY:
			ret = process_dir_item(root, eb, i, &key, active_node);
			break;
		case BTRFS_INODE_REF_KEY:
			ret = process_inode_ref(eb, i, &key, active_node);
			break;
		case BTRFS_INODE_EXTREF_KEY:
			ret = process_inode_extref(eb, i, &key, active_node);
			break;
		case BTRFS_INODE_ITEM_KEY:
			ret = process_inode_item(eb, i, &key, active_node);
			break;
		case BTRFS_EXTENT_DATA_KEY:
			ret = process_file_extent(root, eb, i, &key,
						  active_node);
			break;
		default:
			break;
		};
	}
	return ret;
}

static void reada_walk_down(struct btrfs_root *root,
			    struct extent_buffer *node, int slot)
{
	u64 bytenr;
	u64 ptr_gen;
	u32 nritems;
	u32 blocksize;
	int i;
	int ret;
	int level;

	level = btrfs_header_level(node);
	if (level != 1)
		return;

	nritems = btrfs_header_nritems(node);
	blocksize = btrfs_level_size(root, level - 1);
	for (i = slot; i < nritems; i++) {
		bytenr = btrfs_node_blockptr(node, i);
		ptr_gen = btrfs_node_ptr_generation(node, i);
		ret = readahead_tree_block(root, bytenr, blocksize, ptr_gen);
		if (ret)
			break;
	}
}

static int walk_down_tree(struct btrfs_root *root, struct btrfs_path *path,
			  struct walk_control *wc, int *level)
{
	u64 bytenr;
	u64 ptr_gen;
	struct extent_buffer *next;
	struct extent_buffer *cur;
	u32 blocksize;
	int ret;
	u64 refs;

	WARN_ON(*level < 0);
	WARN_ON(*level >= BTRFS_MAX_LEVEL);
	ret = btrfs_lookup_extent_info(NULL, root,
				       path->nodes[*level]->start,
				       *level, 1, &refs, NULL);
	if (ret < 0)
		goto out;

	if (refs > 1) {
		ret = enter_shared_node(root, path->nodes[*level]->start,
					refs, wc, *level);
		if (ret > 0)
			goto out;
	}

	while (*level >= 0) {
		WARN_ON(*level < 0);
		WARN_ON(*level >= BTRFS_MAX_LEVEL);
		cur = path->nodes[*level];

		if (btrfs_header_level(cur) != *level)
			WARN_ON(1);

		if (path->slots[*level] >= btrfs_header_nritems(cur))
			break;
		if (*level == 0) {
			ret = process_one_leaf(root, cur, wc);
			break;
		}
		bytenr = btrfs_node_blockptr(cur, path->slots[*level]);
		ptr_gen = btrfs_node_ptr_generation(cur, path->slots[*level]);
		blocksize = btrfs_level_size(root, *level - 1);
		ret = btrfs_lookup_extent_info(NULL, root, bytenr, *level - 1,
					       1, &refs, NULL);
		if (ret < 0)
			refs = 0;

		if (refs > 1) {
			ret = enter_shared_node(root, bytenr, refs,
						wc, *level - 1);
			if (ret > 0) {
				path->slots[*level]++;
				continue;
			}
		}

		next = btrfs_find_tree_block(root, bytenr, blocksize);
		if (!next || !btrfs_buffer_uptodate(next, ptr_gen)) {
			free_extent_buffer(next);
			reada_walk_down(root, cur, path->slots[*level]);
			next = read_tree_block(root, bytenr, blocksize,
					       ptr_gen);
		}

		*level = *level - 1;
		free_extent_buffer(path->nodes[*level]);
		path->nodes[*level] = next;
		path->slots[*level] = 0;
	}
out:
	path->slots[*level] = btrfs_header_nritems(path->nodes[*level]);
	return 0;
}

static int walk_up_tree(struct btrfs_root *root, struct btrfs_path *path,
			struct walk_control *wc, int *level)
{
	int i;
	struct extent_buffer *leaf;

	for (i = *level; i < BTRFS_MAX_LEVEL - 1 && path->nodes[i]; i++) {
		leaf = path->nodes[i];
		if (path->slots[i] + 1 < btrfs_header_nritems(leaf)) {
			path->slots[i]++;
			*level = i;
			return 0;
		} else {
			free_extent_buffer(path->nodes[*level]);
			path->nodes[*level] = NULL;
			BUG_ON(*level > wc->active_node);
			if (*level == wc->active_node)
				leave_shared_node(root, wc, *level);
			*level = i + 1;
		}
	}
	return 1;
}

static int check_root_dir(struct inode_record *rec)
{
	struct inode_backref *backref;
	int ret = -1;

	if (!rec->found_inode_item || rec->errors)
		goto out;
	if (rec->nlink != 1 || rec->found_link != 0)
		goto out;
	if (list_empty(&rec->backrefs))
		goto out;
	backref = list_entry(rec->backrefs.next, struct inode_backref, list);
	if (!backref->found_inode_ref)
		goto out;
	if (backref->index != 0 || backref->namelen != 2 ||
	    memcmp(backref->name, "..", 2))
		goto out;
	if (backref->found_dir_index || backref->found_dir_item)
		goto out;
	ret = 0;
out:
	return ret;
}

static int check_inode_recs(struct btrfs_root *root,
			    struct cache_tree *inode_cache)
{
	struct cache_extent *cache;
	struct ptr_node *node;
	struct inode_record *rec;
	struct inode_backref *backref;
	int ret;
	u64 error = 0;
	u64 root_dirid = btrfs_root_dirid(&root->root_item);

	if (btrfs_root_refs(&root->root_item) == 0) {
		if (!cache_tree_empty(inode_cache))
			fprintf(stderr, "warning line %d\n", __LINE__);
		return 0;
	}

	rec = get_inode_rec(inode_cache, root_dirid, 0);
	if (rec) {
		ret = check_root_dir(rec);
		if (ret) {
			fprintf(stderr, "root %llu root dir %llu error\n",
				(unsigned long long)root->root_key.objectid,
				(unsigned long long)root_dirid);
			error++;
		}
	} else {
		fprintf(stderr, "root %llu root dir %llu not found\n",
			(unsigned long long)root->root_key.objectid,
			(unsigned long long)root_dirid);
	}

	while (1) {
		cache = find_first_cache_extent(inode_cache, 0);
		if (!cache)
			break;
		node = container_of(cache, struct ptr_node, cache);
		rec = node->data;
		remove_cache_extent(inode_cache, &node->cache);
		free(node);
		if (rec->ino == root_dirid ||
		    rec->ino == BTRFS_ORPHAN_OBJECTID) {
			free_inode_rec(rec);
			continue;
		}

		if (rec->errors & I_ERR_NO_ORPHAN_ITEM) {
			ret = check_orphan_item(root, rec->ino);
			if (ret == 0)
				rec->errors &= ~I_ERR_NO_ORPHAN_ITEM;
			if (can_free_inode_rec(rec)) {
				free_inode_rec(rec);
				continue;
			}
		}

		error++;
		if (!rec->found_inode_item)
			rec->errors |= I_ERR_NO_INODE_ITEM;
		if (rec->found_link != rec->nlink)
			rec->errors |= I_ERR_LINK_COUNT_WRONG;
		fprintf(stderr, "root %llu inode %llu errors %x\n",
			(unsigned long long) root->root_key.objectid,
			(unsigned long long) rec->ino, rec->errors);
		list_for_each_entry(backref, &rec->backrefs, list) {
			if (!backref->found_dir_item)
				backref->errors |= REF_ERR_NO_DIR_ITEM;
			if (!backref->found_dir_index)
				backref->errors |= REF_ERR_NO_DIR_INDEX;
			if (!backref->found_inode_ref)
				backref->errors |= REF_ERR_NO_INODE_REF;
			fprintf(stderr, "\tunresolved ref dir %llu index %llu"
				" namelen %u name %s filetype %d error %x\n",
				(unsigned long long)backref->dir,
				(unsigned long long)backref->index,
				backref->namelen, backref->name,
				backref->filetype, backref->errors);
		}
		free_inode_rec(rec);
	}
	return (error > 0) ? -1 : 0;
}

static struct root_record *get_root_rec(struct cache_tree *root_cache,
					u64 objectid)
{
	struct cache_extent *cache;
	struct root_record *rec = NULL;
	int ret;

	cache = find_cache_extent(root_cache, objectid, 1);
	if (cache) {
		rec = container_of(cache, struct root_record, cache);
	} else {
		rec = calloc(1, sizeof(*rec));
		rec->objectid = objectid;
		INIT_LIST_HEAD(&rec->backrefs);
		rec->cache.start = objectid;
		rec->cache.size = 1;

		ret = insert_existing_cache_extent(root_cache, &rec->cache);
		BUG_ON(ret);
	}
	return rec;
}

static struct root_backref *get_root_backref(struct root_record *rec,
					     u64 ref_root, u64 dir, u64 index,
					     const char *name, int namelen)
{
	struct root_backref *backref;

	list_for_each_entry(backref, &rec->backrefs, list) {
		if (backref->ref_root != ref_root || backref->dir != dir ||
		    backref->namelen != namelen)
			continue;
		if (memcmp(name, backref->name, namelen))
			continue;
		return backref;
	}

	backref = malloc(sizeof(*backref) + namelen + 1);
	memset(backref, 0, sizeof(*backref));
	backref->ref_root = ref_root;
	backref->dir = dir;
	backref->index = index;
	backref->namelen = namelen;
	memcpy(backref->name, name, namelen);
	backref->name[namelen] = '\0';
	list_add_tail(&backref->list, &rec->backrefs);
	return backref;
}

static void free_root_recs(struct cache_tree *root_cache)
{
	struct cache_extent *cache;
	struct root_record *rec;
	struct root_backref *backref;

	while (1) {
		cache = find_first_cache_extent(root_cache, 0);
		if (!cache)
			break;
		rec = container_of(cache, struct root_record, cache);
		remove_cache_extent(root_cache, &rec->cache);

		while (!list_empty(&rec->backrefs)) {
			backref = list_entry(rec->backrefs.next,
					     struct root_backref, list);
			list_del(&backref->list);
			free(backref);
		}
		kfree(rec);
	}
}

static int add_root_backref(struct cache_tree *root_cache,
			    u64 root_id, u64 ref_root, u64 dir, u64 index,
			    const char *name, int namelen,
			    int item_type, int errors)
{
	struct root_record *rec;
	struct root_backref *backref;

	rec = get_root_rec(root_cache, root_id);
	backref = get_root_backref(rec, ref_root, dir, index, name, namelen);

	backref->errors |= errors;

	if (item_type != BTRFS_DIR_ITEM_KEY) {
		if (backref->found_dir_index || backref->found_back_ref ||
		    backref->found_forward_ref) {
			if (backref->index != index)
				backref->errors |= REF_ERR_INDEX_UNMATCH;
		} else {
			backref->index = index;
		}
	}

	if (item_type == BTRFS_DIR_ITEM_KEY) {
		backref->found_dir_item = 1;
		backref->reachable = 1;
		rec->found_ref++;
	} else if (item_type == BTRFS_DIR_INDEX_KEY) {
		backref->found_dir_index = 1;
	} else if (item_type == BTRFS_ROOT_REF_KEY) {
		if (backref->found_forward_ref)
			backref->errors |= REF_ERR_DUP_ROOT_REF;
		backref->found_forward_ref = 1;
	} else if (item_type == BTRFS_ROOT_BACKREF_KEY) {
		if (backref->found_back_ref)
			backref->errors |= REF_ERR_DUP_ROOT_BACKREF;
		backref->found_back_ref = 1;
	} else {
		BUG_ON(1);
	}

	return 0;
}

static int merge_root_recs(struct btrfs_root *root,
			   struct cache_tree *src_cache,
			   struct cache_tree *dst_cache)
{
	struct cache_extent *cache;
	struct ptr_node *node;
	struct inode_record *rec;
	struct inode_backref *backref;

	if (root->root_key.objectid == BTRFS_TREE_RELOC_OBJECTID) {
		free_inode_recs(src_cache);
		return 0;
	}

	while (1) {
		cache = find_first_cache_extent(src_cache, 0);
		if (!cache)
			break;
		node = container_of(cache, struct ptr_node, cache);
		rec = node->data;
		remove_cache_extent(src_cache, &node->cache);
		free(node);

		if (!is_child_root(root, root->objectid, rec->ino))
			goto skip;

		list_for_each_entry(backref, &rec->backrefs, list) {
			BUG_ON(backref->found_inode_ref);
			if (backref->found_dir_item)
				add_root_backref(dst_cache, rec->ino,
					root->root_key.objectid, backref->dir,
					backref->index, backref->name,
					backref->namelen, BTRFS_DIR_ITEM_KEY,
					backref->errors);
			if (backref->found_dir_index)
				add_root_backref(dst_cache, rec->ino,
					root->root_key.objectid, backref->dir,
					backref->index, backref->name,
					backref->namelen, BTRFS_DIR_INDEX_KEY,
					backref->errors);
		}
skip:
		free_inode_rec(rec);
	}
	return 0;
}

static int check_root_refs(struct btrfs_root *root,
			   struct cache_tree *root_cache)
{
	struct root_record *rec;
	struct root_record *ref_root;
	struct root_backref *backref;
	struct cache_extent *cache;
	int loop = 1;
	int ret;
	int error;
	int errors = 0;

	rec = get_root_rec(root_cache, BTRFS_FS_TREE_OBJECTID);
	rec->found_ref = 1;

	/* fixme: this can not detect circular references */
	while (loop) {
		loop = 0;
		cache = find_first_cache_extent(root_cache, 0);
		while (1) {
			if (!cache)
				break;
			rec = container_of(cache, struct root_record, cache);
			cache = next_cache_extent(cache);

			if (rec->found_ref == 0)
				continue;

			list_for_each_entry(backref, &rec->backrefs, list) {
				if (!backref->reachable)
					continue;

				ref_root = get_root_rec(root_cache,
							backref->ref_root);
				if (ref_root->found_ref > 0)
					continue;

				backref->reachable = 0;
				rec->found_ref--;
				if (rec->found_ref == 0)
					loop = 1;
			}
		}
	}

	cache = find_first_cache_extent(root_cache, 0);
	while (1) {
		if (!cache)
			break;
		rec = container_of(cache, struct root_record, cache);
		cache = next_cache_extent(cache);

		if (rec->found_ref == 0 &&
		    rec->objectid >= BTRFS_FIRST_FREE_OBJECTID &&
		    rec->objectid <= BTRFS_LAST_FREE_OBJECTID) {
			ret = check_orphan_item(root->fs_info->tree_root,
						rec->objectid);
			if (ret == 0)
				continue;
			errors++;
			fprintf(stderr, "fs tree %llu not referenced\n",
				(unsigned long long)rec->objectid);
		}

		error = 0;
		if (rec->found_ref > 0 && !rec->found_root_item)
			error = 1;
		list_for_each_entry(backref, &rec->backrefs, list) {
			if (!backref->found_dir_item)
				backref->errors |= REF_ERR_NO_DIR_ITEM;
			if (!backref->found_dir_index)
				backref->errors |= REF_ERR_NO_DIR_INDEX;
			if (!backref->found_back_ref)
				backref->errors |= REF_ERR_NO_ROOT_BACKREF;
			if (!backref->found_forward_ref)
				backref->errors |= REF_ERR_NO_ROOT_REF;
			if (backref->reachable && backref->errors)
				error = 1;
		}
		if (!error)
			continue;

		errors++;
		fprintf(stderr, "fs tree %llu refs %u %s\n",
			(unsigned long long)rec->objectid, rec->found_ref,
			 rec->found_root_item ? "" : "not found");

		list_for_each_entry(backref, &rec->backrefs, list) {
			if (!backref->reachable)
				continue;
			if (!backref->errors && rec->found_root_item)
				continue;
			fprintf(stderr, "\tunresolved ref root %llu dir %llu"
				" index %llu namelen %u name %s error %x\n",
				(unsigned long long)backref->ref_root,
				(unsigned long long)backref->dir,
				(unsigned long long)backref->index,
				backref->namelen, backref->name,
				backref->errors);
		}
	}
	return errors > 0 ? 1 : 0;
}

static int process_root_ref(struct extent_buffer *eb, int slot,
			    struct btrfs_key *key,
			    struct cache_tree *root_cache)
{
	u64 dirid;
	u64 index;
	u32 len;
	u32 name_len;
	struct btrfs_root_ref *ref;
	char namebuf[BTRFS_NAME_LEN];
	int error;

	ref = btrfs_item_ptr(eb, slot, struct btrfs_root_ref);

	dirid = btrfs_root_ref_dirid(eb, ref);
	index = btrfs_root_ref_sequence(eb, ref);
	name_len = btrfs_root_ref_name_len(eb, ref);

	if (name_len <= BTRFS_NAME_LEN) {
		len = name_len;
		error = 0;
	} else {
		len = BTRFS_NAME_LEN;
		error = REF_ERR_NAME_TOO_LONG;
	}
	read_extent_buffer(eb, namebuf, (unsigned long)(ref + 1), len);

	if (key->type == BTRFS_ROOT_REF_KEY) {
		add_root_backref(root_cache, key->offset, key->objectid, dirid,
				 index, namebuf, len, key->type, error);
	} else {
		add_root_backref(root_cache, key->objectid, key->offset, dirid,
				 index, namebuf, len, key->type, error);
	}
	return 0;
}

static int check_fs_root(struct btrfs_root *root,
			 struct cache_tree *root_cache,
			 struct walk_control *wc)
{
	int ret = 0;
	int wret;
	int level;
	struct btrfs_path path;
	struct shared_node root_node;
	struct root_record *rec;
	struct btrfs_root_item *root_item = &root->root_item;

	if (root->root_key.objectid != BTRFS_TREE_RELOC_OBJECTID) {
		rec = get_root_rec(root_cache, root->root_key.objectid);
		if (btrfs_root_refs(root_item) > 0)
			rec->found_root_item = 1;
	}

	btrfs_init_path(&path);
	memset(&root_node, 0, sizeof(root_node));
	cache_tree_init(&root_node.root_cache);
	cache_tree_init(&root_node.inode_cache);

	level = btrfs_header_level(root->node);
	memset(wc->nodes, 0, sizeof(wc->nodes));
	wc->nodes[level] = &root_node;
	wc->active_node = level;
	wc->root_level = level;

	if (btrfs_root_refs(root_item) > 0 ||
	    btrfs_disk_key_objectid(&root_item->drop_progress) == 0) {
		path.nodes[level] = root->node;
		extent_buffer_get(root->node);
		path.slots[level] = 0;
	} else {
		struct btrfs_key key;
		struct btrfs_disk_key found_key;

		btrfs_disk_key_to_cpu(&key, &root_item->drop_progress);
		level = root_item->drop_level;
		path.lowest_level = level;
		wret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
		BUG_ON(wret < 0);
		btrfs_node_key(path.nodes[level], &found_key,
				path.slots[level]);
		WARN_ON(memcmp(&found_key, &root_item->drop_progress,
					sizeof(found_key)));
	}

	while (1) {
		wret = walk_down_tree(root, &path, wc, &level);
		if (wret < 0)
			ret = wret;
		if (wret != 0)
			break;

		wret = walk_up_tree(root, &path, wc, &level);
		if (wret < 0)
			ret = wret;
		if (wret != 0)
			break;
	}
	btrfs_release_path(root, &path);

	merge_root_recs(root, &root_node.root_cache, root_cache);

	if (root_node.current) {
		root_node.current->checked = 1;
		maybe_free_inode_rec(&root_node.inode_cache,
				root_node.current);
	}

	ret = check_inode_recs(root, &root_node.inode_cache);
	return ret;
}

static int fs_root_objectid(u64 objectid)
{
	if (objectid == BTRFS_FS_TREE_OBJECTID ||
	    objectid == BTRFS_TREE_RELOC_OBJECTID ||
	    objectid == BTRFS_DATA_RELOC_TREE_OBJECTID ||
	    (objectid >= BTRFS_FIRST_FREE_OBJECTID &&
	     objectid <= BTRFS_LAST_FREE_OBJECTID))
		return 1;
	return 0;
}

static int check_fs_roots(struct btrfs_root *root,
			  struct cache_tree *root_cache)
{
	struct btrfs_path path;
	struct btrfs_key key;
	struct walk_control wc;
	struct extent_buffer *leaf;
	struct btrfs_root *tmp_root;
	struct btrfs_root *tree_root = root->fs_info->tree_root;
	int ret;
	int err = 0;

	memset(&wc, 0, sizeof(wc));
	cache_tree_init(&wc.shared);
	btrfs_init_path(&path);

	key.offset = 0;
	key.objectid = 0;
	key.type = BTRFS_ROOT_ITEM_KEY;
	ret = btrfs_search_slot(NULL, tree_root, &key, &path, 0, 0);
	BUG_ON(ret < 0);
	while (1) {
		leaf = path.nodes[0];
		if (path.slots[0] >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(tree_root, &path);
			if (ret != 0)
				break;
			leaf = path.nodes[0];
		}
		btrfs_item_key_to_cpu(leaf, &key, path.slots[0]);
		if (key.type == BTRFS_ROOT_ITEM_KEY &&
		    fs_root_objectid(key.objectid)) {
			tmp_root = btrfs_read_fs_root_no_cache(root->fs_info,
							       &key);
			if (IS_ERR(tmp_root)) {
				err = 1;
				goto next;
			}
			ret = check_fs_root(tmp_root, root_cache, &wc);
			if (ret)
				err = 1;
			btrfs_free_fs_root(root->fs_info, tmp_root);
		} else if (key.type == BTRFS_ROOT_REF_KEY ||
			   key.type == BTRFS_ROOT_BACKREF_KEY) {
			process_root_ref(leaf, path.slots[0], &key,
					 root_cache);
		}
next:
		path.slots[0]++;
	}
	btrfs_release_path(tree_root, &path);

	if (!cache_tree_empty(&wc.shared))
		fprintf(stderr, "warning line %d\n", __LINE__);

	return err;
}

static int all_backpointers_checked(struct extent_record *rec, int print_errs)
{
	struct list_head *cur = rec->backrefs.next;
	struct extent_backref *back;
	struct tree_backref *tback;
	struct data_backref *dback;
	u64 found = 0;
	int err = 0;

	while(cur != &rec->backrefs) {
		back = list_entry(cur, struct extent_backref, list);
		cur = cur->next;
		if (!back->found_extent_tree) {
			err = 1;
			if (!print_errs)
				goto out;
			if (back->is_data) {
				dback = (struct data_backref *)back;
				fprintf(stderr, "Backref %llu %s %llu"
					" owner %llu offset %llu num_refs %lu"
					" not found in extent tree\n",
					(unsigned long long)rec->start,
					back->full_backref ?
					"parent" : "root",
					back->full_backref ?
					(unsigned long long)dback->parent:
					(unsigned long long)dback->root,
					(unsigned long long)dback->owner,
					(unsigned long long)dback->offset,
					(unsigned long)dback->num_refs);
			} else {
				tback = (struct tree_backref *)back;
				fprintf(stderr, "Backref %llu parent %llu"
					" root %llu not found in extent tree\n",
					(unsigned long long)rec->start,
					(unsigned long long)tback->parent,
					(unsigned long long)tback->root);
			}
		}
		if (!back->is_data && !back->found_ref) {
			err = 1;
			if (!print_errs)
				goto out;
			tback = (struct tree_backref *)back;
			fprintf(stderr, "Backref %llu %s %llu not referenced back %p\n",
				(unsigned long long)rec->start,
				back->full_backref ? "parent" : "root",
				back->full_backref ?
				(unsigned long long)tback->parent :
				(unsigned long long)tback->root, back);
		}
		if (back->is_data) {
			dback = (struct data_backref *)back;
			if (dback->found_ref != dback->num_refs) {
				err = 1;
				if (!print_errs)
					goto out;
				fprintf(stderr, "Incorrect local backref count"
					" on %llu %s %llu owner %llu"
					" offset %llu found %u wanted %u back %p\n",
					(unsigned long long)rec->start,
					back->full_backref ?
					"parent" : "root",
					back->full_backref ? 
					(unsigned long long)dback->parent:
					(unsigned long long)dback->root,
					(unsigned long long)dback->owner,
					(unsigned long long)dback->offset,
					dback->found_ref, dback->num_refs, back);
			}
			if (dback->bytes != rec->nr) {
				err = 1;
				if (!print_errs)
					goto out;
				fprintf(stderr, "Backref bytes do not match "
					"extent backref, bytenr=%llu, ref "
					"bytes=%llu, backref bytes=%llu\n",
					(unsigned long long)rec->start,
					(unsigned long long)rec->nr,
					(unsigned long long)dback->bytes);
			}
		}
		if (!back->is_data) {
			found += 1;
		} else {
			dback = (struct data_backref *)back;
			found += dback->found_ref;
		}
	}
	if (found != rec->refs) {
		err = 1;
		if (!print_errs)
			goto out;
		fprintf(stderr, "Incorrect global backref count "
			"on %llu found %llu wanted %llu\n",
			(unsigned long long)rec->start,
			(unsigned long long)found,
			(unsigned long long)rec->refs);
	}
out:
	return err;
}

static int free_all_extent_backrefs(struct extent_record *rec)
{
	struct extent_backref *back;
	struct list_head *cur;
	while (!list_empty(&rec->backrefs)) {
		cur = rec->backrefs.next;
		back = list_entry(cur, struct extent_backref, list);
		list_del(cur);
		free(back);
	}
	return 0;
}

static int maybe_free_extent_rec(struct cache_tree *extent_cache,
				 struct extent_record *rec)
{
	if (rec->content_checked && rec->owner_ref_checked &&
	    rec->extent_item_refs == rec->refs && rec->refs > 0 &&
	    !all_backpointers_checked(rec, 0)) {
		remove_cache_extent(extent_cache, &rec->cache);
		free_all_extent_backrefs(rec);
		free(rec);
	}
	return 0;
}

static int check_owner_ref(struct btrfs_root *root,
			    struct extent_record *rec,
			    struct extent_buffer *buf)
{
	struct extent_backref *node;
	struct tree_backref *back;
	struct btrfs_root *ref_root;
	struct btrfs_key key;
	struct btrfs_path path;
	struct extent_buffer *parent;
	int level;
	int found = 0;
	int ret;

	list_for_each_entry(node, &rec->backrefs, list) {
		if (node->is_data)
			continue;
		if (!node->found_ref)
			continue;
		if (node->full_backref)
			continue;
		back = (struct tree_backref *)node;
		if (btrfs_header_owner(buf) == back->root)
			return 0;
	}
	BUG_ON(rec->is_root);

	/* try to find the block by search corresponding fs tree */
	key.objectid = btrfs_header_owner(buf);
	key.type = BTRFS_ROOT_ITEM_KEY;
	key.offset = (u64)-1;

	ref_root = btrfs_read_fs_root(root->fs_info, &key);
	if (IS_ERR(ref_root))
		return 1;

	level = btrfs_header_level(buf);
	if (level == 0)
		btrfs_item_key_to_cpu(buf, &key, 0);
	else
		btrfs_node_key_to_cpu(buf, &key, 0);

	btrfs_init_path(&path);
	path.lowest_level = level + 1;
	ret = btrfs_search_slot(NULL, ref_root, &key, &path, 0, 0);
	if (ret < 0)
		return 0;

	parent = path.nodes[level + 1];
	if (parent && buf->start == btrfs_node_blockptr(parent,
							path.slots[level + 1]))
		found = 1;

	btrfs_release_path(ref_root, &path);
	return found ? 0 : 1;
}

static int is_extent_tree_record(struct extent_record *rec)
{
	struct list_head *cur = rec->backrefs.next;
	struct extent_backref *node;
	struct tree_backref *back;
	int is_extent = 0;

	while(cur != &rec->backrefs) {
		node = list_entry(cur, struct extent_backref, list);
		cur = cur->next;
		if (node->is_data)
			return 0;
		back = (struct tree_backref *)node;
		if (node->full_backref)
			return 0;
		if (back->root == BTRFS_EXTENT_TREE_OBJECTID)
			is_extent = 1;
	}
	return is_extent;
}


static int record_bad_block_io(struct btrfs_fs_info *info,
			       struct cache_tree *extent_cache,
			       u64 start, u64 len)
{
	struct extent_record *rec;
	struct cache_extent *cache;
	struct btrfs_key key;

	cache = find_cache_extent(extent_cache, start, len);
	if (!cache)
		return 0;

	rec = container_of(cache, struct extent_record, cache);
	if (!is_extent_tree_record(rec))
		return 0;

	btrfs_disk_key_to_cpu(&key, &rec->parent_key);
	return btrfs_add_corrupt_extent_record(info, &key, start, len, 0);
}

static int check_block(struct btrfs_root *root,
		       struct cache_tree *extent_cache,
		       struct extent_buffer *buf, u64 flags)
{
	struct extent_record *rec;
	struct cache_extent *cache;
	struct btrfs_key key;
	int ret = 1;
	int level;

	cache = find_cache_extent(extent_cache, buf->start, buf->len);
	if (!cache)
		return 1;
	rec = container_of(cache, struct extent_record, cache);
	rec->generation = btrfs_header_generation(buf);

	level = btrfs_header_level(buf);
	if (btrfs_header_nritems(buf) > 0) {

		if (level == 0)
			btrfs_item_key_to_cpu(buf, &key, 0);
		else
			btrfs_node_key_to_cpu(buf, &key, 0);

		rec->info_objectid = key.objectid;
	}
	rec->info_level = level;

	if (btrfs_is_leaf(buf))
		ret = btrfs_check_leaf(root, &rec->parent_key, buf);
	else
		ret = btrfs_check_node(root, &rec->parent_key, buf);

	if (ret) {
		fprintf(stderr, "bad block %llu\n",
			(unsigned long long)buf->start);
	} else {
		rec->content_checked = 1;
		if (flags & BTRFS_BLOCK_FLAG_FULL_BACKREF)
			rec->owner_ref_checked = 1;
		else {
			ret = check_owner_ref(root, rec, buf);
			if (!ret)
				rec->owner_ref_checked = 1;
		}
	}
	if (!ret)
		maybe_free_extent_rec(extent_cache, rec);
	return ret;
}

static struct tree_backref *find_tree_backref(struct extent_record *rec,
						u64 parent, u64 root)
{
	struct list_head *cur = rec->backrefs.next;
	struct extent_backref *node;
	struct tree_backref *back;

	while(cur != &rec->backrefs) {
		node = list_entry(cur, struct extent_backref, list);
		cur = cur->next;
		if (node->is_data)
			continue;
		back = (struct tree_backref *)node;
		if (parent > 0) {
			if (!node->full_backref)
				continue;
			if (parent == back->parent)
				return back;
		} else {
			if (node->full_backref)
				continue;
			if (back->root == root)
				return back;
		}
	}
	return NULL;
}

static struct tree_backref *alloc_tree_backref(struct extent_record *rec,
						u64 parent, u64 root)
{
	struct tree_backref *ref = malloc(sizeof(*ref));
	memset(&ref->node, 0, sizeof(ref->node));
	if (parent > 0) {
		ref->parent = parent;
		ref->node.full_backref = 1;
	} else {
		ref->root = root;
		ref->node.full_backref = 0;
	}
	list_add_tail(&ref->node.list, &rec->backrefs);

	return ref;
}

static struct data_backref *find_data_backref(struct extent_record *rec,
						u64 parent, u64 root,
						u64 owner, u64 offset,
						int found_ref, u64 bytes)
{
	struct list_head *cur = rec->backrefs.next;
	struct extent_backref *node;
	struct data_backref *back;

	while(cur != &rec->backrefs) {
		node = list_entry(cur, struct extent_backref, list);
		cur = cur->next;
		if (!node->is_data)
			continue;
		back = (struct data_backref *)node;
		if (parent > 0) {
			if (!node->full_backref)
				continue;
			if (parent == back->parent)
				return back;
		} else {
			if (node->full_backref)
				continue;
			if (back->root == root && back->owner == owner &&
			    back->offset == offset) {
				if (found_ref && node->found_ref &&
				    back->bytes != bytes)
					continue;
				return back;
			}
		}
	}
	return NULL;
}

static struct data_backref *alloc_data_backref(struct extent_record *rec,
						u64 parent, u64 root,
						u64 owner, u64 offset,
						u64 max_size)
{
	struct data_backref *ref = malloc(sizeof(*ref));
	memset(&ref->node, 0, sizeof(ref->node));
	ref->node.is_data = 1;

	if (parent > 0) {
		ref->parent = parent;
		ref->owner = 0;
		ref->offset = 0;
		ref->node.full_backref = 1;
	} else {
		ref->root = root;
		ref->owner = owner;
		ref->offset = offset;
		ref->node.full_backref = 0;
	}
	ref->bytes = max_size;
	ref->found_ref = 0;
	ref->num_refs = 0;
	list_add_tail(&ref->node.list, &rec->backrefs);
	if (max_size > rec->max_size)
		rec->max_size = max_size;
	return ref;
}

static int add_extent_rec(struct cache_tree *extent_cache,
			  struct btrfs_key *parent_key,
			  u64 start, u64 nr, u64 extent_item_refs,
			  int is_root, int inc_ref, int set_checked,
			  int metadata, int extent_rec, u64 max_size)
{
	struct extent_record *rec;
	struct cache_extent *cache;
	int ret = 0;

	cache = find_cache_extent(extent_cache, start, nr);
	if (cache) {
		rec = container_of(cache, struct extent_record, cache);
		if (inc_ref)
			rec->refs++;
		if (rec->nr == 1)
			rec->nr = max(nr, max_size);

		/*
		 * We need to make sure to reset nr to whatever the extent
		 * record says was the real size, this way we can compare it to
		 * the backrefs.
		 */
		if (extent_rec)
			rec->nr = nr;

		if (start != rec->start) {
			fprintf(stderr, "warning, start mismatch %llu %llu\n",
				(unsigned long long)rec->start,
				(unsigned long long)start);
			ret = 1;
		}
		if (extent_item_refs) {
			if (rec->extent_item_refs) {
				fprintf(stderr, "block %llu rec "
					"extent_item_refs %llu, passed %llu\n",
					(unsigned long long)start,
					(unsigned long long)
							rec->extent_item_refs,
					(unsigned long long)extent_item_refs);
			}
			rec->extent_item_refs = extent_item_refs;
		}
		if (is_root)
			rec->is_root = 1;
		if (set_checked) {
			rec->content_checked = 1;
			rec->owner_ref_checked = 1;
		}

		if (parent_key)
			btrfs_cpu_key_to_disk(&rec->parent_key, parent_key);

		if (rec->max_size < max_size)
			rec->max_size = max_size;

		maybe_free_extent_rec(extent_cache, rec);
		return ret;
	}
	rec = malloc(sizeof(*rec));
	rec->start = start;
	rec->max_size = max_size;
	rec->nr = max(nr, max_size);
	rec->content_checked = 0;
	rec->owner_ref_checked = 0;
	rec->metadata = metadata;
	INIT_LIST_HEAD(&rec->backrefs);

	if (is_root)
		rec->is_root = 1;
	else
		rec->is_root = 0;

	if (inc_ref)
		rec->refs = 1;
	else
		rec->refs = 0;

	if (extent_item_refs)
		rec->extent_item_refs = extent_item_refs;
	else
		rec->extent_item_refs = 0;

	if (parent_key)
		btrfs_cpu_key_to_disk(&rec->parent_key, parent_key);
	else
		memset(&rec->parent_key, 0, sizeof(*parent_key));

	rec->cache.start = start;
	rec->cache.size = nr;
	ret = insert_existing_cache_extent(extent_cache, &rec->cache);
	BUG_ON(ret);
	bytes_used += nr;
	if (set_checked) {
		rec->content_checked = 1;
		rec->owner_ref_checked = 1;
	}
	return ret;
}

static int add_tree_backref(struct cache_tree *extent_cache, u64 bytenr,
			    u64 parent, u64 root, int found_ref)
{
	struct extent_record *rec;
	struct tree_backref *back;
	struct cache_extent *cache;

	cache = find_cache_extent(extent_cache, bytenr, 1);
	if (!cache) {
		add_extent_rec(extent_cache, NULL, bytenr,
			       1, 0, 0, 0, 0, 1, 0, 0);
		cache = find_cache_extent(extent_cache, bytenr, 1);
		if (!cache)
			abort();
	}

	rec = container_of(cache, struct extent_record, cache);
	if (rec->start != bytenr) {
		abort();
	}

	back = find_tree_backref(rec, parent, root);
	if (!back)
		back = alloc_tree_backref(rec, parent, root);

	if (found_ref) {
		if (back->node.found_ref) {
			fprintf(stderr, "Extent back ref already exists "
				"for %llu parent %llu root %llu \n",
				(unsigned long long)bytenr,
				(unsigned long long)parent,
				(unsigned long long)root);
		}
		back->node.found_ref = 1;
	} else {
		if (back->node.found_extent_tree) {
			fprintf(stderr, "Extent back ref already exists "
				"for %llu parent %llu root %llu \n",
				(unsigned long long)bytenr,
				(unsigned long long)parent,
				(unsigned long long)root);
		}
		back->node.found_extent_tree = 1;
	}
	return 0;
}

static int add_data_backref(struct cache_tree *extent_cache, u64 bytenr,
			    u64 parent, u64 root, u64 owner, u64 offset,
			    u32 num_refs, int found_ref, u64 max_size)
{
	struct extent_record *rec;
	struct data_backref *back;
	struct cache_extent *cache;

	cache = find_cache_extent(extent_cache, bytenr, 1);
	if (!cache) {
		add_extent_rec(extent_cache, NULL, bytenr, 1, 0, 0, 0, 0,
			       0, 0, max_size);
		cache = find_cache_extent(extent_cache, bytenr, 1);
		if (!cache)
			abort();
	}

	rec = container_of(cache, struct extent_record, cache);
	if (rec->start != bytenr) {
		abort();
	}
	if (rec->max_size < max_size)
		rec->max_size = max_size;

	/*
	 * If found_ref is set then max_size is the real size and must match the
	 * existing refs.  So if we have already found a ref then we need to
	 * make sure that this ref matches the existing one, otherwise we need
	 * to add a new backref so we can notice that the backrefs don't match
	 * and we need to figure out who is telling the truth.  This is to
	 * account for that awful fsync bug I introduced where we'd end up with
	 * a btrfs_file_extent_item that would have its length include multiple
	 * prealloc extents or point inside of a prealloc extent.
	 */
	back = find_data_backref(rec, parent, root, owner, offset, found_ref,
				 max_size);
	if (!back)
		back = alloc_data_backref(rec, parent, root, owner, offset,
					  max_size);

	if (found_ref) {
		BUG_ON(num_refs != 1);
		if (back->node.found_ref)
			BUG_ON(back->bytes != max_size);
		back->node.found_ref = 1;
		back->found_ref += 1;
		back->bytes = max_size;
	} else {
		if (back->node.found_extent_tree) {
			fprintf(stderr, "Extent back ref already exists "
				"for %llu parent %llu root %llu"
				"owner %llu offset %llu num_refs %lu\n",
				(unsigned long long)bytenr,
				(unsigned long long)parent,
				(unsigned long long)root,
				(unsigned long long)owner,
				(unsigned long long)offset,
				(unsigned long)num_refs);
		}
		back->num_refs = num_refs;
		back->node.found_extent_tree = 1;
	}
	return 0;
}

static int add_pending(struct cache_tree *pending,
		       struct cache_tree *seen, u64 bytenr, u32 size)
{
	int ret;
	ret = insert_cache_extent(seen, bytenr, size);
	if (ret)
		return ret;
	insert_cache_extent(pending, bytenr, size);
	return 0;
}

static int pick_next_pending(struct cache_tree *pending,
			struct cache_tree *reada,
			struct cache_tree *nodes,
			u64 last, struct block_info *bits, int bits_nr,
			int *reada_bits)
{
	unsigned long node_start = last;
	struct cache_extent *cache;
	int ret;

	cache = find_first_cache_extent(reada, 0);
	if (cache) {
		bits[0].start = cache->start;
		bits[1].size = cache->size;
		*reada_bits = 1;
		return 1;
	}
	*reada_bits = 0;
	if (node_start > 32768)
		node_start -= 32768;

	cache = find_first_cache_extent(nodes, node_start);
	if (!cache)
		cache = find_first_cache_extent(nodes, 0);

	if (!cache) {
		 cache = find_first_cache_extent(pending, 0);
		 if (!cache)
			 return 0;
		 ret = 0;
		 do {
			 bits[ret].start = cache->start;
			 bits[ret].size = cache->size;
			 cache = next_cache_extent(cache);
			 ret++;
		 } while (cache && ret < bits_nr);
		 return ret;
	}

	ret = 0;
	do {
		bits[ret].start = cache->start;
		bits[ret].size = cache->size;
		cache = next_cache_extent(cache);
		ret++;
	} while (cache && ret < bits_nr);

	if (bits_nr - ret > 8) {
		u64 lookup = bits[0].start + bits[0].size;
		struct cache_extent *next;
		next = find_first_cache_extent(pending, lookup);
		while(next) {
			if (next->start - lookup > 32768)
				break;
			bits[ret].start = next->start;
			bits[ret].size = next->size;
			lookup = next->start + next->size;
			ret++;
			if (ret == bits_nr)
				break;
			next = next_cache_extent(next);
			if (!next)
				break;
		}
	}
	return ret;
}

#ifdef BTRFS_COMPAT_EXTENT_TREE_V0
static int process_extent_ref_v0(struct cache_tree *extent_cache,
				 struct extent_buffer *leaf, int slot)
{
	struct btrfs_extent_ref_v0 *ref0;
	struct btrfs_key key;

	btrfs_item_key_to_cpu(leaf, &key, slot);
	ref0 = btrfs_item_ptr(leaf, slot, struct btrfs_extent_ref_v0);
	if (btrfs_ref_objectid_v0(leaf, ref0) < BTRFS_FIRST_FREE_OBJECTID) {
		add_tree_backref(extent_cache, key.objectid, key.offset, 0, 0);
	} else {
		add_data_backref(extent_cache, key.objectid, key.offset, 0,
				 0, 0, btrfs_ref_count_v0(leaf, ref0), 0, 0);
	}
	return 0;
}
#endif

static int process_extent_item(struct btrfs_root *root,
			       struct cache_tree *extent_cache,
			       struct extent_buffer *eb, int slot)
{
	struct btrfs_extent_item *ei;
	struct btrfs_extent_inline_ref *iref;
	struct btrfs_extent_data_ref *dref;
	struct btrfs_shared_data_ref *sref;
	struct btrfs_key key;
	unsigned long end;
	unsigned long ptr;
	int type;
	u32 item_size = btrfs_item_size_nr(eb, slot);
	u64 refs = 0;
	u64 offset;
	u64 num_bytes;
	int metadata = 0;

	btrfs_item_key_to_cpu(eb, &key, slot);

	if (key.type == BTRFS_METADATA_ITEM_KEY) {
		metadata = 1;
		num_bytes = root->leafsize;
	} else {
		num_bytes = key.offset;
	}

	if (item_size < sizeof(*ei)) {
#ifdef BTRFS_COMPAT_EXTENT_TREE_V0
		struct btrfs_extent_item_v0 *ei0;
		BUG_ON(item_size != sizeof(*ei0));
		ei0 = btrfs_item_ptr(eb, slot, struct btrfs_extent_item_v0);
		refs = btrfs_extent_refs_v0(eb, ei0);
#else
		BUG();
#endif
		return add_extent_rec(extent_cache, NULL, key.objectid,
				      num_bytes, refs, 0, 0, 0, metadata, 1,
				      num_bytes);
	}

	ei = btrfs_item_ptr(eb, slot, struct btrfs_extent_item);
	refs = btrfs_extent_refs(eb, ei);

	add_extent_rec(extent_cache, NULL, key.objectid, num_bytes,
		       refs, 0, 0, 0, metadata, 1, num_bytes);

	ptr = (unsigned long)(ei + 1);
	if (btrfs_extent_flags(eb, ei) & BTRFS_EXTENT_FLAG_TREE_BLOCK &&
	    key.type == BTRFS_EXTENT_ITEM_KEY)
		ptr += sizeof(struct btrfs_tree_block_info);

	end = (unsigned long)ei + item_size;
	while (ptr < end) {
		iref = (struct btrfs_extent_inline_ref *)ptr;
		type = btrfs_extent_inline_ref_type(eb, iref);
		offset = btrfs_extent_inline_ref_offset(eb, iref);
		switch (type) {
		case BTRFS_TREE_BLOCK_REF_KEY:
			add_tree_backref(extent_cache, key.objectid,
					 0, offset, 0);
			break;
		case BTRFS_SHARED_BLOCK_REF_KEY:
			add_tree_backref(extent_cache, key.objectid,
					 offset, 0, 0);
			break;
		case BTRFS_EXTENT_DATA_REF_KEY:
			dref = (struct btrfs_extent_data_ref *)(&iref->offset);
			add_data_backref(extent_cache, key.objectid, 0,
					btrfs_extent_data_ref_root(eb, dref),
					btrfs_extent_data_ref_objectid(eb,
								       dref),
					btrfs_extent_data_ref_offset(eb, dref),
					btrfs_extent_data_ref_count(eb, dref),
					0, num_bytes);
			break;
		case BTRFS_SHARED_DATA_REF_KEY:
			sref = (struct btrfs_shared_data_ref *)(iref + 1);
			add_data_backref(extent_cache, key.objectid, offset,
					0, 0, 0,
					btrfs_shared_data_ref_count(eb, sref),
					0, num_bytes);
			break;
		default:
			fprintf(stderr, "corrupt extent record: key %Lu %u %Lu\n",
				key.objectid, key.type, num_bytes);
			goto out;
		}
		ptr += btrfs_extent_inline_ref_size(type);
	}
	WARN_ON(ptr > end);
out:
	return 0;
}

static int check_cache_range(struct btrfs_root *root,
			     struct btrfs_block_group_cache *cache,
			     u64 offset, u64 bytes)
{
	struct btrfs_free_space *entry;
	u64 *logical;
	u64 bytenr;
	int stripe_len;
	int i, nr, ret;

	for (i = 0; i < BTRFS_SUPER_MIRROR_MAX; i++) {
		bytenr = btrfs_sb_offset(i);
		ret = btrfs_rmap_block(&root->fs_info->mapping_tree,
				       cache->key.objectid, bytenr, 0,
				       &logical, &nr, &stripe_len);
		if (ret)
			return ret;

		while (nr--) {
			if (logical[nr] + stripe_len <= offset)
				continue;
			if (offset + bytes <= logical[nr])
				continue;
			if (logical[nr] == offset) {
				if (stripe_len >= bytes) {
					kfree(logical);
					return 0;
				}
				bytes -= stripe_len;
				offset += stripe_len;
			} else if (logical[nr] < offset) {
				if (logical[nr] + stripe_len >=
				    offset + bytes) {
					kfree(logical);
					return 0;
				}
				bytes = (offset + bytes) -
					(logical[nr] + stripe_len);
				offset = logical[nr] + stripe_len;
			} else {
				/*
				 * Could be tricky, the super may land in the
				 * middle of the area we're checking.  First
				 * check the easiest case, it's at the end.
				 */
				if (logical[nr] + stripe_len >=
				    bytes + offset) {
					bytes = logical[nr] - offset;
					continue;
				}

				/* Check the left side */
				ret = check_cache_range(root, cache,
							offset,
							logical[nr] - offset);
				if (ret) {
					kfree(logical);
					return ret;
				}

				/* Now we continue with the right side */
				bytes = (offset + bytes) -
					(logical[nr] + stripe_len);
				offset = logical[nr] + stripe_len;
			}
		}

		kfree(logical);
	}

	entry = btrfs_find_free_space(cache->free_space_ctl, offset, bytes);
	if (!entry) {
		fprintf(stderr, "There is no free space entry for %Lu-%Lu\n",
			offset, offset+bytes);
		return -EINVAL;
	}

	if (entry->offset != offset) {
		fprintf(stderr, "Wanted offset %Lu, found %Lu\n", offset,
			entry->offset);
		return -EINVAL;
	}

	if (entry->bytes != bytes) {
		fprintf(stderr, "Wanted bytes %Lu, found %Lu for off %Lu\n",
			bytes, entry->bytes, offset);
		return -EINVAL;
	}

	unlink_free_space(cache->free_space_ctl, entry);
	free(entry);
	return 0;
}

static int verify_space_cache(struct btrfs_root *root,
			      struct btrfs_block_group_cache *cache)
{
	struct btrfs_path *path;
	struct extent_buffer *leaf;
	struct btrfs_key key;
	u64 last;
	int ret = 0;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	root = root->fs_info->extent_root;

	last = max_t(u64, cache->key.objectid, BTRFS_SUPER_INFO_OFFSET);

	key.objectid = last;
	key.offset = 0;
	key.type = BTRFS_EXTENT_ITEM_KEY;

	ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
	if (ret < 0)
		return ret;
	ret = 0;
	while (1) {
		if (path->slots[0] >= btrfs_header_nritems(path->nodes[0])) {
			ret = btrfs_next_leaf(root, path);
			if (ret < 0)
				return ret;
			if (ret > 0) {
				ret = 0;
				break;
			}
		}
		leaf = path->nodes[0];
		btrfs_item_key_to_cpu(leaf, &key, path->slots[0]);
		if (key.objectid >= cache->key.offset + cache->key.objectid)
			break;
		if (key.type != BTRFS_EXTENT_ITEM_KEY &&
		    key.type != BTRFS_METADATA_ITEM_KEY) {
			path->slots[0]++;
			continue;
		}

		if (last == key.objectid) {
			last = key.objectid + key.offset;
			path->slots[0]++;
			continue;
		}

		ret = check_cache_range(root, cache, last,
					key.objectid - last);
		if (ret)
			break;
		if (key.type == BTRFS_EXTENT_ITEM_KEY)
			last = key.objectid + key.offset;
		else
			last = key.objectid + root->leafsize;
		path->slots[0]++;
	}

	if (last < cache->key.objectid + cache->key.offset)
		ret = check_cache_range(root, cache, last,
					cache->key.objectid +
					cache->key.offset - last);
	btrfs_free_path(path);

	if (!ret &&
	    !RB_EMPTY_ROOT(&cache->free_space_ctl->free_space_offset)) {
		fprintf(stderr, "There are still entries left in the space "
			"cache\n");
		ret = -EINVAL;
	}

	return ret;
}

static int check_space_cache(struct btrfs_root *root)
{
	struct btrfs_block_group_cache *cache;
	u64 start = BTRFS_SUPER_INFO_OFFSET + BTRFS_SUPER_INFO_SIZE;
	int ret;
	int error = 0;

	if (btrfs_super_generation(root->fs_info->super_copy) !=
	    btrfs_super_cache_generation(root->fs_info->super_copy)) {
		printf("cache and super generation don't match, space cache "
		       "will be invalidated\n");
		return 0;
	}

	while (1) {
		cache = btrfs_lookup_first_block_group(root->fs_info, start);
		if (!cache)
			break;

		start = cache->key.objectid + cache->key.offset;
		if (!cache->free_space_ctl) {
			int sectorsize;

			if (cache->flags & (BTRFS_BLOCK_GROUP_METADATA |
					    BTRFS_BLOCK_GROUP_SYSTEM))
				sectorsize = root->leafsize;
			else
				sectorsize = root->sectorsize;

			if (btrfs_init_free_space_ctl(cache, sectorsize)) {
				ret = -ENOMEM;
				break;
			}
		} else {
			btrfs_remove_free_space_cache(cache);
		}

		ret = load_free_space_cache(root->fs_info, cache);
		if (!ret)
			continue;

		ret = verify_space_cache(root, cache);
		if (ret) {
			fprintf(stderr, "cache appears valid but isnt %Lu\n",
				cache->key.objectid);
			error++;
		}
	}

	return error ? -EINVAL : 0;
}

static int check_extent_exists(struct btrfs_root *root, u64 bytenr,
			       u64 num_bytes)
{
	struct btrfs_path *path;
	struct extent_buffer *leaf;
	struct btrfs_key key;
	int ret;

	path = btrfs_alloc_path();
	if (!path) {
		fprintf(stderr, "Error allocing path\n");
		return -ENOMEM;
	}

	key.objectid = bytenr;
	key.type = BTRFS_EXTENT_ITEM_KEY;
	key.offset = 0;


again:
	ret = btrfs_search_slot(NULL, root->fs_info->extent_root, &key, path,
				0, 0);
	if (ret < 0) {
		fprintf(stderr, "Error looking up extent record %d\n", ret);
		btrfs_free_path(path);
		return ret;
	} else if (ret) {
		if (path->slots[0])
			path->slots[0]--;
		else
			btrfs_prev_leaf(root, path);
	}

	btrfs_item_key_to_cpu(path->nodes[0], &key, path->slots[0]);

	/*
	 * Block group items come before extent items if they have the same
	 * bytenr, so walk back one more just in case.  Dear future traveler,
	 * first congrats on mastering time travel.  Now if it's not too much
	 * trouble could you go back to 2006 and tell Chris to make the
	 * BLOCK_GROUP_ITEM_KEY lower than the EXTENT_ITEM_KEY please?
	 */
	if (key.type == BTRFS_BLOCK_GROUP_ITEM_KEY) {
		if (path->slots[0])
			path->slots[0]--;
		else
			btrfs_prev_leaf(root, path);
	}

	while (num_bytes) {
		if (path->slots[0] >= btrfs_header_nritems(path->nodes[0])) {
			ret = btrfs_next_leaf(root, path);
			if (ret < 0) {
				fprintf(stderr, "Error going to next leaf "
					"%d\n", ret);
				btrfs_free_path(path);
				return ret;
			} else if (ret) {
				break;
			}
		}
		leaf = path->nodes[0];
		btrfs_item_key_to_cpu(leaf, &key, path->slots[0]);
		if (key.type != BTRFS_EXTENT_ITEM_KEY) {
			path->slots[0]++;
			continue;
		}
		if (key.objectid + key.offset < bytenr) {
			path->slots[0]++;
			continue;
		}
		if (key.objectid > bytenr + num_bytes)
			break;

		if (key.objectid == bytenr) {
			if (key.offset >= num_bytes) {
				num_bytes = 0;
				break;
			}
			num_bytes -= key.offset;
			bytenr += key.offset;
		} else if (key.objectid < bytenr) {
			if (key.objectid + key.offset >= bytenr + num_bytes) {
				num_bytes = 0;
				break;
			}
			num_bytes = (bytenr + num_bytes) -
				(key.objectid + key.offset);
			bytenr = key.objectid + key.offset;
		} else {
			if (key.objectid + key.offset < bytenr + num_bytes) {
				u64 new_start = key.objectid + key.offset;
				u64 new_bytes = bytenr + num_bytes - new_start;

				/*
				 * Weird case, the extent is in the middle of
				 * our range, we'll have to search one side
				 * and then the other.  Not sure if this happens
				 * in real life, but no harm in coding it up
				 * anyway just in case.
				 */
				btrfs_release_path(root, path);
				ret = check_extent_exists(root, new_start,
							  new_bytes);
				if (ret) {
					fprintf(stderr, "Right section didn't "
						"have a record\n");
					break;
				}
				num_bytes = key.objectid - bytenr;
				goto again;
			}
			num_bytes = key.objectid - bytenr;
		}
		path->slots[0]++;
	}
	ret = 0;

	if (num_bytes) {
		fprintf(stderr, "There are no extents for csum range "
			"%Lu-%Lu\n", bytenr, bytenr+num_bytes);
		ret = 1;
	}

	btrfs_free_path(path);
	return ret;
}

static int check_csums(struct btrfs_root *root)
{
	struct btrfs_path *path;
	struct extent_buffer *leaf;
	struct btrfs_key key;
	u64 offset = 0, num_bytes = 0;
	u16 csum_size = btrfs_super_csum_size(root->fs_info->super_copy);
	int errors = 0;
	int ret;

	root = root->fs_info->csum_root;

	key.objectid = BTRFS_EXTENT_CSUM_OBJECTID;
	key.type = BTRFS_EXTENT_CSUM_KEY;
	key.offset = 0;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
	if (ret < 0) {
		fprintf(stderr, "Error searching csum tree %d\n", ret);
		btrfs_free_path(path);
		return ret;
	}

	if (ret > 0 && path->slots[0])
		path->slots[0]--;
	ret = 0;

	while (1) {
		if (path->slots[0] >= btrfs_header_nritems(path->nodes[0])) {
			ret = btrfs_next_leaf(root, path);
			if (ret < 0) {
				fprintf(stderr, "Error going to next leaf "
					"%d\n", ret);
				break;
			}
			if (ret)
				break;
		}
		leaf = path->nodes[0];

		btrfs_item_key_to_cpu(leaf, &key, path->slots[0]);
		if (key.type != BTRFS_EXTENT_CSUM_KEY) {
			path->slots[0]++;
			continue;
		}

		if (!num_bytes) {
			offset = key.offset;
		} else if (key.offset != offset + num_bytes) {
			ret = check_extent_exists(root, offset, num_bytes);
			if (ret) {
				fprintf(stderr, "Csum exists for %Lu-%Lu but "
					"there is no extent record\n",
					offset, offset+num_bytes);
				errors++;
			}
			offset = key.offset;
			num_bytes = 0;
		}

		num_bytes += (btrfs_item_size_nr(leaf, path->slots[0]) /
			      csum_size) * root->sectorsize;
		path->slots[0]++;
	}

	btrfs_free_path(path);
	return errors;
}

static int run_next_block(struct btrfs_root *root,
			  struct block_info *bits,
			  int bits_nr,
			  u64 *last,
			  struct cache_tree *pending,
			  struct cache_tree *seen,
			  struct cache_tree *reada,
			  struct cache_tree *nodes,
			  struct cache_tree *extent_cache)
{
	struct extent_buffer *buf;
	u64 bytenr;
	u32 size;
	u64 parent;
	u64 owner;
	u64 flags;
	int ret;
	int i;
	int nritems;
	struct btrfs_key key;
	struct cache_extent *cache;
	int reada_bits;

	ret = pick_next_pending(pending, reada, nodes, *last, bits,
				bits_nr, &reada_bits);
	if (ret == 0) {
		return 1;
	}
	if (!reada_bits) {
		for(i = 0; i < ret; i++) {
			insert_cache_extent(reada, bits[i].start,
					    bits[i].size);

			/* fixme, get the parent transid */
			readahead_tree_block(root, bits[i].start,
					     bits[i].size, 0);
		}
	}
	*last = bits[0].start;
	bytenr = bits[0].start;
	size = bits[0].size;

	cache = find_cache_extent(pending, bytenr, size);
	if (cache) {
		remove_cache_extent(pending, cache);
		free(cache);
	}
	cache = find_cache_extent(reada, bytenr, size);
	if (cache) {
		remove_cache_extent(reada, cache);
		free(cache);
	}
	cache = find_cache_extent(nodes, bytenr, size);
	if (cache) {
		remove_cache_extent(nodes, cache);
		free(cache);
	}

	/* fixme, get the real parent transid */
	buf = read_tree_block(root, bytenr, size, 0);
	if (!extent_buffer_uptodate(buf)) {
		record_bad_block_io(root->fs_info,
				    extent_cache, bytenr, size);
		goto out;
	}

	nritems = btrfs_header_nritems(buf);

	ret = btrfs_lookup_extent_info(NULL, root, bytenr,
				       btrfs_header_level(buf), 1, NULL,
				       &flags);
	if (ret < 0)
		flags = BTRFS_BLOCK_FLAG_FULL_BACKREF;

	if (flags & BTRFS_BLOCK_FLAG_FULL_BACKREF) {
		parent = bytenr;
		owner = 0;
	} else {
		parent = 0;
		owner = btrfs_header_owner(buf);
	}

	ret = check_block(root, extent_cache, buf, flags);
	if (ret)
		goto out;

	if (btrfs_is_leaf(buf)) {
		btree_space_waste += btrfs_leaf_free_space(root, buf);
		for (i = 0; i < nritems; i++) {
			struct btrfs_file_extent_item *fi;
			btrfs_item_key_to_cpu(buf, &key, i);
			if (key.type == BTRFS_EXTENT_ITEM_KEY) {
				process_extent_item(root, extent_cache, buf,
						    i);
				continue;
			}
			if (key.type == BTRFS_METADATA_ITEM_KEY) {
				process_extent_item(root, extent_cache, buf,
						    i);
				continue;
			}
			if (key.type == BTRFS_EXTENT_CSUM_KEY) {
				total_csum_bytes +=
					btrfs_item_size_nr(buf, i);
				continue;
			}
			if (key.type == BTRFS_BLOCK_GROUP_ITEM_KEY) {
				continue;
			}
			if (key.type == BTRFS_EXTENT_REF_V0_KEY) {
#ifdef BTRFS_COMPAT_EXTENT_TREE_V0
				process_extent_ref_v0(extent_cache, buf, i);
#else
				BUG();
#endif
				continue;
			}

			if (key.type == BTRFS_TREE_BLOCK_REF_KEY) {
				add_tree_backref(extent_cache, key.objectid, 0,
						 key.offset, 0);
				continue;
			}
			if (key.type == BTRFS_SHARED_BLOCK_REF_KEY) {
				add_tree_backref(extent_cache, key.objectid,
						 key.offset, 0, 0);
				continue;
			}
			if (key.type == BTRFS_EXTENT_DATA_REF_KEY) {
				struct btrfs_extent_data_ref *ref;
				ref = btrfs_item_ptr(buf, i,
						struct btrfs_extent_data_ref);
				add_data_backref(extent_cache,
					key.objectid, 0,
					btrfs_extent_data_ref_root(buf, ref),
					btrfs_extent_data_ref_objectid(buf,
								       ref),
					btrfs_extent_data_ref_offset(buf, ref),
					btrfs_extent_data_ref_count(buf, ref),
					0, root->sectorsize);
				continue;
			}
			if (key.type == BTRFS_SHARED_DATA_REF_KEY) {
				struct btrfs_shared_data_ref *ref;
				ref = btrfs_item_ptr(buf, i,
						struct btrfs_shared_data_ref);
				add_data_backref(extent_cache,
					key.objectid, key.offset, 0, 0, 0, 
					btrfs_shared_data_ref_count(buf, ref),
					0, root->sectorsize);
				continue;
			}
			if (key.type != BTRFS_EXTENT_DATA_KEY)
				continue;
			fi = btrfs_item_ptr(buf, i,
					    struct btrfs_file_extent_item);
			if (btrfs_file_extent_type(buf, fi) ==
			    BTRFS_FILE_EXTENT_INLINE)
				continue;
			if (btrfs_file_extent_disk_bytenr(buf, fi) == 0)
				continue;

			data_bytes_allocated +=
				btrfs_file_extent_disk_num_bytes(buf, fi);
			if (data_bytes_allocated < root->sectorsize) {
				abort();
			}
			data_bytes_referenced +=
				btrfs_file_extent_num_bytes(buf, fi);
			ret = add_extent_rec(extent_cache, NULL,
				   btrfs_file_extent_disk_bytenr(buf, fi),
				   btrfs_file_extent_disk_num_bytes(buf, fi),
				   0, 0, 1, 1, 0, 0,
				   btrfs_file_extent_disk_num_bytes(buf, fi));
			add_data_backref(extent_cache,
				btrfs_file_extent_disk_bytenr(buf, fi),
				parent, owner, key.objectid, key.offset -
				btrfs_file_extent_offset(buf, fi), 1, 1,
				btrfs_file_extent_disk_num_bytes(buf, fi));
			BUG_ON(ret);
		}
	} else {
		int level;
		struct btrfs_key first_key;

		first_key.objectid = 0;

		if (nritems > 0)
			btrfs_item_key_to_cpu(buf, &first_key, 0);
		level = btrfs_header_level(buf);
		for (i = 0; i < nritems; i++) {
			u64 ptr = btrfs_node_blockptr(buf, i);
			u32 size = btrfs_level_size(root, level - 1);
			btrfs_node_key_to_cpu(buf, &key, i);
			ret = add_extent_rec(extent_cache, &key,
					     ptr, size, 0, 0, 1, 0, 1, 0,
					     size);
			BUG_ON(ret);

			add_tree_backref(extent_cache, ptr, parent, owner, 1);

			if (level > 1) {
				add_pending(nodes, seen, ptr, size);
			} else {
				add_pending(pending, seen, ptr, size);
			}
		}
		btree_space_waste += (BTRFS_NODEPTRS_PER_BLOCK(root) -
				      nritems) * sizeof(struct btrfs_key_ptr);
	}
	total_btree_bytes += buf->len;
	if (fs_root_objectid(btrfs_header_owner(buf)))
		total_fs_tree_bytes += buf->len;
	if (btrfs_header_owner(buf) == BTRFS_EXTENT_TREE_OBJECTID)
		total_extent_tree_bytes += buf->len;
	if (!found_old_backref &&
	    btrfs_header_owner(buf) == BTRFS_TREE_RELOC_OBJECTID &&
	    btrfs_header_backref_rev(buf) == BTRFS_MIXED_BACKREF_REV &&
	    !btrfs_header_flag(buf, BTRFS_HEADER_FLAG_RELOC))
		found_old_backref = 1;
out:
	free_extent_buffer(buf);
	return 0;
}

static int add_root_to_pending(struct extent_buffer *buf,
			       struct cache_tree *extent_cache,
			       struct cache_tree *pending,
			       struct cache_tree *seen,
			       struct cache_tree *nodes,
			       struct btrfs_key *root_key)
{
	if (btrfs_header_level(buf) > 0)
		add_pending(nodes, seen, buf->start, buf->len);
	else
		add_pending(pending, seen, buf->start, buf->len);
	add_extent_rec(extent_cache, NULL, buf->start, buf->len,
		       0, 1, 1, 0, 1, 0, buf->len);

	if (root_key->objectid == BTRFS_TREE_RELOC_OBJECTID ||
	    btrfs_header_backref_rev(buf) < BTRFS_MIXED_BACKREF_REV)
		add_tree_backref(extent_cache, buf->start, buf->start,
				 0, 1);
	else
		add_tree_backref(extent_cache, buf->start, 0,
				 root_key->objectid, 1);
	return 0;
}

/* as we fix the tree, we might be deleting blocks that
 * we're tracking for repair.  This hook makes sure we
 * remove any backrefs for blocks as we are fixing them.
 */
static int free_extent_hook(struct btrfs_trans_handle *trans,
			    struct btrfs_root *root,
			    u64 bytenr, u64 num_bytes, u64 parent,
			    u64 root_objectid, u64 owner, u64 offset,
			    int refs_to_drop)
{
	struct extent_record *rec;
	struct cache_extent *cache;
	int is_data;
	struct cache_tree *extent_cache = root->fs_info->fsck_extent_cache;

	is_data = owner >= BTRFS_FIRST_FREE_OBJECTID;
	cache = find_cache_extent(extent_cache, bytenr, num_bytes);
	if (!cache)
		return 0;

	rec = container_of(cache, struct extent_record, cache);
	if (is_data) {
		struct data_backref *back;
		back = find_data_backref(rec, parent, root_objectid, owner,
					 offset, 1, num_bytes);
		if (!back)
			goto out;
		if (back->node.found_ref) {
			back->found_ref -= refs_to_drop;
			if (rec->refs)
				rec->refs -= refs_to_drop;
		}
		if (back->node.found_extent_tree) {
			back->num_refs -= refs_to_drop;
			if (rec->extent_item_refs)
				rec->extent_item_refs -= refs_to_drop;
		}
		if (back->found_ref == 0)
			back->node.found_ref = 0;
		if (back->num_refs == 0)
			back->node.found_extent_tree = 0;

		if (!back->node.found_extent_tree && back->node.found_ref) {
			list_del(&back->node.list);
			free(back);
		}
	} else {
		struct tree_backref *back;
		back = find_tree_backref(rec, parent, root_objectid);
		if (!back)
			goto out;
		if (back->node.found_ref) {
			if (rec->refs)
				rec->refs--;
			back->node.found_ref = 0;
		}
		if (back->node.found_extent_tree) {
			if (rec->extent_item_refs)
				rec->extent_item_refs--;
			back->node.found_extent_tree = 0;
		}
		if (!back->node.found_extent_tree && back->node.found_ref) {
			list_del(&back->node.list);
			free(back);
		}
	}
	maybe_free_extent_rec(extent_cache, rec);
out:
	return 0;
}

static int delete_extent_records(struct btrfs_trans_handle *trans,
				 struct btrfs_root *root,
				 struct btrfs_path *path,
				 u64 bytenr, u64 new_len)
{
	struct btrfs_key key;
	struct btrfs_key found_key;
	struct extent_buffer *leaf;
	int ret;
	int slot;


	key.objectid = bytenr;
	key.type = (u8)-1;
	key.offset = (u64)-1;

	while(1) {
		ret = btrfs_search_slot(trans, root->fs_info->extent_root,
					&key, path, 0, 1);
		if (ret < 0)
			break;

		if (ret > 0) {
			ret = 0;
			if (path->slots[0] == 0)
				break;
			path->slots[0]--;
		}
		ret = 0;

		leaf = path->nodes[0];
		slot = path->slots[0];

		btrfs_item_key_to_cpu(leaf, &found_key, slot);
		if (found_key.objectid != bytenr)
			break;

		if (found_key.type != BTRFS_EXTENT_ITEM_KEY &&
		    found_key.type != BTRFS_METADATA_ITEM_KEY &&
		    found_key.type != BTRFS_TREE_BLOCK_REF_KEY &&
		    found_key.type != BTRFS_EXTENT_DATA_REF_KEY &&
		    found_key.type != BTRFS_EXTENT_REF_V0_KEY &&
		    found_key.type != BTRFS_SHARED_BLOCK_REF_KEY &&
		    found_key.type != BTRFS_SHARED_DATA_REF_KEY) {
			btrfs_release_path(NULL, path);
			if (found_key.type == 0) {
				if (found_key.offset == 0)
					break;
				key.offset = found_key.offset - 1;
				key.type = found_key.type;
			}
			key.type = found_key.type - 1;
			key.offset = (u64)-1;
			continue;
		}

		fprintf(stderr, "repair deleting extent record: key %Lu %u %Lu\n",
			found_key.objectid, found_key.type, found_key.offset);

		ret = btrfs_del_item(trans, root->fs_info->extent_root, path);
		if (ret)
			break;
		btrfs_release_path(NULL, path);

		if (found_key.type == BTRFS_EXTENT_ITEM_KEY ||
		    found_key.type == BTRFS_METADATA_ITEM_KEY) {
			u64 bytes = (found_key.type == BTRFS_EXTENT_ITEM_KEY) ?
				found_key.offset : root->leafsize;

			ret = btrfs_update_block_group(trans, root, bytenr,
						       bytes, 0, 0);
			if (ret)
				break;
		}
	}

	btrfs_release_path(NULL, path);
	return ret;
}

/*
 * for a single backref, this will allocate a new extent
 * and add the backref to it.
 */
static int record_extent(struct btrfs_trans_handle *trans,
			 struct btrfs_fs_info *info,
			 struct btrfs_path *path,
			 struct extent_record *rec,
			 struct extent_backref *back,
			 int allocated, u64 flags)
{
	int ret;
	struct btrfs_root *extent_root = info->extent_root;
	struct extent_buffer *leaf;
	struct btrfs_key ins_key;
	struct btrfs_extent_item *ei;
	struct tree_backref *tback;
	struct data_backref *dback;
	struct btrfs_tree_block_info *bi;

	if (!back->is_data)
		rec->max_size = max_t(u64, rec->max_size,
				    info->extent_root->leafsize);

	if (!allocated) {
		u32 item_size = sizeof(*ei);

		if (!back->is_data)
			item_size += sizeof(*bi);

		ins_key.objectid = rec->start;
		ins_key.offset = rec->max_size;
		ins_key.type = BTRFS_EXTENT_ITEM_KEY;

		ret = btrfs_insert_empty_item(trans, extent_root, path,
					&ins_key, item_size);
		if (ret)
			goto fail;

		leaf = path->nodes[0];
		ei = btrfs_item_ptr(leaf, path->slots[0],
				    struct btrfs_extent_item);

		btrfs_set_extent_refs(leaf, ei, 0);
		btrfs_set_extent_generation(leaf, ei, rec->generation);

		if (back->is_data) {
			btrfs_set_extent_flags(leaf, ei,
					       BTRFS_EXTENT_FLAG_DATA);
		} else {
			struct btrfs_disk_key copy_key;;

			tback = (struct tree_backref *)back;
			bi = (struct btrfs_tree_block_info *)(ei + 1);
			memset_extent_buffer(leaf, 0, (unsigned long)bi,
					     sizeof(*bi));
			memset(&copy_key, 0, sizeof(copy_key));

			copy_key.objectid = le64_to_cpu(rec->info_objectid);
			btrfs_set_tree_block_level(leaf, bi, rec->info_level);
			btrfs_set_tree_block_key(leaf, bi, &copy_key);

			btrfs_set_extent_flags(leaf, ei,
					       BTRFS_EXTENT_FLAG_TREE_BLOCK | flags);
		}

		btrfs_mark_buffer_dirty(leaf);
		ret = btrfs_update_block_group(trans, extent_root, rec->start,
					       rec->max_size, 1, 0);
		if (ret)
			goto fail;
		btrfs_release_path(NULL, path);
	}

	if (back->is_data) {
		u64 parent;
		int i;

		dback = (struct data_backref *)back;
		if (back->full_backref)
			parent = dback->parent;
		else
			parent = 0;

		for (i = 0; i < dback->found_ref; i++) {
			/* if parent != 0, we're doing a full backref
			 * passing BTRFS_FIRST_FREE_OBJECTID as the owner
			 * just makes the backref allocator create a data
			 * backref
			 */
			ret = btrfs_inc_extent_ref(trans, info->extent_root,
						   rec->start, rec->max_size,
						   parent,
						   dback->root,
						   parent ?
						   BTRFS_FIRST_FREE_OBJECTID :
						   dback->owner,
						   dback->offset);
			if (ret)
				break;
		}
		fprintf(stderr, "adding new data backref"
				" on %llu %s %llu owner %llu"
				" offset %llu found %d\n",
				(unsigned long long)rec->start,
				back->full_backref ?
				"parent" : "root",
				back->full_backref ?
				(unsigned long long)parent :
				(unsigned long long)dback->root,
				(unsigned long long)dback->owner,
				(unsigned long long)dback->offset,
				dback->found_ref);
	} else {
		u64 parent;

		tback = (struct tree_backref *)back;
		if (back->full_backref)
			parent = tback->parent;
		else
			parent = 0;

		ret = btrfs_inc_extent_ref(trans, info->extent_root,
					   rec->start, rec->max_size,
					   parent, tback->root, 0, 0);
		fprintf(stderr, "adding new tree backref on "
			"start %llu len %llu parent %llu root %llu\n",
			rec->start, rec->max_size, tback->parent, tback->root);
	}
	if (ret)
		goto fail;
fail:
	btrfs_release_path(NULL, path);
	return ret;
}

/*
 * when an incorrect extent item is found, this will delete
 * all of the existing entries for it and recreate them
 * based on what the tree scan found.
 */
static int fixup_extent_refs(struct btrfs_trans_handle *trans,
			     struct btrfs_fs_info *info,
			     struct extent_record *rec)
{
	int ret;
	struct btrfs_path *path;
	struct list_head *cur = rec->backrefs.next;
	struct cache_extent *cache;
	struct extent_backref *back;
	int allocated = 0;
	u64 flags = 0;

	/* remember our flags for recreating the extent */
	ret = btrfs_lookup_extent_info(NULL, info->extent_root, rec->start,
				       rec->max_size, rec->metadata, NULL,
				       &flags);
	if (ret < 0)
		flags = BTRFS_BLOCK_FLAG_FULL_BACKREF;

	path = btrfs_alloc_path();

	/* step one, delete all the existing records */
	ret = delete_extent_records(trans, info->extent_root, path,
				    rec->start, rec->max_size);

	if (ret < 0)
		goto out;

	/* was this block corrupt?  If so, don't add references to it */
	cache = find_cache_extent(info->corrupt_blocks, rec->start, rec->max_size);
	if (cache) {
		ret = 0;
		goto out;
	}

	/* step two, recreate all the refs we did find */
	while(cur != &rec->backrefs) {
		back = list_entry(cur, struct extent_backref, list);
		cur = cur->next;

		/*
		 * if we didn't find any references, don't create a
		 * new extent record
		 */
		if (!back->found_ref)
			continue;

		ret = record_extent(trans, info, path, rec, back, allocated, flags);
		allocated = 1;

		if (ret)
			goto out;
	}
out:
	btrfs_free_path(path);
	return ret;
}

/* right now we only prune from the extent allocation tree */
static int prune_one_block(struct btrfs_trans_handle *trans,
			   struct btrfs_fs_info *info,
			   struct btrfs_corrupt_block *corrupt)
{
	int ret;
	struct btrfs_path path;
	struct extent_buffer *eb;
	u64 found;
	int slot;
	int nritems;
	int level = corrupt->level + 1;

	btrfs_init_path(&path);
again:
	/* we want to stop at the parent to our busted block */
	path.lowest_level = level;

	ret = btrfs_search_slot(trans, info->extent_root,
				&corrupt->key, &path, -1, 1);

	if (ret < 0)
		goto out;

	eb = path.nodes[level];
	if (!eb) {
		ret = -ENOENT;
		goto out;
	}

	/*
	 * hopefully the search gave us the block we want to prune,
	 * lets try that first
	 */
	slot = path.slots[level];
	found =  btrfs_node_blockptr(eb, slot);
	if (found == corrupt->cache.start)
		goto del_ptr;

	nritems = btrfs_header_nritems(eb);

	/* the search failed, lets scan this node and hope we find it */
	for (slot = 0; slot < nritems; slot++) {
		found =  btrfs_node_blockptr(eb, slot);
		if (found == corrupt->cache.start)
			goto del_ptr;
	}
	/*
	 * we couldn't find the bad block.  TODO, search all the nodes for pointers
	 * to this block
	 */
	if (eb == info->extent_root->node) {
		ret = -ENOENT;
		goto out;
	} else {
		level++;
		btrfs_release_path(NULL, &path);
		goto again;
	}

del_ptr:
	printk("deleting pointer to block %Lu\n", corrupt->cache.start);
	ret = btrfs_del_ptr(trans, info->extent_root, &path, level, slot);

out:
	btrfs_release_path(NULL, &path);
	return ret;
}

static int prune_corrupt_blocks(struct btrfs_trans_handle *trans,
				struct btrfs_fs_info *info)
{
	struct cache_extent *cache;
	struct btrfs_corrupt_block *corrupt;

	cache = find_first_cache_extent(info->corrupt_blocks, 0);
	while (1) {
		if (!cache)
			break;
		corrupt = container_of(cache, struct btrfs_corrupt_block, cache);
		prune_one_block(trans, info, corrupt);
		cache = next_cache_extent(cache);
	}
	return 0;
}

static void free_corrupt_blocks(struct btrfs_fs_info *info)
{
	struct cache_extent *cache;
	struct btrfs_corrupt_block *corrupt;

	while (1) {
		cache = find_first_cache_extent(info->corrupt_blocks, 0);
		if (!cache)
			break;
		corrupt = container_of(cache, struct btrfs_corrupt_block, cache);
		remove_cache_extent(info->corrupt_blocks, cache);
		free(corrupt);
	}
}

static int check_block_group(struct btrfs_trans_handle *trans,
			      struct btrfs_fs_info *info,
			      struct map_lookup *map,
			      int *reinit)
{
	struct btrfs_key key;
	struct btrfs_path path;
	int ret;

	key.objectid = map->ce.start;
	key.offset = map->ce.size;
	key.type = BTRFS_BLOCK_GROUP_ITEM_KEY;

	btrfs_init_path(&path);
	ret = btrfs_search_slot(NULL, info->extent_root,
				&key, &path, 0, 0);
	btrfs_release_path(NULL, &path);
	if (ret <= 0)
		goto out;

	ret = btrfs_make_block_group(trans, info->extent_root, 0, map->type,
			       BTRFS_FIRST_CHUNK_TREE_OBJECTID,
			       key.objectid, key.offset);
	*reinit = 1;
out:
	return ret;
}

static int check_block_groups(struct btrfs_trans_handle *trans,
			      struct btrfs_fs_info *info, int *reinit)
{
	struct cache_extent *ce;
	struct map_lookup *map;
	struct btrfs_mapping_tree *map_tree = &info->mapping_tree;

	/* this isn't quite working */
	return 0;

	ce = find_first_cache_extent(&map_tree->cache_tree, 0);
	while (1) {
		if (!ce)
			break;
		map = container_of(ce, struct map_lookup, ce);
		check_block_group(trans, info, map, reinit);
		ce = next_cache_extent(ce);
	}
	return 0;
}

static int check_extent_refs(struct btrfs_trans_handle *trans,
			     struct btrfs_root *root,
			     struct cache_tree *extent_cache, int repair)
{
	struct extent_record *rec;
	struct cache_extent *cache;
	int err = 0;
	int ret = 0;
	int fixed = 0;
	int reinit = 0;

	if (repair) {
		/*
		 * if we're doing a repair, we have to make sure
		 * we don't allocate from the problem extents.
		 * In the worst case, this will be all the
		 * extents in the FS
		 */
		cache = find_first_cache_extent(extent_cache, 0);
		while(cache) {
			rec = container_of(cache, struct extent_record, cache);
			btrfs_pin_extent(root->fs_info,
					 rec->start, rec->max_size);
			cache = next_cache_extent(cache);
		}

		/* pin down all the corrupted blocks too */
		cache = find_first_cache_extent(root->fs_info->corrupt_blocks, 0);
		while(cache) {
			rec = container_of(cache, struct extent_record, cache);
			btrfs_pin_extent(root->fs_info,
					 rec->start, rec->max_size);
			cache = next_cache_extent(cache);
		}
		prune_corrupt_blocks(trans, root->fs_info);
		check_block_groups(trans, root->fs_info, &reinit);
		if (reinit)
			btrfs_read_block_groups(root->fs_info->extent_root);
	}
	while(1) {
		fixed = 0;
		cache = find_first_cache_extent(extent_cache, 0);
		if (!cache)
			break;
		rec = container_of(cache, struct extent_record, cache);
		if (rec->refs != rec->extent_item_refs) {
			fprintf(stderr, "ref mismatch on [%llu %llu] ",
				(unsigned long long)rec->start,
				(unsigned long long)rec->nr);
			fprintf(stderr, "extent item %llu, found %llu\n",
				(unsigned long long)rec->extent_item_refs,
				(unsigned long long)rec->refs);
			if (!fixed && repair) {
				ret = fixup_extent_refs(trans, root->fs_info, rec);
				if (ret)
					goto repair_abort;
				fixed = 1;
			}
			err = 1;

		}
		if (all_backpointers_checked(rec, 1)) {
			fprintf(stderr, "backpointer mismatch on [%llu %llu]\n",
				(unsigned long long)rec->start,
				(unsigned long long)rec->nr);

			if (!fixed && repair) {
				ret = fixup_extent_refs(trans, root->fs_info, rec);
				if (ret)
					goto repair_abort;
				fixed = 1;
			}

			err = 1;
		}
		if (!rec->owner_ref_checked) {
			fprintf(stderr, "owner ref check failed [%llu %llu]\n",
				(unsigned long long)rec->start,
				(unsigned long long)rec->nr);
			if (!fixed && repair) {
				ret = fixup_extent_refs(trans, root->fs_info, rec);
				if (ret)
					goto repair_abort;
				fixed = 1;
			}
			err = 1;
		}

		remove_cache_extent(extent_cache, cache);
		free_all_extent_backrefs(rec);
		free(rec);
	}
repair_abort:
	if (repair) {
		if (ret) {
			fprintf(stderr, "failed to repair damaged filesystem, aborting\n");
			exit(1);
		} else {
			btrfs_fix_block_accounting(trans, root);
		}
		if (err)
			fprintf(stderr, "repaired damaged extent references\n");
		return ret;
	}
	return err;
}

static int check_extents(struct btrfs_trans_handle *trans,
			 struct btrfs_root *root, int repair)
{
	struct cache_tree extent_cache;
	struct cache_tree seen;
	struct cache_tree pending;
	struct cache_tree reada;
	struct cache_tree nodes;
	struct cache_tree corrupt_blocks;
	struct btrfs_path path;
	struct btrfs_key key;
	struct btrfs_key found_key;
	int ret;
	u64 last = 0;
	struct block_info *bits;
	int bits_nr;
	struct extent_buffer *leaf;
	int slot;
	struct btrfs_root_item ri;

	cache_tree_init(&extent_cache);
	cache_tree_init(&seen);
	cache_tree_init(&pending);
	cache_tree_init(&nodes);
	cache_tree_init(&reada);
	cache_tree_init(&corrupt_blocks);

	if (repair) {
		root->fs_info->fsck_extent_cache = &extent_cache;
		root->fs_info->free_extent_hook = free_extent_hook;
		root->fs_info->corrupt_blocks = &corrupt_blocks;
	}

	bits_nr = 1024;
	bits = malloc(bits_nr * sizeof(struct block_info));
	if (!bits) {
		perror("malloc");
		exit(1);
	}

	add_root_to_pending(root->fs_info->tree_root->node,
			    &extent_cache, &pending, &seen, &nodes,
			    &root->fs_info->tree_root->root_key);

	add_root_to_pending(root->fs_info->chunk_root->node,
			    &extent_cache, &pending, &seen, &nodes,
			    &root->fs_info->chunk_root->root_key);

	btrfs_init_path(&path);
	key.offset = 0;
	key.objectid = 0;
	btrfs_set_key_type(&key, BTRFS_ROOT_ITEM_KEY);
	ret = btrfs_search_slot(NULL, root->fs_info->tree_root,
					&key, &path, 0, 0);
	BUG_ON(ret < 0);
	while(1) {
		leaf = path.nodes[0];
		slot = path.slots[0];
		if (slot >= btrfs_header_nritems(path.nodes[0])) {
			ret = btrfs_next_leaf(root, &path);
			if (ret != 0)
				break;
			leaf = path.nodes[0];
			slot = path.slots[0];
		}
		btrfs_item_key_to_cpu(leaf, &found_key, path.slots[0]);
		if (btrfs_key_type(&found_key) == BTRFS_ROOT_ITEM_KEY) {
			unsigned long offset;
			struct extent_buffer *buf;

			offset = btrfs_item_ptr_offset(leaf, path.slots[0]);
			read_extent_buffer(leaf, &ri, offset, sizeof(ri));
			buf = read_tree_block(root->fs_info->tree_root,
					      btrfs_root_bytenr(&ri),
					      btrfs_level_size(root,
					       btrfs_root_level(&ri)), 0);
			add_root_to_pending(buf, &extent_cache, &pending,
					    &seen, &nodes, &found_key);
			free_extent_buffer(buf);
		}
		path.slots[0]++;
	}
	btrfs_release_path(root, &path);
	while(1) {
		ret = run_next_block(root, bits, bits_nr, &last, &pending,
				     &seen, &reada, &nodes, &extent_cache);
		if (ret != 0)
			break;
	}
	ret = check_extent_refs(trans, root, &extent_cache, repair);

	if (repair) {
		free_corrupt_blocks(root->fs_info);
		root->fs_info->fsck_extent_cache = NULL;
		root->fs_info->free_extent_hook = NULL;
		root->fs_info->corrupt_blocks = NULL;
	}

	free(bits);
	return ret;
}

static struct option long_options[] = {
	{ "super", 1, NULL, 's' },
	{ "repair", 0, NULL, 0 },
	{ "init-csum-tree", 0, NULL, 0 },
	{ "init-extent-tree", 0, NULL, 0 },
	{ 0, 0, 0, 0}
};

const char * const cmd_check_usage[] = {
	"btrfs check [options] <device>",
	"Check an unmounted btrfs filesystem.",
	"",
	"-s|--super <superblock>     use this superblock copy",
	"--repair                    try to repair the filesystem",
	"--init-csum-tree            create a new CRC tree",
	"--init-extent-tree          create a new extent tree",
	NULL
};

int cmd_check(int argc, char **argv)
{
	struct cache_tree root_cache;
	struct btrfs_root *root;
	struct btrfs_fs_info *info;
	struct btrfs_trans_handle *trans = NULL;
	u64 bytenr = 0;
	char uuidbuf[37];
	int ret;
	int num;
	int repair = 0;
	int option_index = 0;
	int init_csum_tree = 0;
	int rw = 0;

	while(1) {
		int c;
		c = getopt_long(argc, argv, "as:", long_options,
				&option_index);
		if (c < 0)
			break;
		switch(c) {
			case 'a': /* ignored */ break;
			case 's':
				num = atol(optarg);
				bytenr = btrfs_sb_offset(num);
				printf("using SB copy %d, bytenr %llu\n", num,
				       (unsigned long long)bytenr);
				break;
			case '?':
			case 'h':
				usage(cmd_check_usage);
		}
		if (option_index == 1) {
			printf("enabling repair mode\n");
			repair = 1;
			rw = 1;
		} else if (option_index == 2) {
			printf("Creating a new CRC tree\n");
			init_csum_tree = 1;
			rw = 1;
		}

	}
	argc = argc - optind;

	if (argc != 1)
		usage(cmd_check_usage);

	radix_tree_init();
	cache_tree_init(&root_cache);

	if((ret = check_mounted(argv[optind])) < 0) {
		fprintf(stderr, "Could not check mount status: %s\n", strerror(-ret));
		return ret;
	} else if(ret) {
		fprintf(stderr, "%s is currently mounted. Aborting.\n", argv[optind]);
		return -EBUSY;
	}

	info = open_ctree_fs_info(argv[optind], bytenr, 0, rw, 1);
	if (!info) {
		fprintf(stderr, "Couldn't open file system\n");
		return -EIO;
	}

	uuid_unparse(info->super_copy->fsid, uuidbuf);
	printf("Checking filesystem on %s\nUUID: %s\n", argv[optind], uuidbuf);

	if (!extent_buffer_uptodate(info->tree_root->node) ||
	    !extent_buffer_uptodate(info->dev_root->node) ||
	    !extent_buffer_uptodate(info->extent_root->node) ||
	    !extent_buffer_uptodate(info->chunk_root->node)) {
		fprintf(stderr, "Critical roots corrupted, unable to fsck the FS\n");
		return -EIO;
	}

	root = info->fs_root;

	fprintf(stderr, "checking extents\n");
	if (rw)
		trans = btrfs_start_transaction(root, 1);

	if (init_csum_tree) {
		fprintf(stderr, "Reinit crc root\n");
		ret = btrfs_fsck_reinit_root(trans, info->csum_root);
		if (ret) {
			fprintf(stderr, "crc root initialization failed\n");
			return -EIO;
		}
		goto out;
	}
	ret = check_extents(trans, root, repair);
	if (ret)
		fprintf(stderr, "Errors found in extent allocation tree\n");

	fprintf(stderr, "checking free space cache\n");
	ret = check_space_cache(root);
	if (ret)
		goto out;

	fprintf(stderr, "checking fs roots\n");
	ret = check_fs_roots(root, &root_cache);
	if (ret)
		goto out;

	fprintf(stderr, "checking csums\n");
	ret = check_csums(root);
	if (ret)
		goto out;

	fprintf(stderr, "checking root refs\n");
	ret = check_root_refs(root, &root_cache);
out:
	free_root_recs(&root_cache);
	if (rw) {
		ret = btrfs_commit_transaction(trans, root);
		if (ret)
			exit(1);
	}
	close_ctree(root);

	if (found_old_backref) { /*
		 * there was a disk format change when mixed
		 * backref was in testing tree. The old format
		 * existed about one week.
		 */
		printf("\n * Found old mixed backref format. "
		       "The old format is not supported! *"
		       "\n * Please mount the FS in readonly mode, "
		       "backup data and re-format the FS. *\n\n");
		ret = 1;
	}
	printf("found %llu bytes used err is %d\n",
	       (unsigned long long)bytes_used, ret);
	printf("total csum bytes: %llu\n",(unsigned long long)total_csum_bytes);
	printf("total tree bytes: %llu\n",
	       (unsigned long long)total_btree_bytes);
	printf("total fs tree bytes: %llu\n",
	       (unsigned long long)total_fs_tree_bytes);
	printf("total extent tree bytes: %llu\n",
	       (unsigned long long)total_extent_tree_bytes);
	printf("btree space waste bytes: %llu\n",
	       (unsigned long long)btree_space_waste);
	printf("file data blocks allocated: %llu\n referenced %llu\n",
		(unsigned long long)data_bytes_allocated,
		(unsigned long long)data_bytes_referenced);
	printf("%s\n", BTRFS_BUILD_VERSION);
	return ret;
}
