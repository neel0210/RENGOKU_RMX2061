/* SPDX-License-Identifier: GPL-2.0
 *
 * linux/drivers/staging/erofs/internal.h
 *
 * Copyright (C) 2017-2018 HUAWEI, Inc.
 *             http://www.huawei.com/
 * Created by Gao Xiang <gaoxiang25@huawei.com>
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file COPYING in the main directory of the Linux
 * distribution for more details.
 */
#ifndef __INTERNAL_H
#define __INTERNAL_H

#include <linux/fs.h>
#include <linux/dcache.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/bio.h>
#include <linux/buffer_head.h>
#include <linux/cleancache.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include "erofs_fs.h"

/* redefine pr_fmt "erofs: " */
#undef pr_fmt
#define pr_fmt(fmt) "erofs: " fmt

#define errln(x, ...)   pr_err(x "\n", ##__VA_ARGS__)
#define infoln(x, ...)  pr_info(x "\n", ##__VA_ARGS__)
#ifdef CONFIG_EROFS_FS_DEBUG
#define debugln(x, ...) pr_debug(x "\n", ##__VA_ARGS__)

#define dbg_might_sleep         might_sleep
#define DBG_BUGON               BUG_ON
#else
#define debugln(x, ...)         ((void)0)

#define dbg_might_sleep()       ((void)0)
#define DBG_BUGON(...)          ((void)0)
#endif

#ifdef CONFIG_EROFS_FAULT_INJECTION
enum {
	FAULT_KMALLOC,
	FAULT_MAX,
};

extern char *erofs_fault_name[FAULT_MAX];
#define IS_FAULT_SET(fi, type) ((fi)->inject_type & (1 << (type)))

struct erofs_fault_info {
	atomic_t inject_ops;
	unsigned int inject_rate;
	unsigned int inject_type;
};
#endif

/* EROFS_SUPER_MAGIC_V1 to represent the whole file system */
#define EROFS_SUPER_MAGIC   EROFS_SUPER_MAGIC_V1

typedef u64 erofs_nid_t;

struct erofs_sb_info {
	u32 blocks;
	u32 meta_blkaddr;
#ifdef CONFIG_EROFS_FS_XATTR
	u32 xattr_blkaddr;
#endif

	/* inode slot unit size in bit shift */
	unsigned char islotbits;
#ifdef CONFIG_EROFS_FS_ZIP
	/* cluster size in bit shift */
	unsigned char clusterbits;
#endif

	u32 build_time_nsec;
	u64 build_time;

	/* what we really care is nid, rather than ino.. */
	erofs_nid_t root_nid;
	/* used for statfs, f_files - f_favail */
	u64 inos;

	u8 uuid[16];                    /* 128-bit uuid for volume */
	u8 volume_name[16];             /* volume name */
	char *dev_name;

	unsigned int mount_opt;

#ifdef CONFIG_EROFS_FAULT_INJECTION
	struct erofs_fault_info fault_info;	/* For fault injection */
#endif
};

#ifdef CONFIG_EROFS_FAULT_INJECTION
#define erofs_show_injection_info(type)					\
	infoln("inject %s in %s of %pS", erofs_fault_name[type],        \
		__func__, __builtin_return_address(0))

static inline bool time_to_inject(struct erofs_sb_info *sbi, int type)
{
	struct erofs_fault_info *ffi = &sbi->fault_info;

	if (!ffi->inject_rate)
		return false;

	if (!IS_FAULT_SET(ffi, type))
		return false;

	atomic_inc(&ffi->inject_ops);
	if (atomic_read(&ffi->inject_ops) >= ffi->inject_rate) {
		atomic_set(&ffi->inject_ops, 0);
		return true;
	}
	return false;
}
#endif

static inline void *erofs_kmalloc(struct erofs_sb_info *sbi,
					size_t size, gfp_t flags)
{
#ifdef CONFIG_EROFS_FAULT_INJECTION
	if (time_to_inject(sbi, FAULT_KMALLOC)) {
		erofs_show_injection_info(FAULT_KMALLOC);
		return NULL;
	}
#endif
	return kmalloc(size, flags);
}

#define EROFS_SB(sb) ((struct erofs_sb_info *)(sb)->s_fs_info)
#define EROFS_I_SB(inode) ((struct erofs_sb_info *)(inode)->i_sb->s_fs_info)

