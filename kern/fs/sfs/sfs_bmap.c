/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009, 2014
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * SFS filesystem
 *
 * Block mapping logic.
 */
#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <synch.h>
#include <vfs.h>
#include <buf.h>
#include <sfs.h>
#include "sfsprivate.h"

/*
 * This code implements the direct/indirect block logic, which maps
 * block numbers within a file ("fileblocks") to block numbers on the
 * disk device ("diskblocks").
 *
 * The inode has some number each of direct, indirect, double
 * indirect, and triple indirect pointers. Each of these is a subtree
 * that maps some number of fileblocks (possibly only 1) to
 * diskblocks.
 *
 * The following relevant constants are defined in kern/sfs.h:
 *
 *    SFS_DBPERIDB      The number of direct blocks an indirect block
 *                      maps; equivalently the number of block
 *                      pointers in a disk block.
 *
 *    SFS_NDIRECT       The number of direct block pointers in the
 *                      inode.
 *
 *    SFS_NINDIRECT     The number of singly indirect block pointers
 *                      in the inode.
 *
 *    SFS_NDINDIRECT    The number of doubly indirect block pointers
 *                      in the inode.
 *
 *    SFS_NTINDIRECT    The number of triply indirect block pointers
 *                      in the inode.
 *
 * Traditionally in file systems managed this way, there are multiple
 * direct block pointers but only one indirect block pointer of each
 * depth; however, this does not need to be the case. In theory, you
 * can change the numbers arbitrarily, recompile this file, and have
 * it still work; in practice it probably won't, mostly for syntactic
 * reasons.
 *
 * In order to develop some abstraction in here (and thereby make the
 * code manageable) we develop the following concepts:
 *
 * an INDIRECTION LEVEL is the level of tree indirection, ranging from
 * 0 through 3. The indirection level associated with a block pointer
 * is the number of layers of indirect blocks between it and the data
 * blocks: at indirection level 0 a block pointer is a direct block
 * pointer and points directly to a data block. At indirection level 1
 * a block pointer is a singly indirect block pointer and points to a
 * singly indirect block, a block full of direct block pointers. At
 * indirection level 2, a block pointer is a doubly indirect block
 * pointer and points to a double indirect block full of singly
 * indirect block pointers. And so forth.
 *
 * a SUBTREE REFERENCE is a representation that names one of the
 * subtrees in the inode. This consists of an indirection level, that
 * selects the level of pointers in the inode to use, and an
 * indirection number, that selects which of those pointers to use.
 * For example, the second direct block pointer is represented by
 * indirection level 0 and indirection number 1; the first (typically
 * only) triple indirect block pointer is represented by indirection
 * level 3 and indirection number 0.
 *
 * a TREE LOCATION is a complete representation of a (leaf) position
 * in the block mapping tree. This can be represented in the following
 * equivalent ways:
 *   - a fileblock number;
 *   - which subtree in the inode to use, and a fileblock offset into
 *     that subtree;
 *   - which subtree in the inode to use, and for each layer in that
 *     subtree, an offset into the layer.
 *
 * Much of the logic in here pertains to converting from the first to
 * the second of these tree location representations, as it involves
 * the numbers and sizes of the subtrees and doesn't have a simply
 * computable form. The conversion from the second of these
 * representations to the third just involves division and modulus (or
 * shifting and masking) and can be easily done on the fly.
 *
 * The first representation is just an integer; the second is a
 * subtree reference combined with an integer. The third would be a
 * subtree reference combined with an array of integers, but we don't
 * actually materialize it.
 *
 * We also define a BLOCK OBJECT as an abstract wrapper for an entity
 * that contains block pointers. It can be either an inode or an
 * indirect block of any level. If an inode, it includes the subtree
 * reference to use, so the access offset should always be zero.
 */


/*
 * Subtree reference.
 */
struct sfs_subtreeref {
	unsigned str_indirlevel;	/* Indirection level */
	unsigned str_indirnum;		/* Indirection number */
};

/*
 * Block object.
 */
