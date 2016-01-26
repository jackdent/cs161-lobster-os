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
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <assert.h>
#include <limits.h>
#include <err.h>

#include "support.h"
#include "kern/sfs.h"


#ifdef HOST
/*
 * OS/161 runs natively on a big-endian platform, so we can
 * conveniently use the byteswapping functions for network byte order.
 */
#include <netinet/in.h> // for arpa/inet.h
#include <arpa/inet.h>  // for ntohl
#include "hostcompat.h"
#define SWAP64(x) ntohll(x)
#define SWAP32(x) ntohl(x)
#define SWAP16(x) ntohs(x)

extern const char *hostcompat_progname;

#else

#define SWAP64(x) (x)
#define SWAP32(x) (x)
#define SWAP16(x) (x)

#endif

#include "disk.h"

#define ARRAYCOUNT(a) (sizeof(a) / sizeof((a)[0]))
#define DIVROUNDUP(a, b) (((a) + (b) - 1) / (b))

static bool dofiles, dodirs;
static bool doindirect;
static bool recurse;

////////////////////////////////////////////////////////////
// printouts

static unsigned dumppos;

static
void
dumpval(const char *desc, const char *val)
{
	size_t dlen, vlen, used;

	dlen = strlen(desc);
	vlen = strlen(val);

	printf("    ");

	printf("%s: %s", desc, val);

	used = dlen + 2 + vlen;
	for (; used < 36; used++) {
		putchar(' ');
	}

	if (dumppos % 2 == 1) {
		printf("\n");
	}
	dumppos++;
}

static
void
dumpvalf(const char *desc, const char *valf, ...)
{
	va_list ap;
	char buf[128];

	va_start(ap, valf);
	vsnprintf(buf, sizeof(buf), valf, ap);
	va_end(ap);
	dumpval(desc, buf);
}

static
void
dumplval(const char *desc, const char *lval)
{
	if (dumppos % 2 == 1) {
		printf("\n");
		dumppos++;
	}
	printf("    %s: %s\n", desc, lval);
	dumppos += 2;
}

#if 0 /* you may find these useful */
static
void
dumphexrow(uint8_t *buf, size_t len, size_t padlen)
{
	size_t i;

	for (i=0; i<padlen; i++) {
		if (padlen % 8 == 0) {
			putchar(' ');
		}
		if (i < len) {
			printf("%02x", buf[i]);
		}
		else {
			printf("  ");
		}
	}
	printf("  ");
	for (i=0; i<padlen; i++) {
		if (i < len) {
			if (buf[i] < 32 || buf[i] > 126) {
				putchar('.');
			}
			else {
				putchar(buf[i]);
			}
		}
		else {
			putchar(' ');
		}
	}
}

static
void
diffhexdump(uint8_t *od, uint8_t *nd, size_t len)
{
	size_t i, limit;
	char posbuf[16];

	for (i=0; i<len; i+=16) {
		limit = len - i;
		if (limit > 16) {
			limit = 16;
		}

		snprintf(posbuf, sizeof(posbuf), "0x%zx", i);

		printf("-       %4s", posbuf);
		dumphexrow(od+i, limit, 16);
		printf("\n");

		printf("+       %4s", posbuf);
		dumphexrow(nd+i, limit, 16);
		printf("\n");
	}
}
#endif


////////////////////////////////////////////////////////////
// fs structures

static void dumpinode(uint32_t ino, const char *name);

static
uint32_t
readsb(void)
{
	struct sfs_superblock sb;

	diskread(&sb, SFS_SUPER_BLOCK);
	if (SWAP32(sb.sb_magic) != SFS_MAGIC) {
		errx(1, "Not an sfs filesystem");
	}
	return SWAP32(sb.sb_nblocks);
}

