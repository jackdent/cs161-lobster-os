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

#ifndef _SFSPRIVATE_H_
#define _SFSPRIVATE_H_

#include <uio.h> /* for uio_rw */
struct buf; /* in buf.h */


//#define SFS_VERBOSE_RECOVERY


/* Type for log sequence numbers */
typedef uint64_t sfs_lsn_t;

/* jphys write callback context; define it however is convenient */
struct sfs_jphys_writecontext;

/* journal iterator, used during recovery */
struct sfs_jiter; /* opaque */


/* ops tables (in sfs_vnops.c) */
extern const struct vnode_ops sfs_fileops;
extern const struct vnode_ops sfs_dirops;

/* Macro for initializing a uio structure */
#define SFSUIO(iov, uio, ptr, block, rw) \
    uio_kinit(iov, uio, ptr, SFS_BLOCKSIZE, ((off_t)(block))*SFS_BLOCKSIZE, rw)

/* Print macros for verbose recovery */
#ifdef SFS_VERBOSE_RECOVERY
#define SAY(...) kprintf(__VA_ARGS__)
#define UNSAID(x)
#else
#define SAY(...)
#define UNSAID(x) ((void)(x))
#endif


/* Functions in sfs_balloc.c */
int sfs_balloc(struct sfs_fs *sfs, daddr_t *diskblock, struct buf **bufret);
void sfs_bfree(struct sfs_fs *sfs, daddr_t diskblock);
void sfs_bfree_prelocked(struct sfs_fs *sfs, daddr_t diskblock);
int sfs_bused(struct sfs_fs *sfs, daddr_t diskblock);
void sfs_lock_freemap(struct sfs_fs *sfs);
void sfs_unlock_freemap(struct sfs_fs *sfs);

/* Functions in sfs_bmap.c */
int sfs_bmap(struct sfs_vnode *sv, uint32_t fileblock,
		bool doalloc, daddr_t *diskblock);
int sfs_itrunc(struct sfs_vnode *sv, off_t len);

/* Functions in sfs_dir.c */
int sfs_readdir(struct sfs_vnode *sv, int slot, struct sfs_direntry *sd);
int sfs_writedir(struct sfs_vnode *sv, int slot, struct sfs_direntry *sd);
int sfs_dir_nentries(struct sfs_vnode *sv, int *ret);
int sfs_dir_findname(struct sfs_vnode *sv, const char *name,
		uint32_t *ino, int *slot, int *emptyslot);
int sfs_dir_findino(struct sfs_vnode *sv, uint32_t ino,
		struct sfs_direntry *retsd, int *slot);
int sfs_dir_link(struct sfs_vnode *sv, const char *name, uint32_t ino,
		int *slot);
int sfs_dir_unlink(struct sfs_vnode *sv, int slot);
int sfs_dir_checkempty(struct sfs_vnode *sv);
int sfs_lookonce(struct sfs_vnode *sv, const char *name,
		struct sfs_vnode **ret,
		int *slot);

/* Functions in sfs_inode.c */
int sfs_dinode_load(struct sfs_vnode *sv);
void sfs_dinode_unload(struct sfs_vnode *sv);
struct sfs_dinode *sfs_dinode_map(struct sfs_vnode *sv);
void sfs_dinode_mark_dirty(struct sfs_vnode *sv);
int sfs_reclaim(struct vnode *v);
int sfs_loadvnode(struct sfs_fs *sfs, uint32_t ino, int forcetype,
		struct sfs_vnode **ret);
int sfs_makeobj(struct sfs_fs *sfs, int type, struct sfs_vnode **ret);
int sfs_getroot(struct fs *fs, struct vnode **ret);

/* Functions in sfs_io.c */
int sfs_readblock(struct fs *fs, daddr_t block, void *data, size_t len);
int sfs_writeblock(struct fs *fs, daddr_t block, void *fsbufdata,
		   void *data, size_t len);
int sfs_io(struct sfs_vnode *sv, struct uio *uio);
int sfs_metaio(struct sfs_vnode *sv, off_t pos, void *data, size_t len,
	       enum uio_rw rw);

/* Function used by sfs_jphys.c - you write this */
/* XXX: there should be a template; but we don't ship the file it belongs in */
#ifdef SFS_VERBOSE_RECOVERY
const char *sfs_jphys_client_recname(unsigned type);
#endif

/* Functions in sfs_jphys.c */
bool sfs_block_is_journal(struct sfs_fs *sfs, uint32_t block);
/* writer interface */
sfs_lsn_t sfs_jphys_write(struct sfs_fs *sfs,
		void (*callback)(struct sfs_fs *sfs,
			sfs_lsn_t newlsn,
			struct sfs_jphys_writecontext *ctx),
		struct sfs_jphys_writecontext *ctx,
		unsigned code, const void *rec, size_t len);
int sfs_jphys_flush(struct sfs_fs *sfs, sfs_lsn_t lsn);
int sfs_jphys_flushall(struct sfs_fs *sfs);
/* these are already deployed in sfs_writeblock */
int sfs_jphys_flushforjournalblock(struct sfs_fs *sfs, daddr_t diskblock);
void sfs_wrote_journal_block(struct sfs_fs *sfs, daddr_t diskblock);
/* interface for checkpointing */
sfs_lsn_t sfs_jphys_peeknextlsn(struct sfs_fs *sfs);
void sfs_jphys_trim(struct sfs_fs *sfs, sfs_lsn_t taillsn);
uint32_t sfs_jphys_getodometer(struct sfs_jphys *jp);
void sfs_jphys_clearodometer(struct sfs_jphys *jp);
/* reader interface */
bool sfs_jiter_done(struct sfs_jiter *ji);
unsigned sfs_jiter_type(struct sfs_jiter *ji);
sfs_lsn_t sfs_jiter_lsn(struct sfs_jiter *ji);
void *sfs_jiter_rec(struct sfs_jiter *ji, size_t *len_ret);
int sfs_jiter_next(struct sfs_fs *sfs, struct sfs_jiter *ji);
int sfs_jiter_prev(struct sfs_fs *sfs, struct sfs_jiter *ji);
int sfs_jiter_seekhead(struct sfs_fs *sfs, struct sfs_jiter *ji);
int sfs_jiter_seektail(struct sfs_fs *sfs, struct sfs_jiter *ji);
int sfs_jiter_fwdcreate(struct sfs_fs *sfs, struct sfs_jiter **ji_ret);
int sfs_jiter_revcreate(struct sfs_fs *sfs, struct sfs_jiter **ji);
void sfs_jiter_destroy(struct sfs_jiter *ji);
/* load up the physical journal (already deployed in mount) */
int sfs_jphys_loadup(struct sfs_fs *sfs);
/* control and mode changes (already deployed in sfs_fsops.c) */
struct sfs_jphys *sfs_jphys_create(void);
void sfs_jphys_destroy(struct sfs_jphys *jp);
void sfs_jphys_startreading(struct sfs_fs *sfs);
void sfs_jphys_stopreading(struct sfs_fs *sfs);
int sfs_jphys_startwriting(struct sfs_fs *sfs);
void sfs_jphys_unstartwriting(struct sfs_fs *sfs);
void sfs_jphys_stopwriting(struct sfs_fs *sfs);


#endif /* _SFSPRIVATE_H_ */