struct sfs_blockobj {
	bool bo_isinode;
	union {
		struct {
			struct sfs_vnode *i_sv;
			struct sfs_subtreeref i_subtree;
		} bo_inode;
		struct {
			struct buf *id_buf;
		} bo_idblock;
	};
};

////////////////////////////////////////////////////////////
// sfs_subtreeref routines

#if 0 /* not currently used */
/*
 * Maximum block number that we can have in a file.
 */
static const uint32_t sfs_maxblock =
	SFS_NDIRECT +
	SFS_NINDIRECT * SFS_DBPERIDB +
	SFS_NDINDIRECT * SFS_DBPERIDB * SFS_DBPERIDB +
	SFS_NTINDIRECT * SFS_DBPERIDB * SFS_DBPERIDB * SFS_DBPERIDB
;
#endif

/*
 * Find out the indirection level of a file block number; that is,
 * which block pointer in the inode one uses to get to it.
 *
 * FILEBLOCK is the file block number.
 *
 * INDIR_RET returns the indirection level.
 *
 * INDIRNUM_RET returns the index into the inode blocks at that
 * indirection level, e.g. for the 3rd direct block this would be 3.
 *
 * OFFSET_RET returns the block number offset within the tree
 * starting at the designated inode block pointer. For direct blocks
 * this will always be 0.
 *
 * This function has been written so it will continue to work even if
 * SFS_NINDIRECT, SFS_NDINDIRECT, and/or SFS_NTINDIRECT get changed
 * around, although if you do that much of the rest of the code will
 * still need attention.
 *
 * Fails with EFBIG if the requested offset is too large for the
 * filesystem.
 */
static
int
sfs_get_indirection(uint32_t fileblock, struct sfs_subtreeref *subtree_ret,
		    uint32_t *offset_ret)
{
	static const struct {
		unsigned num;
		uint32_t blockseach;
	} info[4] = {
		{ SFS_NDIRECT,    1 },
		{ SFS_NINDIRECT,  SFS_DBPERIDB },
		{ SFS_NDINDIRECT, SFS_DBPERIDB * SFS_DBPERIDB },
		{ SFS_NTINDIRECT, SFS_DBPERIDB * SFS_DBPERIDB * SFS_DBPERIDB },
	};

	unsigned indir;
	uint32_t max;

	for (indir = 0; indir < 4; indir++) {
		max = info[indir].num * info[indir].blockseach;
		if (fileblock < max) {
			subtree_ret->str_indirlevel = indir;
			subtree_ret->str_indirnum =
				fileblock / info[indir].blockseach;
			*offset_ret = fileblock % info[indir].blockseach;
			return 0;
		}
		fileblock -= max;
	}
	return EFBIG;
}

////////////////////////////////////////////////////////////
// sfs_blockobj routines

/*
 * Initialize a blockobj that's a reference to one of the subtrees in
 * the inode.
 */
static
void
sfs_blockobj_init_inode(struct sfs_blockobj *bo,
			struct sfs_vnode *sv, struct sfs_subtreeref *subtree)
{
	bo->bo_isinode = true;
	bo->bo_inode.i_sv = sv;
	bo->bo_inode.i_subtree = *subtree;
}

/*
 * Initialize a blockobj that's an indirect block.
 */
static
void
sfs_blockobj_init_idblock(struct sfs_blockobj *bo,
			  struct buf *idbuf)
{
	bo->bo_isinode = false;
	bo->bo_idblock.id_buf = idbuf;
}

/*
 * Clean up a blockobj. This doesn't need to do anything currently,
 * but it's good practice to leave the hook in place.
 */
static
void
sfs_blockobj_cleanup(struct sfs_blockobj *bo)
{
	(void)bo;
}

/*
 * Get the block value at offset OFFSET in a blockobj. (The offset
 * must be zero if it's an inode blockobj.)
 */
