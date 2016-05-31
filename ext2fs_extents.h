#ifndef _FS_EXT2FS_EXT2_EXTENTS_H_
#define	_FS_EXT2FS_EXT2_EXTENTS_H_

#include <sys/types.h>
#include <ufs/ufs/inode.h>
#define	EXT4_EXT_MAGIC  0xf30a

#define	EXT4_EXT_CACHE_NO	0
#define	EXT4_EXT_CACHE_GAP	1
#define	EXT4_EXT_CACHE_IN	2

/*
 * Ext4 file system extent on disk.
 */
struct ext4_extent {
	uint32_t e_blk;	/* first logical block */
	uint16_t e_len;	/* number of blocks */
	uint16_t e_start_hi;	/* high 16 bits of physical block */
	uint32_t e_start_lo;	/* low 32 bits of physical block */
};

/*
 * Extent index on disk.
 */
struct ext4_extent_index {
	uint32_t ei_blk;	/* indexes logical blocks */
	uint32_t ei_leaf_lo;	/* points to physical block of the
				 * next level */
	uint16_t ei_leaf_hi;	/* high 16 bits of physical block */
	uint16_t ei_unused;
};

/*
 * Extent tree header.
 */
struct ext4_extent_header {
	uint16_t eh_magic;	/* magic number: 0xf30a */
	uint16_t eh_ecount;	/* number of valid entries */
	uint16_t eh_max;	/* capacity of store in entries */
	uint16_t eh_depth;	/* the depth of extent tree */
	uint32_t eh_gen;	/* generation of extent tree */
};

/*
 * Save cached extent.
 */
struct ext4_extent_cache {
	daddr_t	ec_start;	/* extent start */
	uint32_t ec_blk;	/* logical block */
	uint32_t ec_len;
	uint32_t ec_type;
};

/*
 * Save path to some extent.
 */
struct ext4_extent_path {
	uint16_t ep_depth;
	struct buf *ep_bp;
	bool ep_is_sparse;
	union {
		struct ext4_extent ep_sparse_ext;
		struct ext4_extent *ep_ext;
	};
	struct ext4_extent_index *ep_index;
	struct ext4_extent_header *ep_header;
};

struct inode;
struct m_ext2fs;
int	ext4_ext_in_cache(struct inode *, daddr_t, struct ext4_extent *);
void	ext4_ext_put_cache(struct inode *, struct ext4_extent *, int);
struct ext4_extent_path *ext4_ext_find_extent(struct m_ext2fs *fs,
    struct inode *, daddr_t, struct ext4_extent_path *);

#endif /* !_FS_EXT2FS_EXT2_EXTENTS_H_ */