static
void
dumpsb(void)
{
	struct sfs_superblock sb;
	unsigned i;

	diskread(&sb, SFS_SUPER_BLOCK);
	sb.sb_volname[sizeof(sb.sb_volname)-1] = 0;

	printf("Superblock\n");
	printf("----------\n");
	dumpvalf("Magic", "0x%8x", SWAP32(sb.sb_magic));
	dumpvalf("Size", "%u blocks", SWAP32(sb.sb_nblocks));
	dumpvalf("Freemap size", "%u blocks",
		 SFS_FREEMAPBLOCKS(SWAP32(sb.sb_nblocks)));
	dumpvalf("Block size", "%u bytes", SFS_BLOCKSIZE);
	dumpvalf("Journal start", "%u", SWAP32(sb.sb_journalstart));
	dumpvalf("Journal size", "%u blocks", SWAP32(sb.sb_journalblocks));
	dumplval("Volume name", sb.sb_volname);

	for (i=0; i<ARRAYCOUNT(sb.reserved); i++) {
		if (sb.reserved[i] != 0) {
			printf("    Word %u in reserved area: 0x%x\n",
			       i, SWAP32(sb.reserved[i]));
		}
	}
	printf("\n");
}

static
void
dumpfreemap(uint32_t fsblocks)
{
	uint32_t freemapblocks = SFS_FREEMAPBLOCKS(fsblocks);
	uint32_t i, j, k, bn;
	uint8_t data[SFS_BLOCKSIZE], mask;
	char tmp[16];

	printf("Free block bitmap\n");
	printf("-----------------\n");
	for (i=0; i<freemapblocks; i++) {
		diskread(data, SFS_FREEMAP_START+i);
		printf("    Freemap block #%u in disk block %u: blocks %u - %u"
		       " (0x%x - 0x%x)\n",
		       i, SFS_FREEMAP_START+i,
		       i*SFS_BITSPERBLOCK, (i+1)*SFS_BITSPERBLOCK - 1,
		       i*SFS_BITSPERBLOCK, (i+1)*SFS_BITSPERBLOCK - 1);
		for (j=0; j<SFS_BLOCKSIZE; j++) {
			if (j % 8 == 0) {
				snprintf(tmp, sizeof(tmp), "0x%x",
					 i*SFS_BITSPERBLOCK + j*8);
				printf("%-7s ", tmp);
			}
			for (k=0; k<8; k++) {
				bn = i*SFS_BITSPERBLOCK + j*8 + k;
				mask = 1U << k;
				if (bn >= fsblocks) {
					if (data[j] & mask) {
						putchar('x');
					}
					else {
						putchar('!');
					}
				}
				else {
					if (data[j] & mask) {
						putchar('*');
					}
					else {
						putchar('.');
					}
				}
			}
			if (j % 8 == 7) {
				printf("\n");
			}
			else {
				printf(" ");
			}
		}
	}
	printf("\n");
}

static
void
copyandzero(void *dest, size_t destlen, const void *src, size_t srclen)
{
	if (destlen < srclen) {
		printf("[too big: got %zu expected %zu] ", srclen, destlen);
		memcpy(dest, src, destlen);
	}
	else if (destlen > srclen) {
		printf("[too small: got %zu expected %zu] ", srclen, destlen);
		memcpy(dest, src, srclen);
		memset((char *)dest + srclen, '\0', destlen - srclen);
	}
	else {
		memcpy(dest, src, destlen);
	}
}

static
bool
iszeroed(const uint8_t *buf, size_t len)
{
	size_t i;

	for (i=0; i<len; i++) {
		if (buf[i]) {
			return false;
		}
	}
	return true;
}

static
void
dump_container_record(uint32_t myblock, unsigned myoffset, uint64_t mylsn,
		      unsigned type, void *data, size_t len)
{
	char buf[64];

	snprintf(buf, sizeof(buf), "[%u.%u]:", myblock, myoffset);
	printf("    %-8s %-8llu ", buf, (unsigned long long)mylsn);
	switch (type) {

	    /* container-level records */
	    case SFS_JPHYS_INVALID:
		/* XXX: hexdump the contents */
		printf("... invalid\n");
		break;
	    case SFS_JPHYS_PAD:
		printf("[pad %zu]\n", len);
		break;
	    case SFS_JPHYS_TRIM:
		{
			struct sfs_jphys_trim jt;

			copyandzero(&jt, sizeof(jt), data, len);
			printf("TRIM -> %llu\n",
			       (unsigned long long)SWAP64(jt.jt_taillsn));
		}
		break;
	    default:
		/* XXX hexdump it */
		printf("Unknown record type %u\n", type);
		break;
	}
}