static
uint32_t
sfs_blockobj_get(struct sfs_blockobj *bo, uint32_t offset)
{
	if (bo->bo_isinode) {
		struct sfs_vnode *sv = bo->bo_inode.i_sv;
		struct sfs_fs *sfs = sv->sv_absvn.vn_fs->fs_data;
		struct sfs_dinode *dino;
		unsigned indirlevel, indirnum;

		KASSERT(offset == 0);

		dino = sfs_dinode_map(bo->bo_inode.i_sv);
		indirlevel = bo->bo_inode.i_subtree.str_indirlevel;
		indirnum = bo->bo_inode.i_subtree.str_indirnum;

		switch (indirlevel) {
		    case 0:
			KASSERT(indirnum < SFS_NDIRECT);
			return dino->sfi_direct[indirnum];
		    case 1:
			KASSERT(indirnum == 0);
			return dino->sfi_indirect;
		    case 2:
			KASSERT(indirnum == 0);
			return dino->sfi_dindirect;
		    case 3:
			KASSERT(indirnum == 0);
			return dino->sfi_tindirect;
		    default:
			panic("sfs: %s: sfs_blockobj_get: "
			      "invalid indirection %u\n",
			      sfs->sfs_sb.sb_volname,
			      indirlevel);
		}
		return 0;
	}
	else {
		uint32_t *idptr;

		COMPILE_ASSERT(SFS_DBPERIDB*sizeof(idptr[0]) == SFS_BLOCKSIZE);
		KASSERT(offset < SFS_DBPERIDB);

		idptr = buffer_map(bo->bo_idblock.id_buf);
		return idptr[offset];
	}
}

/*
 * Change the block value at offset OFFSET in a blockobj. (The offset
 * must be zero if it's an inode blockobj.)
 */
static
void
sfs_blockobj_set(struct sfs_blockobj *bo, uint32_t offset, uint32_t newval)
{
	if (bo->bo_isinode) {
		struct sfs_vnode *sv = bo->bo_inode.i_sv;
		struct sfs_fs *sfs = sv->sv_absvn.vn_fs->fs_data;
		struct sfs_dinode *dino;
		unsigned indirlevel, indirnum;

		KASSERT(offset == 0);

		dino = sfs_dinode_map(bo->bo_inode.i_sv);
		indirlevel = bo->bo_inode.i_subtree.str_indirlevel;
		indirnum = bo->bo_inode.i_subtree.str_indirnum;

		switch (indirlevel) {
		    case 0:
			KASSERT(indirnum < SFS_NDIRECT);
			dino->sfi_direct[indirnum] = newval;
			break;
		    case 1:
			KASSERT(indirnum == 0);
			dino->sfi_indirect = newval;
			break;
		    case 2:
			KASSERT(indirnum == 0);
			dino->sfi_dindirect = newval;
			break;
		    case 3:
			KASSERT(indirnum == 0);
			dino->sfi_tindirect = newval;
			break;
		    default:
			panic("sfs: %s: sfs_blockobj_get: "
			      "invalid indirection %u\n",
			      sfs->sfs_sb.sb_volname,
			      indirlevel);
		}
		sfs_dinode_mark_dirty(bo->bo_inode.i_sv);
	}
	else {
		uint32_t *idptr;

		COMPILE_ASSERT(SFS_DBPERIDB*sizeof(idptr[0]) == SFS_BLOCKSIZE);
		KASSERT(offset < SFS_DBPERIDB);

		idptr = buffer_map(bo->bo_idblock.id_buf);
		idptr[offset] = newval;
		buffer_mark_dirty(bo->bo_idblock.id_buf);
	}
}

////////////////////////////////////////////////////////////
// bmap

/*
 * Given a pointer to a block slot, return it, allocating a block
 * if necessary.
 */
static
int
sfs_bmap_get(struct sfs_fs *sfs, struct sfs_blockobj *bo, uint32_t offset,
	     bool doalloc, daddr_t *diskblock_ret)
{
	daddr_t block;
	int result;

	/*
	 * Get the block number
	 */
	block = sfs_blockobj_get(bo, offset);

	/*
	 * Do we need to allocate?
	 */
	if (block==0 && doalloc) {
		result = sfs_balloc(sfs, &block, NULL);
		if (result) {
			return result;
		}

		/* Remember what we allocated; mark storage dirty */
		sfs_blockobj_set(bo, offset, block);
	}

	/*
	 * Hand back the block
	 */
	*diskblock_ret = block;
	return 0;
}

