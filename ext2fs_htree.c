#include <sys/param.h>
#include <sys/systm.h>
#include <sys/resourcevar.h>
#include <sys/kernel.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/buf.h>
#include <sys/proc.h>
#include <sys/mount.h>
#include <sys/namei.h>
#include <sys/vnode.h>
#include <sys/lockf.h>
#include <sys/pool.h>
#include <sys/signalvar.h>
#include <sys/kauth.h>

#include <ufs/ufs/dir.h>

#include <ufs/ufs/inode.h>
#include <ufs/ufs/ufsmount.h>
#include <ufs/ext2fs/ext2fs.h>

#include <ufs/ext2fs/ext2fs_extern.h>
#include <ufs/ext2fs/ext2fs_dinode.h>
#include <ufs/ext2fs/ext2fs_dir.h>
#include <ufs/ext2fs/ext2fs_htree.h>




int
ext2fs_htree_has_idx(struct inode *ip)
{
	//if (( ip->inode_u.e2fs->e2fs.e2fs_features_compat & htole32(EXT2F_COMPAT_DIRHASHINDEX) )&&
	int temp1=EXT2_HAS_COMPAT_FEATURE(ip->i_e2fs, EXT2F_COMPAT_DIRHASHINDEX);
	int temp2= ip->i_din.e2fs_din->e2di_flags  & EXT4_INDEX;
	printf ("hash compatible: %x  and directoryhash enable: %x  value of ip->i_flag: %x and EXT4_INDEX:%x\n",temp1,temp2,ip->i_din.e2fs_din->e2di_flags ,EXT4_INDEX);
	if (temp1 &&temp2  )
		return (1);
	else
		return (0);
}

static off_t
ext2fs_htree_get_block(struct ext2fs_htree_entry *ep)
{
	return (ep->h_blk & 0x00FFFFFF);
}


static void
ext2fs_htree_release(struct ext2fs_htree_lookup_info *info)
{
	u_int i;

	for (i = 0; i < info->h_levels_num; i++) {
		struct buf *bp = info->h_levels[i].h_bp;
		if (bp != NULL)
			brelse(bp, 0);
	}
}




static uint16_t
ext2fs_htree_get_limit(struct ext2fs_htree_entry *ep)
{
	return (((struct ext2fs_htree_count *)(ep))->h_entries_max);
}


static uint32_t
ext2fs_htree_root_limit(struct inode *ip, int len)
{
	uint32_t space;

	space = ip->i_e2fs->e2fs_bsize - EXT2_DIR_REC_LEN(1) -
	    EXT2_DIR_REC_LEN(2) - len;
	return (space / sizeof(struct ext2fs_htree_entry));
}


static uint16_t
ext2fs_htree_get_count(struct ext2fs_htree_entry *ep)
{
	return (((struct ext2fs_htree_count *)(ep))->h_entries_num);
}


static uint32_t
ext2fs_htree_get_hash(struct ext2fs_htree_entry *ep)
{
	return (ep->h_hash);
}




static int
ext2fs_htree_check_next(struct inode *ip, uint32_t hash, const char *name,
		struct ext2fs_htree_lookup_info *info)
{
	struct vnode *vp = ITOV(ip);
	struct ext2fs_htree_lookup_level *level;
	struct buf *bp;
	uint32_t next_hash;
	int idx = info->h_levels_num - 1;
	int levels = 0;

	do {
		level = &info->h_levels[idx];
		level->h_entry++;
		if (level->h_entry < level->h_entries +
		    ext2fs_htree_get_count(level->h_entries))
			break;
		if (idx == 0)
			return (0);
		idx--;
		levels++;
	} while (1);

	next_hash = ext2fs_htree_get_hash(level->h_entry);
	if ((hash & 1) == 0) {
		if (hash != (next_hash & ~1))
			return (0);
	}

	while (levels > 0) {
		levels--;
		if (ext2fs_blkatoff(vp, ext2fs_htree_get_block(level->h_entry) *
		    ip->i_e2fs->e2fs_bsize, NULL, &bp) != 0)
			return (0);
		level = &info->h_levels[idx + 1];
		brelse(level->h_bp,0);
		level->h_bp = bp;
		level->h_entry = level->h_entries =
		    ((struct ext2fs_htree_node *)bp->b_data)->h_entries;
	}

	return (1);
}


static int
ext2fs_htree_find_leaf(struct inode *ip, const char *name, int namelen,
		     uint32_t *hash, uint8_t *hash_ver,
		     struct ext2fs_htree_lookup_info *info)
{
	struct vnode *vp;
	struct ext2fs *fs;/* F, G, and H are MD4 functions */
#define F(x, y, z) (((x) & (y)) | ((~x) & (z)))
#define G(x, y, z) (((x) & (y)) | ((x) & (z)) | ((y) & (z)))
#define H(x, y, z) ((x) ^ (y) ^ (z))

/* ROTATE_LEFT rotates x left n bits */
#define ROTATE_LEFT(x, n) (((x) << (n)) | ((x) >> (32 - (n))))
	struct m_ext2fs *m_fs;
	struct buf *bp = NULL;
	struct ext2fs_htree_root *rootp;
	struct ext2fs_htree_entry *entp, *start, *end, *middle, *found;
	struct ext2fs_htree_lookup_level *level_info;
	uint32_t hash_major = 0, hash_minor = 0;
	uint32_t levels, cnt;
	uint8_t hash_version;