/* Mount flags set via mount options or defaults */
#define EROFS_MOUNT_XATTR_USER		0x00000010
#define EROFS_MOUNT_POSIX_ACL		0x00000020
#define EROFS_MOUNT_FAULT_INJECTION	0x00000040

#define clear_opt(sbi, option)	((sbi)->mount_opt &= ~EROFS_MOUNT_##option)
#define set_opt(sbi, option)	((sbi)->mount_opt |= EROFS_MOUNT_##option)
#define test_opt(sbi, option)	((sbi)->mount_opt & EROFS_MOUNT_##option)

/* we strictly follow PAGE_SIZE and no buffer head yet */
#define LOG_BLOCK_SIZE		PAGE_SHIFT

#undef LOG_SECTORS_PER_BLOCK
#define LOG_SECTORS_PER_BLOCK	(PAGE_SHIFT - 9)

#undef SECTORS_PER_BLOCK
#define SECTORS_PER_BLOCK	(1 << SECTORS_PER_BLOCK)

#define EROFS_BLKSIZ		(1 << LOG_BLOCK_SIZE)

#if (EROFS_BLKSIZ % 4096 || !EROFS_BLKSIZ)
#error erofs cannot be used in this platform
#endif

#define ROOT_NID(sb)		((sb)->root_nid)

typedef u64 erofs_off_t;

/* data type for filesystem-wide blocks number */
typedef u32 erofs_blk_t;

#define erofs_blknr(addr)       ((addr) / EROFS_BLKSIZ)
#define erofs_blkoff(addr)      ((addr) % EROFS_BLKSIZ)
#define blknr_to_addr(nr)       ((erofs_off_t)(nr) * EROFS_BLKSIZ)

static inline erofs_off_t iloc(struct erofs_sb_info *sbi, erofs_nid_t nid)
{
	return blknr_to_addr(sbi->meta_blkaddr) + (nid << sbi->islotbits);
}

#define inode_set_inited_xattr(inode)   (EROFS_V(inode)->flags |= 1)
#define inode_has_inited_xattr(inode)   (EROFS_V(inode)->flags & 1)

struct erofs_vnode {
	erofs_nid_t nid;
	unsigned int flags;

	unsigned char data_mapping_mode;
	/* inline size in bytes */
	unsigned char inode_isize;
	unsigned short xattr_isize;

	unsigned xattr_shared_count;
	unsigned *xattr_shared_xattrs;

	erofs_blk_t raw_blkaddr;

	/* the corresponding vfs inode */
	struct inode vfs_inode;
};

#define EROFS_V(ptr)	\
	container_of(ptr, struct erofs_vnode, vfs_inode)

#define __inode_advise(x, bit, bits) \
	(((x) >> (bit)) & ((1 << (bits)) - 1))

#define __inode_version(advise)	\
	__inode_advise(advise, EROFS_I_VERSION_BIT,	\
		EROFS_I_VERSION_BITS)

#define __inode_data_mapping(advise)	\
	__inode_advise(advise, EROFS_I_DATA_MAPPING_BIT,\
		EROFS_I_DATA_MAPPING_BITS)

static inline unsigned long inode_datablocks(struct inode *inode)
{
	/* since i_size cannot be changed */
	return DIV_ROUND_UP(inode->i_size, EROFS_BLKSIZ);
}

static inline bool is_inode_layout_plain(struct inode *inode)
{
	return EROFS_V(inode)->data_mapping_mode == EROFS_INODE_LAYOUT_PLAIN;
}

static inline bool is_inode_layout_compression(struct inode *inode)
{
	return EROFS_V(inode)->data_mapping_mode ==
					EROFS_INODE_LAYOUT_COMPRESSION;
}

static inline bool is_inode_layout_inline(struct inode *inode)
{
	return EROFS_V(inode)->data_mapping_mode == EROFS_INODE_LAYOUT_INLINE;
}

extern const struct super_operations erofs_sops;
extern const struct inode_operations erofs_dir_iops;
extern const struct file_operations erofs_dir_fops;

extern const struct address_space_operations erofs_raw_access_aops;