/*
 * Look up the disk block number in a subtree; that is, we've picked
 * one of the block pointers in the inode and we're now going to
 * look up in the tree it points to.
 *
 * INODEOBJ is the abstract reference to the subtree in the inode.
 * INDIR is its indirection level.
 *
 * OFFSET is the block offset into the subtree.
 * DOALLOC is true if we're allocating blocks.
 *
 * DISKBLOCK_RET gets the resulting disk block number.
 *
 * This function would be somewhat tidier if it were recursive, but
 * recursion in the kernel is generally a bad idea because of the
 * available stack size.
 */
static
int
sfs_bmap_subtree(struct sfs_fs *sfs, struct sfs_blockobj *inodeobj,
		 unsigned indir,
		 uint32_t offset, bool doalloc,
		 daddr_t *diskblock_ret)
{
	daddr_t block;
	struct buf *idbuf;
	uint32_t idoff;
	uint32_t fileblocks_per_entry;
	struct sfs_blockobj idobj;
	int result;

	/* Get the block inodeobj immediately points to (maybe allocating) */
	result = sfs_bmap_get(sfs, inodeobj, 0, doalloc, &block);
	if (result) {
		return result;
	}

	while (indir > 0) {

		/* If nothing here, we're done */
		if (block == 0) {
			KASSERT(doalloc == false);
			*diskblock_ret = 0;
			return 0;
		}

		/*
		 * Compute the index into the indirect block.
		 * Leave the remainder in offset for the next pass.
		 */
		switch (indir) {
		    case 3:
			fileblocks_per_entry = SFS_DBPERIDB * SFS_DBPERIDB;
			break;
		    case 2:
			fileblocks_per_entry = SFS_DBPERIDB;
			break;
		    case 1:
			fileblocks_per_entry = 1;
			break;
		    default:
			panic("sfs: %s: sfs_bmap_subtree: "
			      "invalid indirect level %u\n",
			      sfs->sfs_sb.sb_volname,
			      indir);
		}
		idoff = offset / fileblocks_per_entry;
		offset = offset % fileblocks_per_entry;

		/* Read the indirect block */
		result = buffer_read(&sfs->sfs_absfs, block,
				     SFS_BLOCKSIZE, &idbuf);
		if (result) {
			return result;
		}

		sfs_blockobj_init_idblock(&idobj, idbuf);

		/* Get the address of the next layer down (maybe allocating) */
		result = sfs_bmap_get(sfs, &idobj, idoff, doalloc, &block);

		sfs_blockobj_cleanup(&idobj);
		buffer_release(idbuf);

		if (result) {
			return result;
		}

		indir--;
	}
	*diskblock_ret = block;
	return 0;
}

/*
 * Look up the disk block number (from 0 up to the number of blocks on
 * the disk) given a file and the logical block number within that
 * file. If DOALLOC is set, and no such block exists, one will be
 * allocated.
 *
 * Locking: must hold vnode lock. May get/release buffer cache locks
 * and (via sfs_balloc) sfs_freemaplock.
 *
 * Requires up to 2 buffers.
 */