static
void
dump_client_record(uint32_t myblock, unsigned myoffset, uint64_t mylsn,
		   unsigned type, void *data, size_t len)
{
	char buf[64];

	snprintf(buf, sizeof(buf), "[%u.%u]:", myblock, myoffset);
	printf("    %-8s %-8llu ", buf, (unsigned long long)mylsn);
	switch (type) {

	    /* recovery-level records */

		/*
		 * You write this.
		 */
		(void)data;
		(void)len;

	    default:
		/* XXX hexdump it */
		printf("Unknown record type %u\n", type);
		break;
	}
}

/*
 * Find the physical location of an LSN.
 */
static
void
findlsn(const uint64_t *firstlsns, uint32_t jstart, uint32_t jblocks,
	uint64_t targetlsn,
	uint32_t *block_ret, unsigned *offset_ret)
{
	uint32_t block, nextblock;
	uint8_t buf[SFS_BLOCKSIZE];
	unsigned offset;
	struct sfs_jphys_header jh;
	uint64_t ci;
	uint64_t lsn;
	unsigned len;

	for (block = 0; block < jblocks; block++) {
		nextblock = (block + 1) % jblocks;

		if (targetlsn < firstlsns[block]) {
			continue;
		}
		if (firstlsns[block] > firstlsns[nextblock] ||
		    firstlsns[nextblock] == 0 ||
		    targetlsn < firstlsns[nextblock]) {
			goto found;
		}
	}
	errx(1, "Cannot find block for tail LSN %llu",
	     (unsigned long long)targetlsn);

 found:

	diskread(buf, jstart + block);
	offset = 0;
	while (offset + sizeof(jh) <= SFS_BLOCKSIZE) {
		memcpy(&jh, buf + offset, sizeof(jh));
		ci = SWAP64(jh.jh_coninfo);
		assert(ci != 0);
		lsn = SFS_CONINFO_LSN(ci);
		len = SFS_CONINFO_LEN(ci);

		if (lsn == targetlsn) {
			*block_ret = block;
			*offset_ret = offset;
			return;
		}
		offset += len;
	}
	errx(1, "Cannot find offset for tail LSN %llu in block %u",
	     (unsigned long long)targetlsn, block);
}