/*
 * Logical to physical block mapping, used by erofs_map_blocks()
 *
 * Different with other file systems, it is used for 2 access modes:
 *
 * 1) RAW access mode:
 *
 * Users pass a valid (m_lblk, m_lofs -- usually 0) pair,
 * and get the valid m_pblk, m_pofs and the longest m_len(in bytes).
 *
 * Note that m_lblk in the RAW access mode refers to the number of
 * the compressed ondisk block rather than the uncompressed
 * in-memory block for the compressed file.
 *
 * m_pofs equals to m_lofs except for the inline data page.
 *
 * 2) Normal access mode:
 *
 * If the inode is not compressed, it has no difference with
 * the RAW access mode. However, if the inode is compressed,
 * users should pass a valid (m_lblk, m_lofs) pair, and get
 * the needed m_pblk, m_pofs, m_len to get the compressed data
 * and the updated m_lblk, m_lofs which indicates the start
 * of the corresponding uncompressed data in the file.
 */
enum {
	BH_Zipped = BH_PrivateStart,
};

/* Has a disk mapping */
#define EROFS_MAP_MAPPED	(1 << BH_Mapped)
/* Located in metadata (could be copied from bd_inode) */
#define EROFS_MAP_META		(1 << BH_Meta)
/* The extent has been compressed */
#define EROFS_MAP_ZIPPED	(1 << BH_Zipped)

struct erofs_map_blocks {
	erofs_off_t m_pa, m_la;
	u64 m_plen, m_llen;

	unsigned int m_flags;
};

/* Flags used by erofs_map_blocks() */
#define EROFS_GET_BLOCKS_RAW    0x0001

/* data.c */
extern struct page *erofs_get_meta_page(struct super_block *sb,
	erofs_blk_t blkaddr, bool prio);
extern int erofs_map_blocks(struct inode *, struct erofs_map_blocks *, int);
extern int erofs_map_blocks_iter(struct inode *, struct erofs_map_blocks *,
	struct page **, int);

struct erofs_map_blocks_iter {
	struct erofs_map_blocks map;
	struct page *mpage;
};


static inline struct page *erofs_get_inline_page(struct inode *inode,
	erofs_blk_t blkaddr)
{
	return erofs_get_meta_page(inode->i_sb,
		blkaddr, S_ISDIR(inode->i_mode));
}

/* inode.c */
extern struct inode *erofs_iget(struct super_block *sb,
	erofs_nid_t nid, bool dir);

/* dir.c */
int erofs_namei(struct inode *dir, struct qstr *name,
	erofs_nid_t *nid, unsigned *d_type);

/* xattr.c */
#ifdef CONFIG_EROFS_FS_XATTR
extern const struct xattr_handler *erofs_xattr_handlers[];
#endif

/* symlink */
#ifdef CONFIG_EROFS_FS_XATTR
extern const struct inode_operations erofs_symlink_xattr_iops;
extern const struct inode_operations erofs_fast_symlink_xattr_iops;
extern const struct inode_operations erofs_special_inode_operations;
#endif

static inline void set_inode_fast_symlink(struct inode *inode)
{
#ifdef CONFIG_EROFS_FS_XATTR
	inode->i_op = &erofs_fast_symlink_xattr_iops;
#else
	inode->i_op = &simple_symlink_inode_operations;
#endif
}

static inline bool is_inode_fast_symlink(struct inode *inode)
{
#ifdef CONFIG_EROFS_FS_XATTR
	return inode->i_op == &erofs_fast_symlink_xattr_iops;
#else
	return inode->i_op == &simple_symlink_inode_operations;
#endif
}

static inline void *erofs_vmap(struct page **pages, unsigned int count)
{
#ifdef CONFIG_EROFS_FS_USE_VM_MAP_RAM
	int i = 0;

	while (1) {
		void *addr = vm_map_ram(pages, count, -1, PAGE_KERNEL);
		/* retry two more times (totally 3 times) */
		if (addr != NULL || ++i >= 3)
			return addr;
		vm_unmap_aliases();
	}
	return NULL;
#else
	return vmap(pages, count, VM_MAP, PAGE_KERNEL);
#endif
}

static inline void erofs_vunmap(const void *mem, unsigned int count)
{
#ifdef CONFIG_EROFS_FS_USE_VM_MAP_RAM
	vm_unmap_ram(mem, count);
#else
	vunmap(mem);
#endif
}

#endif