int
sfs_bmap(struct sfs_vnode *sv, uint32_t fileblock, bool doalloc,
		daddr_t *diskblock)
{
	struct sfs_fs *sfs = sv->sv_absvn.vn_fs->fs_data;
	struct sfs_subtreeref subtree;
	uint32_t offset;
	struct sfs_blockobj inodeobj;
	int result;

	KASSERT(lock_do_i_hold(sv->sv_lock));

	/* Figure out where to start */
	result = sfs_get_indirection(fileblock, &subtree, &offset);
	if (result) {
		return result;
	}

	/* Load the inode */
	result = sfs_dinode_load(sv);
	if (result) {
		return result;
	}

	/* Initialize inodeobj to point at the top of this subtree */
	sfs_blockobj_init_inode(&inodeobj, sv, &subtree);

	/* Do the work in the indicated subtree */
	result = sfs_bmap_subtree(sfs, &inodeobj,
				  subtree.str_indirlevel,
				  offset, doalloc,
				  diskblock);
	sfs_blockobj_cleanup(&inodeobj);
	sfs_dinode_unload(sv);

	if (result) {
		return result;
	}

	/* Hand back the result and return. */
	if (*diskblock != 0 && !sfs_bused(sfs, *diskblock)) {
		panic("sfs: %s: Data block %u (block %u of file %u) "
		      "marked free\n",
		      sfs->sfs_sb.sb_volname,
		      *diskblock, fileblock, sv->sv_ino);
	}
	return 0;
}

////////////////////////////////////////////////////////////
// truncate

/*
 * State structure for a single indirection layer of a truncate
 * operation.
 */
struct layerinfo {
	daddr_t block;
	struct buf *buf;
	uint32_t *data;
	int pos;
	bool hasnonzero;
	bool modified;
};

/*
 * Find the intersection between the ranges [astart, aend)
 * and [bstart, bend). Returns true if this is nonempty.
 */
static
bool
sfs_intersect_range(uint32_t astart, uint32_t aend,
		    uint32_t bstart, uint32_t bend,
		    uint32_t *ret_start, uint32_t *ret_end)
{
	KASSERT(astart <= aend);
	KASSERT(bstart <= bend);

	if (astart == aend || bstart == bend) {
		return false;
	}

	if (aend <= bstart) {
		return false;
	}
	if (bend <= astart) {
		return false;
	}

	if (ret_start != NULL) {
		*ret_start = astart > bstart ? astart : bstart;
	}
	if (ret_end != NULL) {
		*ret_end = aend < bend ? aend : bend;
	}
	return true;
}

/*
 * Check if we can skip over an indirect block entry. We skip over
 * it if: it's zero; or the range it mapps doesn't intersect the
 * range we're trying to discard.
 *
 * We always inspect every entry of every indirect block we look at
 * (even if we only truncate from part of it) because the parts we
 * aren't truncating might already be zero, and if the indirect
 * block's all zeros we want to get rid of it.
 */
static
bool
sfs_skip_iblock_entry(struct sfs_fs *sfs,
		      struct layerinfo *layers, unsigned layer,
		      uint32_t startoffset, uint32_t endoffset)
{
	uint32_t lo, hi;

	layers[layer - 1].block = layers[layer].data[layers[layer].pos];
	switch (layer) {
	    case 3:
		lo = SFS_DBPERIDB * SFS_DBPERIDB * layers[3].pos;
		hi = lo + SFS_DBPERIDB * SFS_DBPERIDB;
		break;
	    case 2:
		lo = SFS_DBPERIDB * SFS_DBPERIDB * layers[3].pos
			+ SFS_DBPERIDB * layers[2].pos;
		hi = lo + SFS_DBPERIDB;
		break;
	    case 1:
		lo = SFS_DBPERIDB * SFS_DBPERIDB * layers[3].pos
			+ SFS_DBPERIDB * layers[2].pos
			+ layers[1].pos;
		hi = lo + 1;
		break;
	    default:
		panic("sfs: %s: sfs_skip_iblock_entry: "
		      "invalid layer %u\n",
		      sfs->sfs_sb.sb_volname, layer);
	}
	if (!sfs_intersect_range(lo, hi, startoffset,
				 endoffset,
				 NULL, NULL)) {
		/*
		 * Not in the discard range; but remember if we see
		 * any nonzero blocks in here.
		 */
		if (layers[layer - 1].block != 0) {
			layers[layer].hasnonzero = true;
		}
		return true;
	}
	if (layers[layer - 1].block == 0) {
		/* nothing to do */
		return true;
	}

	return false;
}

/*
 * Read an indirect block at level LAYER and stash the buffer in the
 * layers structure.
 */