static
void
dumpjournal(void)
{
	struct sfs_superblock sb;
	uint32_t jstart, jblocks;
	struct sfs_jphys_header jh;
	uint64_t ci;
	unsigned len;
	unsigned class, type;
	struct sfs_jphys_trim jt;
	uint8_t buf[SFS_BLOCKSIZE];

	uint64_t bh_checkpoint_taillsn, eoj_checkpoint_taillsn;
	//uint32_t bh_checkpoint_block, eoj_checkpoint_block;
	//unsigned bh_checkpoint_offset, eoj_checkpoint_offset;

	uint64_t veryfirstlsn, prevlsn, headlsn, smallestlsn, taillsn;
	uint32_t headblock, smallestlsn_block, tailblock;
	unsigned tailoffset;

	uint64_t *firstlsns;

	uint64_t mylsn, lsn;
	uint32_t myblock, block;
	unsigned myoffset, offset;
	void *mydata;
	unsigned mylen;


	diskread(&sb, SFS_SUPER_BLOCK);
	jstart = SWAP32(sb.sb_journalstart);
	jblocks = SWAP32(sb.sb_journalblocks);

	printf("Journal (%u blocks at %u)\n", jblocks, jstart);
	printf("--------------------------------\n");

	/*
	 * First pass: read the LSNs and find the head. If this
	 * doesn't work, try using -J to do a physical journal dump.
	 */

	bh_checkpoint_taillsn = eoj_checkpoint_taillsn = 0;
	//bh_checkpoint_block = eoj_checkpoint_block = 0;
	//bh_checkpoint_offset = eoj_checkpoint_offset = 0;

	veryfirstlsn = 0;
	prevlsn = 0;
	headlsn = 0;
	smallestlsn = 0;
	headblock = 0;
	smallestlsn_block = 0;
	tailblock = 0;
	tailoffset = 0;
	firstlsns = malloc(jblocks * sizeof(firstlsns[0]));

	for (block=0; block<jblocks; block++) {
		diskread(buf, jstart + block);
		offset = 0;
		while (offset + sizeof(jh) <= SFS_BLOCKSIZE) {
			assert(offset % sizeof(uint16_t) == 0);
			memcpy(&jh, buf + offset, sizeof(jh));
			ci = SWAP64(jh.jh_coninfo);
			if (ci == 0) {
				if (offset != 0) {
					errx(1, "At %u[%u] in journal: "
					     "zero header\n", block, offset);
				}
				/* block hasn't been used yet */
				firstlsns[block] = 0;
				if (headlsn == 0) {
					headlsn = prevlsn + 1;
					headblock = block;
				}
				break;
			}
			lsn = SFS_CONINFO_LSN(ci);
			len = SFS_CONINFO_LEN(ci);

			if (offset == 0) {
				firstlsns[block] = lsn;
			}

			if (len == 0) {
				errx(1, "At %u[%u] in journal: "
				     "zero-length record", block, offset);
			}
			if (len < sizeof(jh)) {
				errx(1, "At %u[%u] in journal: "
				     "runt record (length %u)",
				     block, offset, len);
			}

			if (block == 0 && offset == 0) {
				veryfirstlsn = lsn;
			}
			else if (block > 0 && offset == 0 && lsn < prevlsn) {
				if (lsn > veryfirstlsn) {
					errx(1, "At %u[%u] in journal: "
					     "duplicate lsn %llu\n",
					     block, offset,
					     (unsigned long long)lsn);
				}
				smallestlsn = lsn;
				smallestlsn_block = block;
				headlsn = prevlsn + 1;
				headblock = block;
			}
			else {
				if (lsn != prevlsn + 1) {
					errx(1, "At %u[%u] in journal: "
					     "discontiguous lsn %llu, "
					     "after %llu\n",
					     block, offset,
					     (unsigned long long)lsn,
					     (unsigned long long)prevlsn);
				}
			}

			/*
			 * Remember the location and save the contents of:
			 *    - the last checkpoint we see before we find
			 *      the head
			 *    - the last checkpoint we see before the
			 *      physical end of the journal
			 */
			if (SFS_CONINFO_CLASS(ci) == SFS_JPHYS_CONTAINER &&
			    SFS_CONINFO_TYPE(ci) == SFS_JPHYS_TRIM) {
				if (len != sizeof(jh) + sizeof(jt)) {
					errx(1, "At %u[%u] in journal: "
					     "bad trim record size %u\n",
					     block, offset, len);
				}
				memcpy(&jt, buf + offset + sizeof(jh),
				       sizeof(jt));
				jt.jt_taillsn = SWAP64(jt.jt_taillsn);
				if (headlsn == 0) {
					bh_checkpoint_taillsn = jt.jt_taillsn;
					//bh_checkpoint_block = block;
					//bh_checkpoint_offset = offset;
				}
				else {
					eoj_checkpoint_taillsn = jt.jt_taillsn;
					//eoj_checkpoint_block = block;
					//eoj_checkpoint_offset = offset;
				}
			}

			prevlsn = lsn;
			offset += len;
		}
	}

	/*
	 * Second: find the tail. We don't need to scan again; pick
	 * either bh_checkpoint_taillsn or eoj_checkpoint_taillsn.
	 * Or neither, in which case we use either veryfirstlsn or
	 * smallestlsn.
	 */
	if (bh_checkpoint_taillsn != 0) {
		taillsn = bh_checkpoint_taillsn;
		findlsn(firstlsns, jstart, jblocks, taillsn,
			&tailblock, &tailoffset);
	}
	else if (eoj_checkpoint_taillsn != 0) {
		taillsn = eoj_checkpoint_taillsn;
		findlsn(firstlsns, jstart, jblocks, taillsn,
			&tailblock, &tailoffset);
	}
	else if (smallestlsn != 0) {
		taillsn = smallestlsn;
		tailblock = smallestlsn_block;
		tailoffset = 0;
	}
	else {
		taillsn = veryfirstlsn;
		tailblock = 0;
		tailoffset = 0;
	}

	free(firstlsns);
	firstlsns = NULL;

	printf("    head: lsn %llu, at %u[0]\n",
	       (unsigned long long)headlsn, headblock);
	printf("    tail: lsn %llu, at %u[%u]\n",
	       (unsigned long long)taillsn, tailblock, tailoffset);
	printf("\n");

	myblock = tailblock;
	myoffset = tailoffset;
	mylsn = taillsn;
	diskread(buf, jstart + myblock);
	while (mylsn < headlsn) {
		while (myoffset + sizeof(jh) <= SFS_BLOCKSIZE) {
			memcpy(&jh, buf + myoffset, sizeof(jh));
			ci = SWAP64(jh.jh_coninfo);
			class = SFS_CONINFO_CLASS(ci);
			type = SFS_CONINFO_TYPE(ci);
			len = SFS_CONINFO_LEN(ci);
			lsn = SFS_CONINFO_LSN(ci);

			/* these have already been checked */
			assert(lsn == mylsn);
			assert(len >= sizeof(jh));

			mydata = buf + myoffset + sizeof(jh);
			mylen = len - sizeof(jh);

			if (class == SFS_JPHYS_CONTAINER) {
				dump_container_record(myblock, myoffset, mylsn,
						      type, mydata, mylen);
			}
			else {
				dump_client_record(myblock, myoffset, mylsn,
						   type, mydata, mylen);
			}

			myoffset += len;
			mylsn++;
		}
		myblock = (myblock + 1) % jblocks;
		myoffset = 0;
		diskread(buf, jstart + myblock);
	}
	printf("\n");
}

