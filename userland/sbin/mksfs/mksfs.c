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

#include <sys/types.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <limits.h>
#include <err.h>

#include "support.h"
#include "kern/sfs.h"


#ifdef HOST

#include <netinet/in.h> // for arpa/inet.h
#include <arpa/inet.h>  // for ntohl
#include "hostcompat.h"
#define SWAP64(x) ntohll(x)
#define SWAP32(x) ntohl(x)
#define SWAP16(x) ntohs(x)

#else

#define SWAP64(x) (x)
#define SWAP32(x) (x)
#define SWAP16(x) (x)

#endif

#include "disk.h"

/* Maximum size of freemap we support */
#define MAXFREEMAPBLOCKS 32

/* Block number for the initial root directory contents */
static uint32_t rootdir_data_block;

/* Journal location and size */
static uint32_t journalstart, journalblocks;

/* Free block bitmap */
static char freemapbuf[MAXFREEMAPBLOCKS * SFS_BLOCKSIZE];

/*
 * Assert that the on-disk data structures are correctly sized.
 */
static
void
check(void)
{
	assert(sizeof(struct sfs_superblock)==SFS_BLOCKSIZE);
	assert(sizeof(struct sfs_dinode)==SFS_BLOCKSIZE);
	assert(SFS_BLOCKSIZE % sizeof(struct sfs_direntry) == 0);
}

/*
 * Mark a block allocated.
 */
static
void
allocblock(uint32_t block)
{
	uint32_t mapbyte = block/CHAR_BIT;
	unsigned char mask = (1<<(block % CHAR_BIT));

	assert((freemapbuf[mapbyte] & mask) == 0);
	freemapbuf[mapbyte] |= mask;
}

/*
 * Initialize the free block bitmap.
 */
static
void
initfreemap(uint32_t fsblocks)
{
	uint32_t freemapbits = SFS_FREEMAPBITS(fsblocks);
	uint32_t freemapblocks = SFS_FREEMAPBLOCKS(fsblocks);
	uint32_t i;

	if (freemapblocks > MAXFREEMAPBLOCKS) {
		errx(1, "Filesystem too large -- "
		     "increase MAXFREEMAPBLOCKS and recompile");
	}

	/* mark the superblock and root inode in use */
	allocblock(SFS_SUPER_BLOCK);
	allocblock(SFS_ROOTDIR_INO);

	/* the freemap blocks must be in use */
	for (i=0; i<freemapblocks; i++) {
		allocblock(SFS_FREEMAP_START + i);
	}

	/* journal goes after the freemap */
	journalstart = SFS_FREEMAP_START + freemapblocks;
	journalblocks = fsblocks / 20;
	for (i=0; i<journalblocks; i++) {
		allocblock(journalstart + i);
	}

	/* allocate a block for the root directory contents */
	rootdir_data_block = journalstart + journalblocks;
	allocblock(rootdir_data_block);

	/* all blocks in the freemap but past the volume end are "in use" */
	for (i=fsblocks; i<freemapbits; i++) {
		allocblock(i);
	}
}

/*
 * Initialize and write out the superblock.
 */
static
void
writesuper(const char *volname, uint32_t nblocks)
{
	struct sfs_superblock sb;

	/* The cast is required on some outdated host systems. */
	bzero((void *)&sb, sizeof(sb));

	if (strlen(volname) >= SFS_VOLNAME_SIZE) {
		errx(1, "Volume name %s too long", volname);
	}

	/* Initialize the superblock structure */
	sb.sb_magic = SWAP32(SFS_MAGIC);
	sb.sb_nblocks = SWAP32(nblocks);
	strcpy(sb.sb_volname, volname);
	sb.sb_journalstart = SWAP32(journalstart);
	sb.sb_journalblocks = SWAP32(journalblocks);

	/* and write it out. */
	diskwrite(&sb, SFS_SUPER_BLOCK);
}

/*
 * Write out the free block bitmap.
 */
static
void
writefreemap(uint32_t fsblocks)
{
	uint32_t freemapblocks;
	char *ptr;
	uint32_t i;

	/* Write out each of the blocks in the free block bitmap. */
	freemapblocks = SFS_FREEMAPBLOCKS(fsblocks);
	for (i=0; i<freemapblocks; i++) {
		ptr = freemapbuf + i*SFS_BLOCKSIZE;
		diskwrite(ptr, SFS_FREEMAP_START+i);
	}
}