static
int
sfs_itrunc_readindir(struct sfs_vnode *sv, struct layerinfo *layers,
		     unsigned layer)
{
	struct sfs_fs *sfs = sv->sv_absvn.vn_fs->fs_data;
	int result;

	result = buffer_read(sv->sv_absvn.vn_fs, layers[layer].block,
			     SFS_BLOCKSIZE, &layers[layer].buf);

	/*
	 * If there's an error, guess we just lose all the blocks
	 * referenced by this block! XXX.
	 */
	if (result) {
		kprintf("sfs: %s: sfs_itrunc: error reading level %u indirect "
			" block %u: %s\n",
			sfs->sfs_sb.sb_volname,
			layer, layers[layer].block, strerror(result));
		return result;
	}
	layers[layer].modified = false;
	return 0;
}

/*
 * Discard from one of the subtrees in the inode. ROOTPTR points to the
 * block pointer in the inode. INDIR is the indirection level of that
 * block pointer. STARTOFFSET and ENDOFFSET are fileblock numbers
 * relative to the beginning of this subtree, not indexes into the
 * indirect blocks. (yes, this is confusing)
 *
 * XXX: this code is a mess. I have been working on splitting out the
 * cut/paste portions, but it still needs quite a bit more.
 */
static
int
sfs_discard_subtree(struct sfs_vnode *sv, uint32_t *rootptr, unsigned indir,
		    uint32_t startoffset, uint32_t endoffset)
{
	struct sfs_fs *sfs = sv->sv_absvn.vn_fs->fs_data;

	struct layerinfo layers[4];
	unsigned layer;

	int result = 0, final_result = 0;

	unsigned ii;

	COMPILE_ASSERT(SFS_DBPERIDB * sizeof(layers[0].data[0])
		       == SFS_BLOCKSIZE);

	if (*rootptr == 0) {
		/* nothing to do */
		return 0;
	}

	for (ii=0; ii<4; ii++) {
		layers[ii].block = 0;
		layers[ii].buf = NULL;
		layers[ii].data = NULL;
		layers[ii].pos = 0;
		layers[ii].hasnonzero = false;
		layers[ii].modified = false;
	}

	/*
	 * We are going to cycle through all the blocks, changing levels
	 * of indirection. And free the ones that are past the new end
	 * of file.
	 */

	layers[indir].block = *rootptr;
	// otherwise we would not be here
	KASSERT(layers[indir].block != 0);

	/* Read the (however-many) indirect block */
	result = sfs_itrunc_readindir(sv, layers, indir);
	if (result) {
		return result;
	}

	if (indir == 1) {
		/*
		 * We do not need to execute the parts
		 * for double and triple levels of
		 * indirection.
		 */
		goto ilevel1;
	}
	if (indir == 2) {
		/*
		 * We do not need to execute the parts
		 * for the triple level of indirection.
		 */
		goto ilevel2;
	}
	if (indir == 3) {
		goto ilevel3;
	}

	/*
	 * This is the loop for level of indirection 3
	 * Go through all double indirect blocks
	 * pointed to from this triple indirect block,
	 * discard the ones that are past the new end
	 * of file.
	 */

 ilevel3:
	layer = 3;
	layers[layer].data = buffer_map(layers[layer].buf);
	for (layers[layer].pos = 0; layers[layer].pos < SFS_DBPERIDB; layers[layer].pos++) {
		if (sfs_skip_iblock_entry(sfs, layers, layer,
					  startoffset, endoffset)) {
			continue;
		}
		/*
		 * Read the double indirect block,
		 * hand it to the next inner loop.
		 */
		result = sfs_itrunc_readindir(sv, layers, layer - 1);
		if (result) {
			/* XXX blah */
			final_result = result;
			continue;
		}


		/*
		 * This is the loop for level of
		 * indirection 2 Go through all
		 * indirect blocks pointed to from
		 * this double indirect block, discard
		 * the ones that are past the new end
		 * of file.
		 */
    ilevel2:
		layer = 2;
		layers[layer].data = buffer_map(layers[layer].buf);
		for (layers[layer].pos = 0; layers[layer].pos < SFS_DBPERIDB; layers[layer].pos++) {
			/*
			 * Discard any blocks that are
			 * past the new EOF
			 */
			if (sfs_skip_iblock_entry(sfs, layers, layer,
						  startoffset, endoffset)) {
				continue;
			}
			/*
			 * Read the indirect block, hand it to the
			 * next inner loop.
			 */
			result = sfs_itrunc_readindir(sv, layers, layer - 1);
			if (result) {
				/* XXX blah */
				final_result = result;
				continue;
			}

			/*
			 * This is the loop for level
			 * of indirection 1
			 * Go through all direct
			 * blocks pointed to from this
			 * indirect block, discard the
			 * ones that are past the new
			 * end of file.
			 */
	    ilevel1:
			layer = 1;
			layers[layer].data = buffer_map(layers[layer].buf);
			for (layers[layer].pos = 0; layers[layer].pos < SFS_DBPERIDB; layers[layer].pos++) {
				/*
				 * Discard any blocks
				 * that are past the
				 * new EOF
				 */
				if (sfs_skip_iblock_entry(sfs, layers, layer,
							  startoffset,
							  endoffset)) {
					continue;
				}
				layers[layer].data[layers[layer].pos] = 0;
				layers[layer].modified = true;

				sfs_bfree_prelocked(sfs, layers[layer - 1].block);
			}
			/* end for level 1 */

			if (!layers[1].hasnonzero) {
				/*
				 * The whole indirect
				 * block is empty now;
				 * free it
				 */
				sfs_bfree_prelocked(sfs, layers[1].block);
				if (indir == 1) {
					*rootptr = 0;
					sfs_dinode_mark_dirty(sv);
				}
				if (indir != 1) {
					layers[2].modified = true;
					layers[2].data[layers[2].pos] = 0;
				}
				buffer_release_and_invalidate(layers[1].buf);
			}
			else if (layers[1].modified) {
				/*
				 * The indirect block
				 * has been modified
				 */
				buffer_mark_dirty(layers[1].buf);
				if (indir != 1) {
					layers[2].hasnonzero = true;
				}
				buffer_release(layers[1].buf);
			}
			else {
				buffer_release(layers[1].buf);
			}

			/*
			 * If we are just doing 1
			 * level of indirection, break
			 * out of the loop
			 */
			if (indir == 1) {
				break;
			}

			/* back to layer 2 */
			layer = 2;
		}
		/* end for level2 */

		/*
		 * If we are just doing 1 level of
		 * indirection, break out of the loop
		 */
		if (indir == 1) {
			break;
		}

		if (!layers[2].hasnonzero) {
			/*
			 * The whole double indirect
			 * block is empty now; free it
			 */
			sfs_bfree_prelocked(sfs, layers[2].block);
			if (indir == 2) {
				*rootptr = 0;
				sfs_dinode_mark_dirty(sv);
			}
			if (indir == 3) {
				layers[3].modified = true;
				layers[3].data[layers[3].pos] = 0;
			}
			buffer_release_and_invalidate(layers[2].buf);
		}
		else if (layers[2].modified) {
			/*
			 * The double indirect block
			 * has been modified
			 */
			buffer_mark_dirty(layers[2].buf);
			if (indir == 3) {
				layers[3].hasnonzero = true;
			}
			buffer_release(layers[2].buf);
		}
		else {
			buffer_release(layers[2].buf);
		}

		if (indir < 3) {
			break;
		}

		/* back to layer 3 */
		layer = 2;
	}
	/* end for level 3 */
	if (indir < 3) {
		return final_result;
	}
	if (!layers[3].hasnonzero) {
		/*
		 * The whole triple indirect block is
		 * empty now; free it
		 */
		sfs_bfree_prelocked(sfs, layers[3].block);
		*rootptr = 0;
		sfs_dinode_mark_dirty(sv);
		buffer_release_and_invalidate(layers[3].buf);
	}
	else if (layers[3].modified) {
		/*
		 * The triple indirect block has been
		 * modified
		 */
		buffer_mark_dirty(layers[3].buf);
		buffer_release(layers[3].buf);
	}
	else {
		buffer_release(layers[3].buf);
	}

	return final_result;
}