static
void
dumpphysjournal(void)
{
	struct sfs_superblock sb;
	uint32_t jstart, jblocks;
	uint8_t buf[SFS_BLOCKSIZE];
	struct sfs_jphys_header jh;
	uint64_t ci;
	unsigned class, type;
	unsigned len;

	uint64_t lsn;
	uint32_t block;
	unsigned offset;

	void *recdata;
	size_t reclen;

	unsigned slop, fix;

	char pbuf[64];


	diskread(&sb, SFS_SUPER_BLOCK);
	jstart = SWAP32(sb.sb_journalstart);
	jblocks = SWAP32(sb.sb_journalblocks);

	printf("Physical journal (%u blocks at %u)\n", jblocks, jstart);
	printf("----------------------------------------\n");

	for (block=0; block<jblocks; block++) {
		diskread(buf, jstart + block);
		offset = 0;
		while (offset + sizeof(jh) <= SFS_BLOCKSIZE) {
			slop = offset % sizeof(uint16_t);
			if (slop != 0) {
				fix = sizeof(jh) - slop;
				warnx("At %u[%u] in journal: "
				      "unaligned, skipping %u bytes",
				      block, offset, fix);
				offset += fix;
				continue;
			}
			assert(offset % sizeof(uint16_t) == 0);

			if (iszeroed(buf, sizeof(buf))) {
				snprintf(pbuf, sizeof(pbuf), "[%u.*]:", block);
				printf("    %-8s [block is zero]\n", pbuf);
				break;
			}

			memcpy(&jh, buf + offset, sizeof(jh));
			ci = SWAP64(jh.jh_coninfo);
			if (ci == 0) {
				snprintf(pbuf, sizeof(pbuf), "[%u.%u]:",
					 block, offset);
				printf("    %-8s 0  [Zero record]\n", pbuf);
				offset += sizeof(jh);
				continue;
			}

			class = SFS_CONINFO_CLASS(ci);
			type = SFS_CONINFO_TYPE(ci);
			len = SFS_CONINFO_LEN(ci);
			lsn = SFS_CONINFO_LSN(ci);

			if (len < sizeof(jh)) {
				warnx("At %u[%u] in journal: "
				      "record too small (size %u)",
				      block, offset, len);
				/* There is at least this much data present. */
				len = sizeof(jh);
			}
			if (offset + len > SFS_BLOCKSIZE) {
				warnx("At %u[%u] in journal: "
				      "record too large (size %u)",
				      block, offset, len);
				len = SFS_BLOCKSIZE - offset;
			}
			recdata = buf + offset + sizeof(jh);
			reclen = len - sizeof(jh);
			if (class == SFS_JPHYS_CONTAINER) {
				dump_container_record(block, offset, lsn, type,
						      recdata, reclen);
			}
			else {
				dump_client_record(block, offset, lsn, type,
						   recdata, reclen);
			}
			offset += len;
		}
	}
}