	if (name == NULL || info == NULL)
		return (-1);

	vp = ITOV(ip);
	fs = &(ip->i_e2fs->e2fs);
	m_fs = ip->i_e2fs;

	if (ext2fs_blkatoff(vp, 0, NULL, &bp) != 0)
		return (-1);

	info->h_levels_num = 1;
	info->h_levels[0].h_bp = bp;
	rootp = (struct ext2fs_htree_root *)bp->b_data;
	if (rootp->h_info.h_hash_version != EXT2_HTREE_LEGACY &&
	    rootp->h_info.h_hash_version != EXT2_HTREE_HALF_MD4 &&
	    rootp->h_info.h_hash_version != EXT2_HTREE_TEA)
		goto error;

	hash_version = rootp->h_info.h_hash_version;
	if (hash_version <= EXT2_HTREE_TEA)
		hash_version += m_fs->e2fs_uhash;
	*hash_ver = hash_version;

	ext2fs_htree_hash(name, namelen, fs->e3fs_hash_seed,
	    hash_version, &hash_major, &hash_minor);
	*hash = hash_major;

	if ((levels = rootp->h_info.h_ind_levels) > 1)
		goto error;

	entp = (struct ext2fs_htree_entry *)(((char *)&rootp->h_info) +
	    rootp->h_info.h_info_len);

	if (ext2fs_htree_get_limit(entp) !=
	    ext2fs_htree_root_limit(ip, rootp->h_info.h_info_len))
		goto error;

	while (1) {
		cnt = ext2fs_htree_get_count(entp);
		if (cnt == 0 || cnt > ext2fs_htree_get_limit(entp))
			goto error;

		start = entp + 1;
		end = entp + cnt - 1;
		while (start <= end) {
			middle = start + (end - start) / 2;
			if (ext2fs_htree_get_hash(middle) > hash_major)
				end = middle - 1;
			else
				start = middle + 1;
		}
		found = start - 1;

		level_info = &(info->h_levels[info->h_levels_num - 1]);
		level_info->h_bp = bp;
		level_info->h_entries = entp;
		level_info->h_entry = found;
		if (levels == 0)
			return (0);
		levels--;
		if (ext2fs_blkatoff(vp,
		    ext2fs_htree_get_block(found) * m_fs->e2fs_bsize,
		    NULL, &bp) != 0)
			goto error;
		entp = ((struct ext2fs_htree_node *)bp->b_data)->h_entries;
		info->h_levels_num++;
		info->h_levels[info->h_levels_num - 1].h_bp = bp;
	}

error:
	ext2fs_htree_release(info);
	return (-1);
}

/*
 * Try to lookup a directory entry in HTree index
 */
int
ext2fs_htree_lookup(struct inode *ip, const char *name, int namelen,
		  struct buf **bpp, int *entryoffp, doff_t *offp,
		  doff_t *prevoffp, doff_t *endusefulp,
		  struct ext2fs_searchslot *ss)
{
	printf("in fun: %s,lineno: %d\n", __func__, __LINE__);
	struct vnode *vp;
	struct ext2fs_htree_lookup_info info;
	struct ext2fs_htree_entry *leaf_node;
	struct m_ext2fs *m_fs;
	struct buf *bp;
	uint32_t blk;
	uint32_t dirhash;
	uint32_t bsize;
	uint8_t hash_version;
	int search_next;
	int found = 0;

	m_fs = ip->i_e2fs;
	bsize = m_fs->e2fs_bsize;
	vp = ITOV(ip);

	/* TODO: print error msg because we don't lookup '.' and '..' */

	memset(&info, 0, sizeof(info));
	if (ext2fs_htree_find_leaf(ip, name, namelen, &dirhash,
	    &hash_version, &info)) {
		printf("in fun: %s,lineno: %d\n", __func__, __LINE__);
		return (-1);
	}

	do {
		leaf_node = info.h_levels[info.h_levels_num - 1].h_entry;
		blk = ext2fs_htree_get_block(leaf_node);
		if (ext2fs_blkatoff(vp, blk * bsize, NULL, &bp) != 0) {
			ext2fs_htree_release(&info);
			printf("in fun: %s,lineno: %d\n", __func__, __LINE__);
			return (-1);
		}

		*offp = blk * bsize;
		*entryoffp = 0;
		*prevoffp = blk * bsize;
		*endusefulp = blk * bsize;

		if (ss->slotstatus == NONE) {
			ss->slotoffset = -1;
			ss->slotfreespace = 0;
		}

		if (ext2fs_search_dirblock(ip, bp->b_data, &found,
		    name, namelen, entryoffp, offp, prevoffp,
		    endusefulp, ss) != 0) {
			brelse(bp,0);
			ext2fs_htree_release(&info);
			printf("in fun: %s,lineno: %d\n", __func__, __LINE__);
			return (-1);
		}

		if (found) {
			printf("in fun: %s,lineno: %d\n", __func__, __LINE__);
			*bpp = bp;
			ext2fs_htree_release(&info);
			return (0);
		}

		brelse(bp,0);
		search_next = ext2fs_htree_check_next(ip, dirhash, name, &info);
	} while (search_next);

	ext2fs_htree_release(&info);
	return (ENOENT);
}