/*
 * Discard all blocks in the file from STARTFILEBLOCK through
 * ENDFILEBLOCK - 1.
 */
static
int
sfs_discard(struct sfs_vnode *sv,
	    uint32_t startfileblock, uint32_t endfileblock)
{
	struct sfs_fs *sfs = sv->sv_absvn.vn_fs->fs_data;
	struct sfs_dinode *inodeptr;
	uint32_t i;
	daddr_t block;
	uint32_t lo, hi, substart, subend;
	int result;

	inodeptr = sfs_dinode_map(sv);

	/*
	 * Go through the direct blocks. Discard any that are
	 * within the region we're discarding.
	 */
	for (i=0; i<SFS_NDIRECT; i++) {
		block = inodeptr->sfi_direct[i];
		if (i >= startfileblock && i < endfileblock && block != 0) {
			sfs_bfree_prelocked(sfs, block);
			inodeptr->sfi_direct[i] = 0;
			sfs_dinode_mark_dirty(sv);
		}
	}

	/* Indirect block */
	lo = SFS_NDIRECT;
	hi = lo + SFS_DBPERIDB;
	if (sfs_intersect_range(lo, hi, startfileblock, endfileblock,
				&substart, &subend)) {
		result = sfs_discard_subtree(sv, &inodeptr->sfi_indirect, 1,
					     substart - lo, subend - lo);
		if (result) {
			return result;
		}
	}

	/* Double indirect block */
	lo = hi;
	hi = lo + SFS_DBPERIDB * SFS_DBPERIDB;
	if (sfs_intersect_range(lo, hi, startfileblock, endfileblock,
				&substart, &subend)) {
		result = sfs_discard_subtree(sv, &inodeptr->sfi_dindirect, 2,
					     substart - lo, subend - lo);
		if (result) {
			return result;
		}
	}

	/* Triple indirect block */
	lo = hi;
	hi = lo + SFS_DBPERIDB * SFS_DBPERIDB * SFS_DBPERIDB;
	if (sfs_intersect_range(lo, hi, startfileblock, endfileblock,
				&substart, &subend)) {
		result = sfs_discard_subtree(sv, &inodeptr->sfi_tindirect, 3,
					     substart - lo, subend - lo);
		if (result) {
			return result;
		}
	}

	return 0;
}