static
void
dumpindirect(uint32_t block, unsigned indirection)
{
	uint32_t ib[SFS_BLOCKSIZE/sizeof(uint32_t)];
	char tmp[128];
	unsigned i;

	static const char *const names[4] = {
		"Direct", "Indirect", "Double indirect", "Triple indirect"
	};

	assert(indirection < 4);

	if (block == 0) {
		return;
	}
	printf("%s block %u\n", names[indirection], block);

	diskread(ib, block);
	for (i=0; i<ARRAYCOUNT(ib); i++) {
		if (i % 4 == 0) {
			printf("@%-3u   ", i);
		}
		snprintf(tmp, sizeof(tmp), "%u (0x%x)",
			 SWAP32(ib[i]), SWAP32(ib[i]));
		printf("  %-16s", tmp);
		if (i % 4 == 3) {
			printf("\n");
		}
	}
	if (indirection > 1) {
		for (i=0; i<ARRAYCOUNT(ib); i++) {
			dumpindirect(SWAP32(ib[i]), indirection - 1);
		}
	}
}

static
uint32_t
traverse_ib(uint32_t fileblock, uint32_t numblocks, uint32_t block,
	    unsigned indirection, void (*doblock)(uint32_t, uint32_t))
{
	uint32_t ib[SFS_BLOCKSIZE/sizeof(uint32_t)];
	unsigned i;

	if (block == 0) {
		memset(ib, 0, sizeof(ib));
	}
	else {
		diskread(ib, block);
	}
	for (i=0; i<ARRAYCOUNT(ib) && fileblock < numblocks; i++) {
		if (indirection > 1) {
			fileblock = traverse_ib(fileblock, numblocks,
						SWAP32(ib[i]), indirection-1,
						doblock);
		}
		else {
			doblock(fileblock++, SWAP32(ib[i]));
		}
	}
	return fileblock;
}

static
void
traverse(const struct sfs_dinode *sfi, void (*doblock)(uint32_t, uint32_t))
{
	uint32_t fileblock;
	uint32_t numblocks;
	unsigned i;

	numblocks = DIVROUNDUP(SWAP32(sfi->sfi_size), SFS_BLOCKSIZE);

	fileblock = 0;
	for (i=0; i<SFS_NDIRECT && fileblock < numblocks; i++) {
		doblock(fileblock++, SWAP32(sfi->sfi_direct[i]));
	}
	if (fileblock < numblocks) {
		fileblock = traverse_ib(fileblock, numblocks,
					SWAP32(sfi->sfi_indirect), 1, doblock);
	}
	if (fileblock < numblocks) {
		fileblock = traverse_ib(fileblock, numblocks,
				       SWAP32(sfi->sfi_dindirect), 2, doblock);
	}
	if (fileblock < numblocks) {
		fileblock = traverse_ib(fileblock, numblocks,
				       SWAP32(sfi->sfi_tindirect), 3, doblock);
	}
	assert(fileblock == numblocks);
}

static
void
dumpdirblock(uint32_t fileblock, uint32_t diskblock)
{
	struct sfs_direntry sds[SFS_BLOCKSIZE/sizeof(struct sfs_direntry)];
	int nsds = SFS_BLOCKSIZE/sizeof(struct sfs_direntry);
	int i;

	(void)fileblock;
	if (diskblock == 0) {
		printf("    [block %u - empty]\n", diskblock);
		return;
	}
	diskread(&sds, diskblock);

	printf("    [block %u]\n", diskblock);
	for (i=0; i<nsds; i++) {
		uint32_t ino = SWAP32(sds[i].sfd_ino);
		if (ino==SFS_NOINO) {
			printf("        [free entry]\n");
		}
		else {
			sds[i].sfd_name[SFS_NAMELEN-1] = 0; /* just in case */
			printf("        %u %s\n", ino, sds[i].sfd_name);
		}
	}
}

static
void
dumpdir(uint32_t ino, const struct sfs_dinode *sfi)
{
	int nentries;

	nentries = SWAP32(sfi->sfi_size) / sizeof(struct sfs_direntry);
	if (SWAP32(sfi->sfi_size) % sizeof(struct sfs_direntry) != 0) {
		warnx("Warning: dir size is not a multiple of dir entry size");
	}
	printf("Directory contents for inode %u: %d entries\n", ino, nentries);
	traverse(sfi, dumpdirblock);
}

