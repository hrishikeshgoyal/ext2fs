#include <sys/param.h>
#include <sys/systm.h>
#include <sys/resourcevar.h>
#include <sys/kernel.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/buf.h>
#include <sys/proc.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/signalvar.h>
#include <sys/kauth.h>

#include <ufs/ufs/inode.h>
#include <ufs/ufs/ufsmount.h>
#include <ufs/ufs/ufs_extern.h>


#include <ufs/ext2fs/ext2fs.h>
#include <ufs/ext2fs/ext2fs_extents.h>
#include <ufs/ext2fs/ext2fs_extern.h>



static bool
ext4_ext_binsearch_index(struct inode *ip, struct ext4_extent_path *path,
		daddr_t lbn, daddr_t *first_lbn, daddr_t *last_lbn)
{
	printf("ext4_ext_binsearch_index\n");
	struct ext4_extent_header *ehp = path->ep_header;
	struct ext4_extent_index *first, *last, *l, *r, *m;

	first = (struct ext4_extent_index *)(char *)(ehp + 1);
	last = first + ehp->eh_ecount - 1;
	l = first;
	r = last;
	while (l <= r) {
		m = l + (r - l) / 2;
		if (lbn < m->ei_blk)
			r = m - 1;
		else
			l = m + 1;
	}

	if (l == first) {
		path->ep_sparse_ext.e_blk = *first_lbn;
		path->ep_sparse_ext.e_len = first->ei_blk - *first_lbn;
		path->ep_sparse_ext.e_start_hi = 0;
		path->ep_sparse_ext.e_start_lo = 0;
		path->ep_is_sparse = true;
		return (true);
	}
	path->ep_index = l - 1;
	*first_lbn = path->ep_index->ei_blk;
	if (path->ep_index < last)
		*last_lbn = l->ei_blk - 1;
	return (false);
}

static void
ext4_ext_binsearch(struct inode *ip, struct ext4_extent_path *path, daddr_t lbn,
		daddr_t first_lbn, daddr_t last_lbn)
{
	printf("ext4_ext_binsearch\n");
	struct ext4_extent_header *ehp = path->ep_header;
	struct ext4_extent *first, *l, *r, *m;

	if (ehp->eh_ecount == 0)
		return;

	first = (struct ext4_extent *)(char *)(ehp + 1);
	l = first;
	r = first + ehp->eh_ecount - 1;
	while (l <= r) {
		m = l + (r - l) / 2;
		if (lbn < m->e_blk)
			r = m - 1;
		else
			l = m + 1;
	}

	if (l == first) {
		path->ep_sparse_ext.e_blk = first_lbn;
		path->ep_sparse_ext.e_len = first->e_blk - first_lbn;
		path->ep_sparse_ext.e_start_hi = 0;
		path->ep_sparse_ext.e_start_lo = 0;
		path->ep_is_sparse = true;
		return;
	}
	path->ep_ext = l - 1;
	if (path->ep_ext->e_blk + path->ep_ext->e_len <= lbn) {
		path->ep_sparse_ext.e_blk = path->ep_ext->e_blk +
		    path->ep_ext->e_len;
		if (l <= (first + ehp->eh_ecount - 1))
			path->ep_sparse_ext.e_len = l->e_blk -
			    path->ep_sparse_ext.e_blk;
		else
			path->ep_sparse_ext.e_len = last_lbn -
			    path->ep_sparse_ext.e_blk + 1;
		path->ep_sparse_ext.e_start_hi = 0;
		path->ep_sparse_ext.e_start_lo = 0;
		path->ep_is_sparse = true;
	}
}

/*
 * Find a block in ext4 extent cache.
 */
int
ext4_ext_in_cache(struct inode *ip, daddr_t lbn, struct ext4_extent *ep)
{
	printf("ext4_ext_in_cache\n");
	struct ext4_extent_cache *ecp;
	int ret = EXT4_EXT_CACHE_NO;

	ecp = &ip->inode_ext.e2fs.i_ext_cache;

	/* cache is invalid */
	if (ecp->ec_type == EXT4_EXT_CACHE_NO)
		return (ret);

	if (lbn >= ecp->ec_blk && lbn < ecp->ec_blk + ecp->ec_len) {
		ep->e_blk = ecp->ec_blk;
		ep->e_start_lo = ecp->ec_start & 0xffffffff;
		ep->e_start_hi = ecp->ec_start >> 32 & 0xffff;
		ep->e_len = ecp->ec_len;
		ret = ecp->ec_type;
	}
	return (ret);
}

/*
 * Put an ext4_extent structure in ext4 cache.
 */
void
ext4_ext_put_cache(struct inode *ip, struct ext4_extent *ep, int type)
{
	printf("ext4_ext_put_cache\n");
	struct ext4_extent_cache *ecp;

	ecp = &ip->inode_ext.e2fs.i_ext_cache;
	ecp->ec_type = type;
	ecp->ec_blk = ep->e_blk;
	ecp->ec_len = ep->e_len;
	ecp->ec_start = (daddr_t)ep->e_start_hi << 32 | ep->e_start_lo;
}

/*
 * Find an extent.
 */
struct ext4_extent_path *
ext4_ext_find_extent(struct m_ext2fs *fs, struct inode *ip,
		     daddr_t lbn, struct ext4_extent_path *path)
{
	printf("inside ext4_ext_find_extent\n");
	struct ext4_extent_header *ehp;
	uint16_t i;
	int error, size;
	daddr_t nblk;

	ehp = (struct ext4_extent_header *)(char *)ip->i_din.e2fs_din->e2di_blocks;

	if (ehp->eh_magic != EXT4_EXT_MAGIC)
		return (NULL);

	path->ep_header = ehp;

	daddr_t first_lbn = 0;
	daddr_t last_lbn = lblkno(ip->i_e2fs, ip->i_size);

	for (i = ehp->eh_depth; i != 0; --i) {
		path->ep_depth = i;
		path->ep_ext = NULL;
		if (ext4_ext_binsearch_index(ip, path, lbn, &first_lbn,
		    &last_lbn)) {
			return (path);
		}

		nblk = (daddr_t)path->ep_index->ei_leaf_hi << 32 |
		    path->ep_index->ei_leaf_lo;
		size = blksize(fs, ip, nblk);
		if (path->ep_bp != NULL) {
			brelse(path->ep_bp,0);
			path->ep_bp = NULL;
		}
		error = bread(ip->i_devvp, fsbtodb(fs, nblk), size, 0,
			    &path->ep_bp);
		if (error) {
			brelse(path->ep_bp,0);
			path->ep_bp = NULL;
			return (NULL);
		}
		ehp = (struct ext4_extent_header *)path->ep_bp->b_data;
		path->ep_header = ehp;
	}

	path->ep_depth = i;
	path->ep_ext = NULL;
	path->ep_index = NULL;
	path->ep_is_sparse = false;

	ext4_ext_binsearch(ip, path, lbn, first_lbn, last_lbn);
	return (path);
}