/*
 * Write out the root directory inode.
 */
static
void
writerootdir(void)
{
	struct sfs_dinode sfi;
	struct sfs_direntry sfd[SFS_BLOCKSIZE / sizeof(struct sfs_direntry)];

	assert(rootdir_data_block > 0);
	assert(sizeof(sfd) >= sizeof(struct sfs_direntry) * 2);

	/* Initialize the dinode */
	bzero((void *)&sfi, sizeof(sfi));

	sfi.sfi_size = SWAP32(sizeof(struct sfs_direntry) * 2);
	sfi.sfi_type = SWAP16(SFS_TYPE_DIR);
	sfi.sfi_linkcount = SWAP16(2);
	sfi.sfi_direct[0] = SWAP32(rootdir_data_block);

	/* Write it out */
	diskwrite(&sfi, SFS_ROOTDIR_INO);

	/* Write out the initial root directory contents */
	bzero((void *)sfd, sizeof(sfd));
	sfd[0].sfd_ino = SWAP32(SFS_ROOTDIR_INO);
	strcpy(sfd[0].sfd_name, ".");
	sfd[1].sfd_ino = SWAP32(SFS_ROOTDIR_INO);
	strcpy(sfd[1].sfd_name, "..");

	diskwrite(sfd, rootdir_data_block);
}

/*
 * Write out the journal.
 */
static
void
writejournal(void)
{
	char block[SFS_BLOCKSIZE];
	struct sfs_jphys_header hdr;
	struct sfs_jphys_trim rec;
	uint64_t coninfo;
	unsigned i;

	bzero((void *)block, sizeof(block));

	/* Zero all of the journal but the first block */
	for (i=1; i<journalblocks; i++) {
		diskwrite(block, journalstart + i);
	}

	/* and write a trim record into the first block */
	coninfo = SFS_MKCONINFO(SFS_JPHYS_CONTAINER,
				SFS_JPHYS_TRIM,
				sizeof(hdr) + sizeof(rec), 1 /* first lsn */);
	hdr.jh_coninfo = SWAP64(coninfo);
	rec.jt_taillsn = SWAP64(1 /* first lsn */);

	memcpy(block, &hdr, sizeof(hdr));
	memcpy(block + sizeof(hdr), &rec, sizeof(rec));

	/* put more stuff in here if needed for your checkpoint scheme */

	/* the rest of the block is a pad record */
	coninfo = SFS_MKCONINFO(SFS_JPHYS_CONTAINER,
				SFS_JPHYS_PAD,
				SFS_BLOCKSIZE - sizeof(hdr) - sizeof(rec),
				2 /* second lsn */);
	hdr.jh_coninfo = SWAP64(coninfo);
	memcpy(block + sizeof(hdr) + sizeof(rec), &hdr, sizeof(hdr));

	diskwrite(block, journalstart);
}

/*
 * Main.
 */
int
main(int argc, char **argv)
{
	uint32_t size, blocksize;
	char *volname, *s;

#ifdef HOST
	hostcompat_init(argc, argv);
#endif

	if (argc!=3) {
		errx(1, "Usage: mksfs device/diskfile volume-name");
	}

	check();

	volname = argv[2];

	/* Remove one trailing colon from volname, if present */
	s = strchr(volname, ':');
	if (s != NULL) {
		if (strlen(s)!=1) {
			errx(1, "Illegal volume name %s", volname);
		}
		*s = 0;
	}

	/* Don't allow slashes */
	s = strchr(volname, '/');
	if (s != NULL) {
		errx(1, "Illegal volume name %s", volname);
	}

	opendisk(argv[1]);
	blocksize = diskblocksize();

	if (blocksize!=SFS_BLOCKSIZE) {
		errx(1, "Device has wrong blocksize %u (should be %u)\n",
		     blocksize, SFS_BLOCKSIZE);
	}
	size = diskblocks();

	/* Write out the on-disk structures */
	initfreemap(size);
	writesuper(volname, size);
	writefreemap(size);
    writejournal();
	writerootdir();

	closedisk();

	return 0;
}