static
void
recursedirblock(uint32_t fileblock, uint32_t diskblock)
{
	struct sfs_direntry sds[SFS_BLOCKSIZE/sizeof(struct sfs_direntry)];
	int nsds = SFS_BLOCKSIZE/sizeof(struct sfs_direntry);
	int i;

	(void)fileblock;
	if (diskblock == 0) {
		return;
	}
	diskread(&sds, diskblock);

	for (i=0; i<nsds; i++) {
		uint32_t ino = SWAP32(sds[i].sfd_ino);
		if (ino==SFS_NOINO) {
			continue;
		}
		sds[i].sfd_name[SFS_NAMELEN-1] = 0; /* just in case */
		if (!strcmp(sds[i].sfd_name, ".") ||
		    !strcmp(sds[i].sfd_name, "..")) {
			continue;
		}
		dumpinode(ino, sds[i].sfd_name);
	}
}

static
void
recursedir(uint32_t ino, const struct sfs_dinode *sfi)
{
	int nentries;

	nentries = SWAP32(sfi->sfi_size) / sizeof(struct sfs_direntry);
	printf("Recursing into directory %u: %d entries\n", ino, nentries);
	traverse(sfi, recursedirblock);
	printf("Done with directory %u\n", ino);
}

static
void dumpfileblock(uint32_t fileblock, uint32_t diskblock)
{
	uint8_t data[SFS_BLOCKSIZE];
	unsigned i, j;
	char tmp[128];

	if (diskblock == 0) {
		printf("    0x%6x  [sparse]\n", fileblock * SFS_BLOCKSIZE);
		return;
	}

	diskread(data, diskblock);
	for (i=0; i<SFS_BLOCKSIZE; i++) {
		if (i % 16 == 0) {
			snprintf(tmp, sizeof(tmp), "0x%x",
				 fileblock * SFS_BLOCKSIZE + i);
			printf("%8s", tmp);
		}
		if (i % 8 == 0) {
			printf("  ");
		}
		else {
			printf(" ");
		}
		printf("%02x", data[i]);
		if (i % 16 == 15) {
			printf("  ");
			for (j = i-15; j<=i; j++) {
				if (data[j] < 32 || data[j] > 126) {
					putchar('.');
				}
				else {
					putchar(data[j]);
				}
			}
			printf("\n");
		}
	}
}

static
void
dumpfile(uint32_t ino, const struct sfs_dinode *sfi)
{
	printf("File contents for inode %u:\n", ino);
	traverse(sfi, dumpfileblock);
}

static
void
dumpinode(uint32_t ino, const char *name)
{
	struct sfs_dinode sfi;
	const char *typename;
	char tmp[128];
	unsigned i;

	diskread(&sfi, ino);

	printf("Inode %u", ino);
	if (name != NULL) {
		printf(" (%s)", name);
	}
	printf("\n");
	printf("--------------\n");

	switch (SWAP16(sfi.sfi_type)) {
	    case SFS_TYPE_FILE: typename = "regular file"; break;
	    case SFS_TYPE_DIR: typename = "directory"; break;
	    default: typename = "invalid"; break;
	}
	dumpvalf("Type", "%u (%s)", SWAP16(sfi.sfi_type), typename);
	dumpvalf("Size", "%u", SWAP32(sfi.sfi_size));
	dumpvalf("Link count", "%u", SWAP16(sfi.sfi_linkcount));
	printf("\n");

        printf("    Direct blocks:\n");
        for (i=0; i<SFS_NDIRECT; i++) {
		if (i % 4 == 0) {
			printf("@%-2u    ", i);
		}
		/*
		 * Assume the disk size might be > 64K sectors (which
		 * would be 32M) but is < 1024K sectors (512M) so we
		 * need up to 5 hex digits for a block number. And
		 * assume it's actually < 1 million sectors so we need
		 * only up to 6 decimal digits. The complete block
		 * number print then needs up to 16 digits.
		 */
		snprintf(tmp, sizeof(tmp), "%u (0x%x)",
			 SWAP32(sfi.sfi_direct[i]), SWAP32(sfi.sfi_direct[i]));
		printf("  %-16s", tmp);
		if (i % 4 == 3) {
			printf("\n");
		}
	}
	if (i % 4 != 0) {
		printf("\n");
	}
	printf("    Indirect block: %u (0x%x)\n",
	       SWAP32(sfi.sfi_indirect), SWAP32(sfi.sfi_indirect));
	printf("    Double indirect block: %u (0x%x)\n",
	       SWAP32(sfi.sfi_dindirect), SWAP32(sfi.sfi_dindirect));
	printf("    Triple indirect block: %u (0x%x)\n",
	       SWAP32(sfi.sfi_tindirect), SWAP32(sfi.sfi_tindirect));
	for (i=0; i<ARRAYCOUNT(sfi.sfi_waste); i++) {
		if (sfi.sfi_waste[i] != 0) {
			printf("    Word %u in waste area: 0x%x\n",
			       i, SWAP32(sfi.sfi_waste[i]));
		}
	}

	if (doindirect) {
		dumpindirect(SWAP32(sfi.sfi_indirect), 1);
		dumpindirect(SWAP32(sfi.sfi_dindirect), 2);
		dumpindirect(SWAP32(sfi.sfi_tindirect), 3);
	}

	if (SWAP16(sfi.sfi_type) == SFS_TYPE_DIR && dodirs) {
		dumpdir(ino, &sfi);
	}
	if (SWAP16(sfi.sfi_type) == SFS_TYPE_FILE && dofiles) {
		dumpfile(ino, &sfi);
	}
	if (SWAP16(sfi.sfi_type) == SFS_TYPE_DIR && recurse) {
		recursedir(ino, &sfi);
	}
}