/*
 * Truncate a file (or directory).
 *
 * Locking: must hold vnode lock. Acquires/releases buffer locks.
 *
 * Requires up to 4 buffers.
 */
int
sfs_itrunc(struct sfs_vnode *sv, off_t newlen)
{
	struct sfs_fs *sfs = sv->sv_absvn.vn_fs->fs_data;
	struct sfs_dinode *inodeptr;
	uint32_t oldblocklen, newblocklen;
	int result;

	KASSERT(lock_do_i_hold(sv->sv_lock));

	result = sfs_dinode_load(sv);
	if (result) {
		return result;
	}
	inodeptr = sfs_dinode_map(sv);

	/* Length in blocks (divide rounding up) */
	oldblocklen = DIVROUNDUP(inodeptr->sfi_size, SFS_BLOCKSIZE);
	newblocklen = DIVROUNDUP(newlen, SFS_BLOCKSIZE);

	/* Lock the freemap for the whole truncate */
	sfs_lock_freemap(sfs);

	if (newblocklen < oldblocklen) {
		result = sfs_discard(sv, newblocklen, oldblocklen);
		if (result) {
			sfs_unlock_freemap(sfs);
			sfs_dinode_unload(sv);
			return result;
		}
	}

	/* Set the file size */
	inodeptr->sfi_size = newlen;

	/* Mark the inode dirty */
	sfs_dinode_mark_dirty(sv);

	/* release the freemap */
	sfs_unlock_freemap(sfs);

	/* release the inode buffer */
	sfs_dinode_unload(sv);

	return 0;
}