////////////////////////////////////////////////////////////
// main

static
void
usage(void)
{
	warnx("Usage: dumpsfs [options] device/diskfile");
	warnx("   -s: dump superblock");
	warnx("   -b: dump free block bitmap");
	warnx("   -j: dump journal");
	warnx("   -J: physical dump of journal");
	warnx("   -i ino: dump specified inode");
	warnx("   -I: dump indirect blocks");
	warnx("   -f: dump file contents");
	warnx("   -d: dump directory contents");
	warnx("   -r: recurse into directory contents");
	warnx("   -a: equivalent to -sbdfr -i 1");
	errx(1, "   Default is -i 1");
}

int
main(int argc, char **argv)
{
	bool dosb = false;
	bool dofreemap = false;
	bool dojournal = false;
	bool dophysjournal = false;
	uint32_t dumpino = 0;
	const char *dumpdisk = NULL;

	int i, j;
	uint32_t nblocks;

#ifdef HOST
	/* Don't do this; it frobs the tty and you can't pipe to less */
	/*hostcompat_init(argc, argv);*/
	hostcompat_progname = argv[0];
#endif

	for (i=1; i<argc; i++) {
		if (argv[i][0] == '-') {
			for (j=1; argv[i][j]; j++) {
				switch (argv[i][j]) {
				    case 's': dosb = true; break;
				    case 'b': dofreemap = true; break;
				    case 'j': dojournal = true; break;
				    case 'J': dophysjournal = true; break;
				    case 'i':
					if (argv[i][j+1] == 0) {
						dumpino = atoi(argv[++i]);
					}
					else {
						dumpino = atoi(argv[i]+j+1);
						j = strlen(argv[i]);
					}
					/* XXX ugly */
					goto nextarg;
				    case 'I': doindirect = true; break;
				    case 'f': dofiles = true; break;
				    case 'd': dodirs = true; break;
				    case 'r': recurse = true; break;
				    case 'a':
					dosb = true;
					dofreemap = true;
					if (dumpino == 0) {
						dumpino = SFS_ROOTDIR_INO;
					}
					doindirect = true;
					dofiles = true;
					dodirs = true;
					recurse = true;
					break;
				    default:
					usage();
					break;
				}
			}
		}
		else {
			if (dumpdisk != NULL) {
				usage();
			}
			dumpdisk = argv[i];
		}
	 nextarg:
		;
	}
	if (dumpdisk == NULL) {
		usage();
	}

	if (!dosb && !dofreemap && !dojournal && !dophysjournal &&
	    dumpino == 0) {
		dumpino = SFS_ROOTDIR_INO;
	}

	opendisk(dumpdisk);
	nblocks = readsb();

	if (dosb) {
		dumpsb();
	}
	if (dofreemap) {
		dumpfreemap(nblocks);
	}
	if (dophysjournal) {
		dumpphysjournal();
	}
	if (dojournal) {
		dumpjournal();
	}
	if (dumpino != 0) {
		dumpinode(dumpino, NULL);
	}

	closedisk();

	return 0;
}
