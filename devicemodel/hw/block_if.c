/*-
 * Copyright (c) 2013  Peter Grehan <grehan@freebsd.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 */

#include <sys/param.h>
#include <sys/queue.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/falloc.h>
#include <linux/fs.h>
#include <errno.h>
#include <err.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <liburing.h>

#include "dm.h"
#include "block_if.h"
#include "ahci.h"
#include "dm_string.h"
#include "log.h"
#include "iothread.h"

/*
 * Notes:
 * The F_OFD_SETLK support is introduced in glibc 2.20.
 * The glibc version on target board is above 2.20.
 * The following code temporarily fixes up building issues on Ubuntu 14.04,
 * where the glibc version is 2.19 by default.
 * Theoretically we should use cross-compiling tool to compile applications.
 */
#ifndef F_OFD_SETLK
#define F_OFD_SETLK	37
#endif

#define BLOCKIF_SIG	0xb109b109

#define BLOCKIF_NUMTHR	8
#define BLOCKIF_MAXREQ	(64 + BLOCKIF_NUMTHR)
#define MAX_DISCARD_SEGMENT	256

#define AIO_MODE_THREAD_POOL	0
#define AIO_MODE_IO_URING	1

/* the max number of entries for the io_uring submission/completion queue */
#define MAX_IO_URING_ENTRIES	256

/*
 * Debug printf
 */
static int block_if_debug;
#define DPRINTF(params) do { if (block_if_debug) pr_dbg params; } while (0)
#define WPRINTF(params) (pr_err params)

enum blockop {
	BOP_READ,
	BOP_WRITE,
	BOP_FLUSH,
	BOP_DISCARD
};

enum blockstat {
	BST_FREE,
	BST_BLOCK,
	BST_PEND,
	BST_BUSY,
	BST_DONE
};

struct blockif_elem {
	TAILQ_ENTRY(blockif_elem) link;
	struct blockif_req  *req;
	enum blockop	     op;
	enum blockstat	     status;
	pthread_t            tid;
	off_t		     block;
};

struct blockif_queue {
	int			closing;

	pthread_t		btid[BLOCKIF_NUMTHR];
	pthread_mutex_t		mtx;
	pthread_cond_t		cond;

	/* Request elements and free/pending/busy queues */
	TAILQ_HEAD(, blockif_elem) freeq;
	TAILQ_HEAD(, blockif_elem) pendq;
	TAILQ_HEAD(, blockif_elem) busyq;
	struct blockif_elem	reqs[BLOCKIF_MAXREQ];

	int			in_flight;
	struct io_uring		ring;
	struct iothread_mevent	iomvt;
	struct iothread_ctx	*ioctx;

	struct blockif_ctxt	*bc;
};

struct blockif_ops {
	int aio_mode;

	int (*init)(struct blockif_queue *, char *);
	void (*deinit)(struct blockif_queue *);

	void (*mutex_lock)(pthread_mutex_t *);
	void (*mutex_unlock)(pthread_mutex_t *);

	void (*request)(struct blockif_queue *);
};

struct blockif_ctxt {
	int			fd;
	int			isblk;
	int			candiscard;
	int			rdonly;
	off_t			size;
	int			sub_file_assign;
	off_t			sub_file_start_lba;
	struct flock		fl;
	int			sectsz;
	int			psectsz;
	int			psectoff;
	int			max_discard_sectors;
	int			max_discard_seg;
	int			discard_sector_alignment;
	struct blockif_queue	*bqs;
	int			bq_num;

	int			aio_mode;
	const struct blockif_ops *ops;

	/* write cache enable */
	uint8_t			wce;

	/* whether bypass the Service VM's page cache or not */
	uint8_t			bypass_host_cache;

	/*
	 * whether enable BST_BLOCK logic in blockif_dequeue/blockif_complete or not.
	 *
	 * If the BST_BLOCK logic is enabled, following check would be done:
	 *     if the current request is consecutive to any request in penq or busyq,
	 *     current request's status is set to BST_BLOCK. Then, this request is blocked until the prior request,
	 *     which blocks it, is completed.
	 * It indicates that consecutive requests are executed sequentially.
	 */
	uint8_t			bst_block;
};

static pthread_once_t blockif_once = PTHREAD_ONCE_INIT;

struct blockif_sig_elem {
	pthread_mutex_t			mtx;
	pthread_cond_t			cond;
	int				pending;
	struct blockif_sig_elem		*next;
};

struct discard_range {
	uint64_t sector;
	uint32_t num_sectors;
	uint32_t flags;
};

static struct blockif_sig_elem *blockif_bse_head;

static int
blockif_flush_cache(struct blockif_ctxt *bc)
{
	int err;

	err = 0;
	if (!bc->wce) {
		if (fsync(bc->fd))
			err = errno;
	}
	return err;
}

static int
blockif_enqueue(struct blockif_queue *bq, struct blockif_req *breq,
		enum blockop op)
{
	struct blockif_elem *be, *tbe;
	off_t off;
	int i;

	be = TAILQ_FIRST(&bq->freeq);
	if (be == NULL || be->status != BST_FREE) {
		WPRINTF(("%s: failed to get element from freeq\n", __func__));
		return 0;
	}
	TAILQ_REMOVE(&bq->freeq, be, link);
	be->req = breq;
	be->op = op;

	be->status = BST_PEND;
	if (bq->bc->bst_block == 1) {
		switch (op) {
		case BOP_READ:
		case BOP_WRITE:
		case BOP_DISCARD:
			off = breq->offset;
			for (i = 0; i < breq->iovcnt; i++)
				off += breq->iov[i].iov_len;
			break;
		default:
			/* off = OFF_MAX; */
			off = 1 << (sizeof(off_t) - 1);
		}
		be->block = off;
		TAILQ_FOREACH(tbe, &bq->pendq, link) {
			if (tbe->block == breq->offset)
				break;
		}
		if (tbe == NULL) {
			TAILQ_FOREACH(tbe, &bq->busyq, link) {
				if (tbe->block == breq->offset)
					break;
			}
		}
		if (tbe != NULL)
			be->status = BST_BLOCK;
	}

	TAILQ_INSERT_TAIL(&bq->pendq, be, link);
	return (be->status == BST_PEND);
}

static int
blockif_dequeue(struct blockif_queue *bq, pthread_t t, struct blockif_elem **bep)
{
	struct blockif_elem *be;

	TAILQ_FOREACH(be, &bq->pendq, link) {
		if (be->status == BST_PEND)
			break;
	}
	if (be == NULL)
		return 0;
	TAILQ_REMOVE(&bq->pendq, be, link);
	be->status = BST_BUSY;
	be->tid = t;
	TAILQ_INSERT_TAIL(&bq->busyq, be, link);
	*bep = be;
	return 1;
}

static void
blockif_complete(struct blockif_queue *bq, struct blockif_elem *be)
{
	struct blockif_elem *tbe;

	if (be->status == BST_DONE || be->status == BST_BUSY)
		TAILQ_REMOVE(&bq->busyq, be, link);
	else
		TAILQ_REMOVE(&bq->pendq, be, link);

	if (bq->bc->bst_block == 1) {
		TAILQ_FOREACH(tbe, &bq->pendq, link) {
			if (tbe->req->offset == be->block)
				tbe->status = BST_PEND;
		}
	}
	be->tid = 0;
	be->status = BST_FREE;
	be->req = NULL;
	TAILQ_INSERT_TAIL(&bq->freeq, be, link);
}

static int
discard_range_validate(struct blockif_ctxt *bc, off_t start, off_t size)
{
	off_t start_sector = start / DEV_BSIZE;
	off_t size_sector = size / DEV_BSIZE;

	if (!size || (start + size) > (bc->size + bc->sub_file_start_lba))
		return -1;

	if ((size_sector > bc->max_discard_sectors) ||
			(bc->discard_sector_alignment &&
			start_sector % bc->discard_sector_alignment))
		return -1;
	return 0;
}

static int
blockif_process_discard(struct blockif_ctxt *bc, struct blockif_req *br)
{
	int err;
	struct discard_range *range;
	int n_range, i, segment;
	off_t arg[MAX_DISCARD_SEGMENT][2];

	err = 0;
	n_range = 0;
	segment = 0;
	if (!bc->candiscard)
		return EOPNOTSUPP;

	if (bc->rdonly)
		return EROFS;

	if (br->iovcnt == 1) {
		/* virtio-blk use iov to transfer discard range */
		n_range = br->iov[0].iov_len/sizeof(*range);
		range = br->iov[0].iov_base;
		for (i = 0; i < n_range; i++) {
			arg[i][0] = range[i].sector * DEV_BSIZE +
					bc->sub_file_start_lba;
			arg[i][1] = range[i].num_sectors * DEV_BSIZE;
			segment++;
			if (segment > bc->max_discard_seg) {
				WPRINTF(("segment > max_discard_seg\n"));
				return EINVAL;
			}
			if (discard_range_validate(bc, arg[i][0], arg[i][1])) {
				WPRINTF(("range [%ld: %ld] is invalid\n", arg[i][0], arg[i][1]));
				return EINVAL;
			}
		}
	} else {
		/* ahci parse discard range to br->offset and br->reside */
		arg[0][0] = br->offset + bc->sub_file_start_lba;
		arg[0][1] = br->resid;
		segment = 1;
	}
	for (i = 0; i < segment; i++) {
		if (bc->isblk) {
			err = ioctl(bc->fd, BLKDISCARD, arg[i]);
		} else {
			/* FALLOC_FL_PUNCH_HOLE:
			 *	Deallocates space in the byte range starting at offset and
			 *	continuing for length bytes.  After a successful call,
			 *	subsequent reads from this range will return zeroes.
			 * FALLOC_FL_KEEP_SIZE:
			 *	Do not modify the apparent length of the file.
			 */
			err = fallocate(bc->fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
				arg[i][0], arg[i][1]);
			if (!err)
				err = fdatasync(bc->fd);
		}
		if (err) {
			WPRINTF(("Failed to discard offset=%ld nbytes=%ld err code: %d\n",
				 arg[i][0], arg[i][1], err));
			return err;
		}
	}
	br->resid = 0;

	return 0;
}

static void
blockif_init_iov_align_info(struct blockif_req *br)
{
	int i, size;
	struct br_align_info *info = &br->align_info;

	size = 0;
	info->is_iov_base_aligned = true;
	info->is_iov_len_aligned = true;

	for (i = 0; i < br->iovcnt; i++) {
		size += br->iov[i].iov_len;

		if ((uint64_t)(br->iov[i].iov_base) % info->alignment) {
			info->is_iov_base_aligned = false;
		}

		if (br->iov[i].iov_len % info->alignment) {
			info->is_iov_len_aligned = false;
		}
	}

	info->org_size = size;

	return;
}

/* only for debug purpose */
static void
blockif_dump_align_info(struct blockif_req *br)
{
	struct br_align_info *info = &br->align_info;
	int i;

	if (!info->is_offset_aligned) {
		DPRINTF(("%s: Misaligned offset 0x%llx \n\r", __func__, (info->aligned_dn_start + info->head)));
	}

	/* iov info */
	if (!info->is_iov_base_aligned) {
		DPRINTF(("%s: Misaligned iov_base \n\r", __func__));
	}
	if (!info->is_iov_len_aligned) {
		DPRINTF(("%s: Misaligned iov_len \n\r", __func__));
	}

	DPRINTF(("%s: alignment %d, br->iovcnt %d \n\r", __func__, info->alignment, br->iovcnt));
	for (i = 0; i < br->iovcnt; i++) {
		DPRINTF(("%s: iov[%d].iov_base 0x%llx (remainder %d), iov[%d].iov_len %d (remainder %d) \n\r",
			__func__,
			i, (uint64_t)(br->iov[i].iov_base), (uint64_t)(br->iov[i].iov_base) % info->alignment,
			i, br->iov[i].iov_len, (br->iov[i].iov_len) % info->alignment));
	}

	/* overall info */
	DPRINTF(("%s: head %d, tail %d, org_size %d, bounced_size %d, aligned_dn_start 0x%lx aligned_dn_end 0x%lx \n\r",
		__func__, info->head, info->tail, info->org_size, info->bounced_size,
		info->aligned_dn_start, info->aligned_dn_end));
}

/*
 *  |<------------------------------------- bounced_size --------------------------------->|
 *  |<-------- alignment ------->|                            |<-------- alignment ------->|
 *  |<--- head --->|<------------------------ org_size ---------------------->|<-- tail -->|
 *  |              |             |                            |               |            |
 *  *--------------$-------------*----------- ... ------------*---------------$------------*
 *  |              |             |                            |               |            |
 *  |              start                                                      end          |
 *  aligned_dn_start                                          aligned_dn_end
 *  |__________head_area_________|                            |__________tail_area_________|
 *  |<--- head --->|             |                            |<-- end_rmd -->|<-- tail -->|
 *  |<-------- alignment ------->|                            |<-------- alignment ------->|
 *
 *
 * Original access area:
 *  - start = br->offset + bc->sub_file_start_lba
 *  - org_size = SUM of org_iov[i].iov_len
 *  - end = start + org_size
 *
 *
 * Head area to be bounced:
 *  - head = start % alignment
 *  - aligned_dn_start = start - head
 *     head        | head_area
 *    -------------|-------------
 *     0           | not exist
 *     non-zero    | exist
 *
 *
 * Tail area to be bounced:
 *  - end_rmd = end % alignment
 *  - aligned_dn_end = end - end_rmd
 *     end_rmd     | tail                  | tail_area
 *    -------------|-----------------------|------------------
 *     0           | 0                     | not exist
 *     non-zero    | alignment - end_rmd   | exist
 *
 *
 * Overall bounced area:
 *  - bounced_size = head + org_size + tail
 *
 *
 * Use a single bounce_iov to do the aligned READ/WRITE.
 *  - bounce_iov cnt = 1
 *  - bounce_iov.iov_base = return of posix_memalign (aligned to @alignment)
 *  - bounce_iov.len = bounced_size
 *  - Accessing from the offset `aligned_dn_start`
 *
 *
 * For READ access:
 *    1. Do the aligned READ (using `bounce_iov`) from the offset `aligned_dn_start`, with the length `bounced_size`.
 *    2. AFTER the aligned READ is completed, copy the data from the bounce_iov to the org_iov.
 *       from                          | length
 *       ------------------------------|---------------
 *       bounce_iov.iov_base + head    | org_size
 *
 *
 * For WRITE access:
 *    1. BEFORE the aligned WRITE is conducted, construct the bounced data with three parts in bounce_iov.
 *        (a). If head is not 0, get data of first alignment area -> head_area data (by doing aligned read)
 *             from                | length
 *             --------------------|---------------
 *             aligned_dn_start    | alignment
 *
 *        (b). If tail is not 0, get data of last alignment area -> tail_area data (by doing aligned read)
 *             from                | length
 *             --------------------|---------------
 *             aligned_dn_end      | alignment
 *
 *        (c). Construct the bounced data in bounce_iov
 *             from                | to               | length        | source
 *             --------------------|------------------|---------------|---------------------------------
 *             aligned_dn_start    | start            | head          | head_area data from block device
 *             start               | end              | org_size      | data specified in org_iov[]
 *             end                 | end + tail       | tail          | tail_area data from block device
 *    2. Do the aligned WRITE (using `bounce_iov`) from the offset `aligned_dn_start`, with the length `bounced_size`.
 *
 *
 */
static void
blockif_init_alignment_info(struct blockif_ctxt *bc, struct blockif_req *br)
{
	struct br_align_info *info = &br->align_info;
	uint32_t alignment = bc->sectsz;
	uint32_t end_rmd;
	off_t start, end;
	bool all_aligned;

	/* If O_DIRECT flag is not used, does NOT need to initialize the alignment info. */
	if (!bc->bypass_host_cache) {
		info->need_conversion = false;
		return;
	}

	start = br->offset + bc->sub_file_start_lba;
	info->is_offset_aligned = (!(start % alignment));

	info->alignment = alignment;
	blockif_init_iov_align_info(br);

	all_aligned = (info->is_offset_aligned && info->is_iov_base_aligned && info->is_iov_len_aligned);
	/*
	 * If O_DIRECT flag is used and the request is aligned,
	 * does NOT need to initialize the alignment info further.
	 */
	if (all_aligned) {
		info->need_conversion = false;
		return;
	}
	info->need_conversion = true;

	/* head area */
	info->head = start % alignment;
	info->aligned_dn_start = start - info->head;

	/* tail area */
	end = start + info->org_size;
	end_rmd = (end % alignment);
	info->tail = (end_rmd == 0) ? (0) : (alignment - end_rmd);
	info->aligned_dn_end = end - end_rmd;

	/* overall bounced area */
	info->bounced_size = info->head + info->org_size + info->tail;

	/* only for debug purpose */
	blockif_dump_align_info(br);

	return;
}

/*
 * Use a single bounce_iov to do the aligned READ/WRITE.
 *  - bounce_iov cnt = 1
 *  - bounce_iov.iov_base = return of posix_memalign (aligned to @alignment)
 *  - bounce_iov.len = bounced_size
 *  - Accessing from the offset `aligned_dn_start`
 */
static int
blockif_init_bounce_iov(struct blockif_req *br)
{
	int ret = 0;
	void *bounce_buf = NULL;
	struct br_align_info *info = &br->align_info;

	ret = posix_memalign(&bounce_buf, info->alignment, info->bounced_size);
	if (ret != 0) {
		bounce_buf = NULL;
		pr_err("%s: posix_memalign fails, error %s \n", __func__, strerror(-ret));
	} else {
		info->bounce_iov.iov_base = bounce_buf;
		info->bounce_iov.iov_len = info->bounced_size;
	}

	return ret;
}

static void
blockif_deinit_bounce_iov(struct blockif_req *br)
{
	struct br_align_info *info = &br->align_info;

	if (info->bounce_iov.iov_base == NULL) {
		pr_err("%s: info->bounce_iov.iov_base is NULL %s \n", __func__);
		return;
	}

	free(info->bounce_iov.iov_base);
	info->bounce_iov.iov_base = NULL;
}

/*
 * For READ access:
 *    1. Do the aligned READ (using `bounce_iov`) from the offset `aligned_dn_start`, with the length `bounced_size`.
 *    2. AFTER the aligned READ is completed, copy the data from the bounce_iov to the org_iov.
 *       from                          | length
 *       ------------------------------|---------------
 *       bounce_iov.iov_base + head    | org_size
 */
static void
blockif_complete_bounced_read(struct blockif_req *br)
{
	struct iovec *iov = br->iov;
	struct br_align_info *info = &br->align_info;
	int length = info->org_size;
	int i, len, done;

	if (info->bounce_iov.iov_base == NULL) {
		pr_err("%s: info->bounce_iov.iov_base is NULL %s \n", __func__);
		return;
	}

	done = info->head;
	for (i = 0; i < br->iovcnt; i++) {
		len = (iov[i].iov_len < length) ? iov[i].iov_len : length;
		memcpy(iov[i].iov_base, info->bounce_iov.iov_base + done, len);

		done += len;
		length -= len;
		if (length <= 0)
			break;
	}

	return;
};

/*
 * It is used to read out the head/tail area to construct the bounced data.
 *
 * Allocate an aligned buffer for @b_iov and do an aligned read from @offset (with length @alignment).
 * @offset shall be guaranteed to be aligned by caller (either aligned_dn_start or aligned_dn_end).
 */
static int
blockif_read_head_or_tail_area(int fd, struct iovec *b_iov, off_t offset, uint32_t alignment)
{
	int ret = 0;
	int bytes_read;
	void *area = NULL;

	ret = posix_memalign(&area, alignment, alignment);
	if (ret != 0) {
		area = NULL;
		pr_err("%s: posix_memalign fails, error %s \n", __func__, strerror(-ret));
		return ret;
	}

	b_iov->iov_base = area;
	b_iov->iov_len = alignment;
	bytes_read = preadv(fd, b_iov, 1, offset);

	if (bytes_read < 0) {
		pr_err("%s: read fails \n", __func__);
		ret = errno;
	}

	return ret;
}

/*
 * For WRITE access:
 *    1. BEFORE the aligned WRITE is conducted, construct the bounced data with three parts in bounce_iov.
 *        (a). If head is not 0, get data of first alignment area -> head_area data (by doing aligned read)
 *             from                | length
 *             --------------------|---------------
 *             aligned_dn_start    | alignment
 *
 *        (b). If tail is not 0, get data of last alignment area -> tail_area data (by doing aligned read)
 *             from                | length
 *             --------------------|---------------
 *             aligned_dn_end      | alignment
 *
 *        (c). Construct the bounced data in bounce_iov
 *             from                | to               | length        | source
 *             --------------------|------------------|---------------|---------------------------------
 *             aligned_dn_start    | start            | head          | head_area data from block device
 *             start               | end              | org_size      | data specified in org_iov[]
 *             end                 | end + tail       | tail          | tail_area data from block device
 *    2. Do the aligned WRITE (using `bounce_iov`) from the offset `aligned_dn_start`, with the length `bounced_size`.
 */
static int
blockif_init_bounced_write(struct blockif_ctxt *bc, struct blockif_req *br)
{
	struct iovec *iov = br->iov;
	struct br_align_info *info = &br->align_info;
	uint32_t alignment = info->alignment;
	struct iovec head_iov, tail_iov;
	uint32_t head = info->head;
	uint32_t tail = info->tail;
	int i, done, ret;

	ret = 0;

	if (info->bounce_iov.iov_base == NULL) {
		pr_err("%s: info->bounce_iov.iov_base is NULL \n", __func__);
		return -1;
	}

	memset(&head_iov, 0, sizeof(head_iov));
	memset(&tail_iov, 0, sizeof(tail_iov));

	/*
	 * If head is not 0, get data of first alignment area, head_area data (by doing aligned read)
	 *  from                | length
	 *  --------------------|---------------
	 *  aligned_dn_start    | alignment
	 */
	if (head != 0) {
		ret = blockif_read_head_or_tail_area(bc->fd, &head_iov, info->aligned_dn_start, alignment);
		if (ret < 0) {
			pr_err("%s: fails to read out the head area \n", __func__);
			goto end;
		}
	}

	/*
	 * If tail is not 0, get data of last alignment area, tail_area data (by doing aligned read)
	 *  from                | length
	 *  --------------------|---------------
	 *  aligned_dn_end      | alignment
	 */
	if (tail != 0) {
		ret = blockif_read_head_or_tail_area(bc->fd, &tail_iov, info->aligned_dn_end, alignment);
		if (ret < 0) {
			pr_err("%s: fails to read out the tail area \n", __func__);
			goto end;
		}
	}

	done = 0;
	/*
	 * Construct the bounced data in bounce_iov
	 *  from                | to               | length        | source
	 *  --------------------|------------------|---------------|---------------------------------
	 *  aligned_dn_start    | start            | head          | head_area data from block device
	 *  start               | end              | org_size      | data specified in org_iov[]
	 *  end                 | end + tail       | tail          | tail_area data from block device
	 */
	if (head_iov.iov_base != NULL) {
		memcpy(info->bounce_iov.iov_base, head_iov.iov_base, head);
		done += head;
	}

	/* data specified in org_iov[] */
	for (i = 0; i < br->iovcnt; i++) {
		memcpy(info->bounce_iov.iov_base + done, iov[i].iov_base, iov[i].iov_len);
		done += iov[i].iov_len;
	}

	if (tail_iov.iov_base != NULL) {
		memcpy(info->bounce_iov.iov_base + done, tail_iov.iov_base + alignment - tail, tail);
		done += tail;
	}

end:
	if (head_iov.iov_base != NULL) {
		free(head_iov.iov_base);
	}

	if (tail_iov.iov_base != NULL) {
		free(tail_iov.iov_base);
	}

	return ret;
};

static void
blockif_proc(struct blockif_queue *bq, struct blockif_elem *be)
{
	struct blockif_req *br;
	struct blockif_ctxt *bc;
	struct br_align_info *info;
	ssize_t len, iovcnt;
	struct iovec *iovecs;
	off_t offset;
	int err;

	br = be->req;
	bc = bq->bc;
	info = &br->align_info;
	err = 0;

	if ((be->op == BOP_READ) || (be->op == BOP_WRITE)) {
		if (info->need_conversion) {
			/* bounce_iov has been initialized in blockif_request */
			iovecs = &(info->bounce_iov);
			iovcnt = 1;
			offset = info->aligned_dn_start;
		} else {
			/* use the original iov if no conversion is required */
			iovecs = br->iov;
			iovcnt = br->iovcnt;
			offset = br->offset + bc->sub_file_start_lba;
		}
	}

	switch (be->op) {
	case BOP_READ:
		len = preadv(bc->fd, iovecs, iovcnt, offset);
		if (info->need_conversion) {
			blockif_complete_bounced_read(br);
			blockif_deinit_bounce_iov(br);
		}

		if (len < 0)
			err = errno;
		else
			br->resid -= len;
		break;
	case BOP_WRITE:
		if (bc->rdonly) {
			err = EROFS;
			break;
		}

		len = pwritev(bc->fd, iovecs, iovcnt, offset);
		if (info->need_conversion) {
			blockif_deinit_bounce_iov(br);
		}

		if (len < 0)
			err = errno;
		else {
			br->resid -= len;
			err = blockif_flush_cache(bc);
		}
		break;
	case BOP_FLUSH:
		if (fsync(bc->fd))
			err = errno;
		break;
	case BOP_DISCARD:
		err = blockif_process_discard(bc, br);
		break;
	default:
		err = EINVAL;
		break;
	}

	be->status = BST_DONE;

	(*br->callback)(br, err);
}

static void *
blockif_thr(void *arg)
{
	struct blockif_queue *bq;
	struct blockif_elem *be;
	pthread_t t;

	bq = arg;
	t = pthread_self();

	pthread_mutex_lock(&bq->mtx);

	for (;;) {
		while (blockif_dequeue(bq, t, &be)) {
			pthread_mutex_unlock(&bq->mtx);
			blockif_proc(bq, be);
			pthread_mutex_lock(&bq->mtx);
			blockif_complete(bq, be);
		}
		/* Check ctxt status here to see if exit requested */
		if (bq->closing)
			break;
		pthread_cond_wait(&bq->cond, &bq->mtx);
	}

	pthread_mutex_unlock(&bq->mtx);
	pthread_exit(NULL);
	return NULL;
}

static void
blockif_sigcont_handler(int signal)
{
	struct blockif_sig_elem *bse;

	WPRINTF(("block_if sigcont handler!\n"));

	for (;;) {
		/*
		 * Process the entire list even if not intended for
		 * this thread.
		 */
		do {
			bse = blockif_bse_head;
			if (bse == NULL)
				return;
		} while (!__sync_bool_compare_and_swap(
					(uintptr_t *)&blockif_bse_head,
					(uintptr_t)bse,
					(uintptr_t)bse->next));

		pthread_mutex_lock(&bse->mtx);
		bse->pending = 0;
		pthread_cond_signal(&bse->cond);
		pthread_mutex_unlock(&bse->mtx);
	}
}

static void
blockif_init(void)
{
	signal(SIGCONT, blockif_sigcont_handler);
}

/*
 * This function checks if the sub file range, specified by sub_start and
 * sub_size, has any overlap with other sub file ranges with write access.
 */
static int
sub_file_validate(struct blockif_ctxt *bc, int fd, int read_only,
		  off_t sub_start, off_t sub_size)
{
	struct flock *fl = &bc->fl;

	memset(fl, 0, sizeof(struct flock));
	fl->l_whence = SEEK_SET;	/* offset base is start of file */
	if (read_only)
		fl->l_type = F_RDLCK;
	else
		fl->l_type = F_WRLCK;
	fl->l_start = sub_start;
	fl->l_len = sub_size;

	/* use "open file description locks" to validate */
	if (fcntl(fd, F_OFD_SETLK, fl) == -1) {
		DPRINTF(("failed to lock subfile!\n"));
		return -1;
	}

	/* Keep file lock on to prevent other sub files, until DM exits */
	return 0;
}

void
sub_file_unlock(struct blockif_ctxt *bc)
{
	struct flock *fl;

	if (bc->sub_file_assign) {
		fl = &bc->fl;
		DPRINTF(("blockif: release file lock...\n"));
		fl->l_type = F_UNLCK;
		if (fcntl(bc->fd, F_OFD_SETLK, fl) == -1) {
			pr_err("blockif: failed to unlock subfile!\n");
			exit(1);
		}
		DPRINTF(("blockif: release done\n"));
	}
}

static int
thread_pool_init(struct blockif_queue *bq, char *tag)
{
	int i;
	char tname[MAXCOMLEN + 1];

	for (i = 0; i < BLOCKIF_NUMTHR; i++) {
		if (snprintf(tname, sizeof(tname), "%s-%d",
					tag, i) >= sizeof(tname)) {
			pr_err("blk thread name too long");
		}
		pthread_create(&bq->btid[i], NULL, blockif_thr, bq);
		pthread_setname_np(bq->btid[i], tname);
	}

	return 0;
}

static void
thread_pool_deinit(struct blockif_queue *bq)
{
	int i;
	void *jval;

	for (i = 0; i < BLOCKIF_NUMTHR; i++)
		pthread_join(bq->btid[i], &jval);
}

static inline void
thread_pool_mutex_lock(pthread_mutex_t *mutex)
{
	pthread_mutex_lock(mutex);
}

static inline void
thread_pool_mutex_unlock(pthread_mutex_t *mutex)
{
	pthread_mutex_unlock(mutex);
}

static void
thread_pool_request(struct blockif_queue *bq)
{
	pthread_cond_signal(&bq->cond);
}

static struct blockif_ops blockif_ops_thread_pool = {
	.aio_mode	= AIO_MODE_THREAD_POOL,

	.init		= thread_pool_init,
	.deinit		= thread_pool_deinit,

	.mutex_lock	= thread_pool_mutex_lock,
	.mutex_unlock	= thread_pool_mutex_unlock,

	.request	= thread_pool_request,
};

static bool
is_io_uring_supported_op(enum blockop op)
{
	return ((op == BOP_READ) || (op == BOP_WRITE) || (op == BOP_FLUSH));
}

static int
iou_submit_sqe(struct blockif_queue *bq, struct blockif_elem *be)
{
	int ret;
	struct io_uring *ring = &bq->ring;
	struct io_uring_sqe *sqes = io_uring_get_sqe(ring);
	struct blockif_req *br = be->req;
	struct blockif_ctxt *bc = bq->bc;
	struct br_align_info *info = &br->align_info;
	struct iovec *iovecs;
	size_t iovcnt;
	off_t offset;

	if (!sqes) {
		pr_err("%s: io_uring_get_sqe fails. NO available submission queue entry. \n", __func__);
		return -1;
	}

	if ((be->op == BOP_READ) || (be->op == BOP_WRITE)) {
		if (info->need_conversion) {
			/* bounce_iov has been initialized in blockif_request */
			iovecs = &(info->bounce_iov);
			iovcnt = 1;
			offset = info->aligned_dn_start;
		} else {
			/* use the original iov if no conversion is required */
			iovecs = br->iov;
			iovcnt = br->iovcnt;
			offset = br->offset + bc->sub_file_start_lba;
		}
	}

	switch (be->op) {
	case BOP_READ:
		io_uring_prep_readv(sqes, bc->fd, iovecs, iovcnt, offset);
		break;
	case BOP_WRITE:
		io_uring_prep_writev(sqes, bc->fd, iovecs, iovcnt, offset);
		break;
	case BOP_FLUSH:
		io_uring_prep_fsync(sqes, bc->fd, IORING_FSYNC_DATASYNC);
		break;
	default:
		/* is_io_uring_supported_op guarantees that this case will not occur */
		break;
	}

	io_uring_sqe_set_data(sqes, be);
	bq->in_flight++;
	ret = io_uring_submit(ring);
	if (ret < 0) {
		pr_err("%s: io_uring_submit fails, error %s \n", __func__, strerror(-ret));
	}

	return ret;
}

static void
iou_submit(struct blockif_queue *bq)
{
	int err = 0;
	struct blockif_elem *be;
	struct blockif_req *br;
	struct blockif_ctxt *bc = bq->bc;

	while (blockif_dequeue(bq, 0, &be)) {
		if (is_io_uring_supported_op(be->op)) {
			err = iou_submit_sqe(bq, be);

			/*
			 * -1 means that there is NO available submission queue entry (SQE) in the submission queue.
			 * Break the while loop here. Request can only be submitted when SQE is available.
			 */
			if (err == -1) {
				break;
			}
		} else {
			br = be->req;
			if (be->op == BOP_DISCARD) {
				err = blockif_process_discard(bc, br);
			} else {
				pr_err("%s: op %d is not supported \n", __func__, be->op);
				err = EINVAL;
			}
			be->status = BST_DONE;
			(*br->callback)(br, err);
			blockif_complete(bq, be);
		}
	}
	return;
}

static void
iou_process_completions(struct blockif_queue *bq)
{
	struct io_uring_cqe *cqes = NULL;
	struct blockif_elem *be;
	struct blockif_req *br;
	struct io_uring *ring = &bq->ring;
	int err = 0;

	while (io_uring_peek_cqe(ring, &cqes) == 0) {
		if (!cqes) {
			pr_err("%s: cqes is NULL \n", __func__);
			break;
		}

		be = io_uring_cqe_get_data(cqes);
		bq->in_flight--;
		io_uring_cqe_seen(ring, cqes);
		cqes = NULL;
		if (!be) {
			pr_err("%s: be is NULL \n", __func__);
			break;
		}

		br = be->req;
		if (!br) {
			pr_err("%s: br is NULL \n", __func__);
			break;
		}

		/* when a misaligned request is converted to an aligned one, need to do some post-work */
		if (br->align_info.need_conversion) {
			if (be->op == BOP_READ) {
				blockif_complete_bounced_read(br);
			}
			blockif_deinit_bounce_iov(br);
		}

		if (be->op == BOP_WRITE) {
			err = blockif_flush_cache(bq->bc);
		}

		be->status = BST_DONE;
		(*br->callback)(br, err);
		blockif_complete(bq, be);
	}

	return;
}

static void
iou_submit_and_reap(struct blockif_queue *bq)
{
	iou_submit(bq);

	if (bq->in_flight > 0) {
		iou_process_completions(bq);
	}

	return;
}

static void
iou_reap_and_submit(struct blockif_queue *bq)
{
	iou_process_completions(bq);

	if (!TAILQ_EMPTY(&bq->pendq)) {
		iou_submit(bq);
	}

	return;
}

static void
iou_completion_cb(void *arg)
{
	struct blockif_queue *bq = (struct blockif_queue *)arg;
	iou_reap_and_submit(bq);
}

static int
iou_set_iothread(struct blockif_queue *bq)
{
	int fd = bq->ring.ring_fd;
	int ret = 0;

	bq->iomvt.arg = bq;
	bq->iomvt.run = iou_completion_cb;
	bq->iomvt.fd = fd;

	ret = iothread_add(bq->ioctx, fd, &bq->iomvt);
	if (ret < 0) {
		pr_err("%s: iothread_add fails, error %d \n", __func__, ret);
	}
	return ret;
}

static int
iou_del_iothread(struct blockif_queue *bq)
{
	int fd = bq->ring.ring_fd;
	int ret = 0;

	ret = iothread_del(bq->ioctx, fd);
	if (ret < 0) {
		pr_err("%s: iothread_del fails, error %d \n", __func__, ret);
	}
	return ret;
}

static int
iou_init(struct blockif_queue *bq, char *tag __attribute__((unused)))
{
	int ret = 0;
	struct io_uring *ring = &bq->ring;

	/*
	 * - When Service VM owns more dedicated cores, IORING_SETUP_SQPOLL and IORING_SETUP_IOPOLL, along with NVMe
	 *   polling mechanism could benefit the performance.
	 * - When Service VM owns limited cores, the benefit of polling is also limited.
	 * As in most of the use cases, Service VM does not own much dedicated cores, IORING_SETUP_SQPOLL and
	 * IORING_SETUP_IOPOLL are not enabled by default.
	 */
	ret = io_uring_queue_init(MAX_IO_URING_ENTRIES, ring, 0);
	if (ret < 0) {
		pr_err("%s: io_uring_queue_init fails, error %d \n", __func__, ret);
	} else {
		ret = iou_set_iothread(bq);
		if (ret < 0) {
			pr_err("%s: iou_set_iothread fails \n", __func__);
		}
	}

	return ret;
}

static void
iou_deinit(struct blockif_queue *bq)
{
	struct io_uring *ring = &bq->ring;

	iou_del_iothread(bq);
	io_uring_queue_exit(ring);
}

static inline void iou_mutex_lock(pthread_mutex_t *mutex __attribute__((unused))) {}
static inline void iou_mutex_unlock(pthread_mutex_t *mutex __attribute__((unused))) {}

static struct blockif_ops blockif_ops_iou = {
	.aio_mode	= AIO_MODE_IO_URING,

	.init		= iou_init,
	.deinit		= iou_deinit,

	.mutex_lock	= iou_mutex_lock,
	.mutex_unlock	= iou_mutex_unlock,

	.request	= iou_submit_and_reap,
};

struct blockif_ctxt *
blockif_open(const char *optstr, const char *ident, int queue_num, struct iothreads_info *iothrds_info)
{
	char tag[MAXCOMLEN + 1];
	char *nopt, *xopts, *cp;
	struct blockif_ctxt *bc = NULL;
	struct stat sbuf;
	/* struct diocgattr_arg arg; */
	off_t size, psectsz, psectoff;
	int fd, i, j, sectsz;
	int writeback, ro, candiscard, ssopt, pssopt;
	long sz;
	long long b;
	int err_code = -1;
	off_t sub_file_start_lba, sub_file_size;
	int sub_file_assign;
	int max_discard_sectors, max_discard_seg, discard_sector_alignment;
	off_t probe_arg[] = {0, 0};
	int aio_mode;
	int bypass_host_cache, open_flag, bst_block;

	pthread_once(&blockif_once, blockif_init);

	fd = -1;
	ssopt = 0;
	pssopt = 0;
	ro = 0;
	sub_file_assign = 0;
	sub_file_start_lba = 0;
	sub_file_size = 0;

	max_discard_sectors = -1;
	max_discard_seg = -1;
	discard_sector_alignment = -1;

	/* default mode is thread pool */
	aio_mode = AIO_MODE_THREAD_POOL;

	/* writethru is on by default */
	writeback = 0;

	/* By default, do NOT bypass Service VM's page cache. */
	bypass_host_cache = 0;

	/* By default, bst_block is 1, meaning that the BST_BLOCK logic in blockif_dequeue is enabled. */
	bst_block = 1;

	candiscard = 0;

	if (queue_num <= 0)
		queue_num = 1;

	/*
	 * The first element in the optstring is always a pathname.
	 * Optional elements follow
	 */
	nopt = xopts = strdup(optstr);
	if (!nopt) {
		WPRINTF(("block_if.c: strdup retruns NULL\n"));
		return NULL;
	}
	while (xopts != NULL) {
		cp = strsep(&xopts, ",");
		if (cp == nopt)		/* file or device pathname */
			continue;
		else if (!strcmp(cp, "writeback"))
			writeback = 1;
		else if (!strcmp(cp, "writethru"))
			writeback = 0;
		else if (!strcmp(cp, "ro"))
			ro = 1;
		else if (!strcmp(cp, "nocache"))
			bypass_host_cache = 1;
		else if (!strcmp(cp, "no_bst_block"))
			bst_block = 0;
		else if (!strncmp(cp, "discard", strlen("discard"))) {
			strsep(&cp, "=");
			if (cp != NULL) {
				if (!(!dm_strtoi(cp, &cp, 10, &max_discard_sectors) &&
					*cp == ':' &&
					!dm_strtoi(cp + 1, &cp, 10, &max_discard_seg) &&
					*cp == ':' &&
					!dm_strtoi(cp + 1, &cp, 10, &discard_sector_alignment)))
					goto err;
			}
			candiscard = 1;
		} else if (!strncmp(cp, "sectorsize", strlen("sectorsize"))) {
			/*
			 *  sectorsize=<sector size>
			 * or
			 *  sectorsize=<sector size>/<physical sector size>
			 */
			if (strsep(&cp, "=") && !dm_strtoi(cp, &cp, 10, &ssopt)) {
				pssopt = ssopt;
				if (*cp == '/' &&
					dm_strtoi(cp + 1, &cp, 10, &pssopt) < 0)
					goto err;
			} else {
				goto err;
			}
		} else if (!strncmp(cp, "range", strlen("range"))) {
			/* range=<start lba>/<subfile size> */
			if (strsep(&cp, "=") &&
				!dm_strtol(cp, &cp, 10, &sub_file_start_lba) &&
				*cp == '/' &&
				!dm_strtol(cp + 1, &cp, 10, &sub_file_size))
				sub_file_assign = 1;
			else
				goto err;
		} else if (!strncmp(cp, "aio", strlen("aio"))) {
			/* aio=threads or aio=io_uring */
			strsep(&cp, "=");
			if (cp != NULL) {
				if (!strncmp(cp, "threads", strlen("threads"))) {
					aio_mode = AIO_MODE_THREAD_POOL;
				} else if (!strncmp(cp, "io_uring", strlen("io_uring"))) {
					aio_mode = AIO_MODE_IO_URING;
				} else {
					pr_err("Invalid aio option, only support threads or io_uring \"%s\"\n", cp);
					goto err;
				}
			}
		} else {
			pr_err("Invalid device option \"%s\"\n", cp);
			goto err;
		}
	}

	/*
	 * To support "writeback" and "writethru" mode switch during runtime,
	 * O_SYNC is not used directly, as O_SYNC flag cannot dynamic change
	 * after file is opened. Instead, we call fsync() after each write
	 * operation to emulate it.
	 */
	open_flag = (ro ? O_RDONLY : O_RDWR);
	if (bypass_host_cache == 1) {
		open_flag |= O_DIRECT;
	}
	fd = open(nopt, open_flag);

	if (fd < 0 && !ro) {
		/* Attempt a r/w fail with a r/o open */
		fd = open(nopt, O_RDONLY);
		ro = 1;
	}

	if (fd < 0) {
		pr_err("Could not open backing file: %s", nopt);
		goto err;
	}

	if (fstat(fd, &sbuf) < 0) {
		pr_err("Could not stat backing file %s", nopt);
		goto err;
	}

	/*
	 * Deal with raw devices
	 */
	size = sbuf.st_size;
	sectsz = DEV_BSIZE;
	psectsz = psectoff = 0;

	if (S_ISBLK(sbuf.st_mode)) {
		/* get size */
		err_code = ioctl(fd, BLKGETSIZE, &sz);
		if (err_code) {
			pr_err("error %d getting block size!\n",
				err_code);
			size = sbuf.st_size;	/* set default value */
		} else {
			size = sz * DEV_BSIZE;	/* DEV_BSIZE is 512 on Linux */
		}
		if (!err_code || err_code == EFBIG) {
			err_code = ioctl(fd, BLKGETSIZE64, &b);
			if (err_code || b == 0 || b == sz)
				size = b * DEV_BSIZE;
			else
				size = b;
		}
		DPRINTF(("block partition size is 0x%lx\n", size));

		/* get sector size, 512 on Linux */
		sectsz = DEV_BSIZE;
		DPRINTF(("block partition sector size is 0x%x\n", sectsz));

		/* get physical sector size */
		err_code = ioctl(fd, BLKPBSZGET, &psectsz);
		if (err_code) {
			pr_err("error %d getting physical sectsz!\n",
				err_code);
			psectsz = DEV_BSIZE;  /* set default physical size */
		}
		DPRINTF(("block partition physical sector size is 0x%lx\n",
			 psectsz));

		if (candiscard) {
			err_code = ioctl(fd, BLKDISCARD, probe_arg);
			if (err_code) {
				WPRINTF(("not support DISCARD\n"));
				candiscard = 0;
			}
		}

	} else {
		if (size < DEV_BSIZE || (size & (DEV_BSIZE - 1))) {
			WPRINTF(("%s size not corret, should be multiple of %d\n",
						nopt, DEV_BSIZE));
			goto err;
		}
		psectsz = sbuf.st_blksize;
	}

	if (ssopt != 0) {
		if (!powerof2(ssopt) || !powerof2(pssopt) || ssopt < 512 ||
		    ssopt > pssopt) {
			pr_err("Invalid sector size %d/%d\n",
			    ssopt, pssopt);
			goto err;
		}

		/*
		 * Some backend drivers (e.g. cd0, ada0) require that the I/O
		 * size be a multiple of the device's sector size.
		 *
		 * Validate that the emulated sector size complies with this
		 * requirement.
		 */
		if (S_ISCHR(sbuf.st_mode)) {
			if (ssopt < sectsz || (ssopt % sectsz) != 0) {
				pr_err("Sector size %d incompatible with underlying device sector size %d\n",
				    ssopt, sectsz);
				goto err;
			}
		}

		sectsz = ssopt;
		psectsz = pssopt;
		psectoff = 0;
	}

	bc = calloc(1, sizeof(struct blockif_ctxt));
	if (bc == NULL) {
		pr_err("calloc");
		goto err;
	}

	if (sub_file_assign) {
		DPRINTF(("sector size is %d\n", sectsz));
		bc->sub_file_assign = 1;
		bc->sub_file_start_lba = sub_file_start_lba * sectsz;
		size = sub_file_size * sectsz;
		DPRINTF(("Validating sub file...\n"));
		err_code = sub_file_validate(bc, fd, ro, bc->sub_file_start_lba,
					     size);
		if (err_code < 0) {
			pr_err("subfile range specified not valid!\n");
			exit(1);
		}
		DPRINTF(("Validated done!\n"));
	} else {
		/* normal case */
		bc->sub_file_assign = 0;
		bc->sub_file_start_lba = 0;
	}

	bc->fd = fd;
	bc->isblk = S_ISBLK(sbuf.st_mode);
	bc->candiscard = candiscard;
	if (candiscard) {
		bc->max_discard_sectors =
			(max_discard_sectors != -1) ?
				max_discard_sectors : (size / DEV_BSIZE);
		bc->max_discard_seg =
			(max_discard_seg != -1) ? max_discard_seg : 1;
		bc->discard_sector_alignment =
			(discard_sector_alignment != -1) ? discard_sector_alignment : 0;
	}
	bc->rdonly = ro;
	bc->size = size;
	bc->sectsz = sectsz;
	bc->psectsz = psectsz;
	bc->psectoff = psectoff;
	bc->wce = writeback;
	bc->bypass_host_cache = bypass_host_cache;
	bc->aio_mode = aio_mode;

	if (bc->aio_mode == AIO_MODE_IO_URING) {
		bc->ops = &blockif_ops_iou;
		bc->bst_block = 0;
	} else {
		bc->ops = &blockif_ops_thread_pool;
		bc->bst_block = bst_block;
	}

	bc->bq_num = queue_num;
	bc->bqs = calloc(bc->bq_num, sizeof(struct blockif_queue));
	if (bc->bqs == NULL) {
		pr_err("calloc bqs");
		goto err;
	}

	for (j = 0; j < bc->bq_num; j++) {
		struct blockif_queue *bq = bc->bqs + j;

		bq->bc = bc;

		if ((iothrds_info != NULL) && (iothrds_info->ioctx_base != NULL) && (iothrds_info->num != 0)) {
			bq->ioctx = iothrds_info->ioctx_base + j % iothrds_info->num;
		} else {
			bq->ioctx = NULL;
		}

		pthread_mutex_init(&bq->mtx, NULL);
		pthread_cond_init(&bq->cond, NULL);
		TAILQ_INIT(&bq->freeq);
		TAILQ_INIT(&bq->pendq);
		TAILQ_INIT(&bq->busyq);
		for (i = 0; i < BLOCKIF_MAXREQ; i++) {
			bq->reqs[i].status = BST_FREE;
			TAILQ_INSERT_HEAD(&bq->freeq, &bq->reqs[i], link);
		}

		if (snprintf(tag, sizeof(tag), "blk-%s-%d",
					ident, j) >= sizeof(tag)) {
			pr_err("blk thread tag too long");
		}

		if (bc->ops->init) {
			if (bc->ops->init(bq, tag) < 0) {
				goto err;
			}
		}
	}

	/* free strdup memory */
	if (nopt) {
		free(nopt);
		nopt = NULL;
	}

	return bc;
err:
	/* handle failure case: free strdup memory*/
	if (nopt)
		free(nopt);
	if (fd >= 0)
		close(fd);
	if (bc) {
		if (bc->bqs)
			free(bc->bqs);
		free(bc);
	}
	return NULL;
}

static int
blockif_request(struct blockif_ctxt *bc, struct blockif_req *breq,
		enum blockop op)
{
	struct blockif_queue *bq;
	int err;

	err = 0;

	if (breq->qidx >= bc->bq_num) {
		pr_err("%s: invalid qidx %d\n", __func__, breq->qidx);
		return ENOENT;
	}
	bq = bc->bqs + breq->qidx;

	blockif_init_alignment_info(bc, breq);
	/* For misaligned READ/WRITE, need a bounce_iov to convert the misaligned request to an aligned one. */
	if (((op == BOP_READ) || (op == BOP_WRITE)) && (breq->align_info.need_conversion)) {
		err = blockif_init_bounce_iov(breq);
		if (err < 0) {
			return err;
		}

		if (op == BOP_WRITE) {
			err = blockif_init_bounced_write(bc, breq);
			if (err < 0) {
				return err;
			}
		}
	}

	if (bc->ops->mutex_lock) {
		bc->ops->mutex_lock(&bq->mtx);
	}
	if (!TAILQ_EMPTY(&bq->freeq)) {
		/*
		 * Enqueue and inform the block i/o thread
		 * that there is work available
		 */
		if (blockif_enqueue(bq, breq, op)) {
			if (bc->ops->request) {
				bc->ops->request(bq);
			}
		}
	} else {
		/*
		 * Callers are not allowed to enqueue more than
		 * the specified blockif queue limit. Return an
		 * error to indicate that the queue length has been
		 * exceeded.
		 */
		err = E2BIG;
	}
	if (bc->ops->mutex_unlock) {
		bc->ops->mutex_unlock(&bq->mtx);
	}
	return err;
}

int
blockif_read(struct blockif_ctxt *bc, struct blockif_req *breq)
{
	return blockif_request(bc, breq, BOP_READ);
}

int
blockif_write(struct blockif_ctxt *bc, struct blockif_req *breq)
{
	return blockif_request(bc, breq, BOP_WRITE);
}

int
blockif_flush(struct blockif_ctxt *bc, struct blockif_req *breq)
{
	return blockif_request(bc, breq, BOP_FLUSH);
}

int
blockif_discard(struct blockif_ctxt *bc, struct blockif_req *breq)
{
	return blockif_request(bc, breq, BOP_DISCARD);
}

int
blockif_cancel(struct blockif_ctxt *bc, struct blockif_req *breq)
{
	struct blockif_elem *be;
	struct blockif_queue *bq;

	if (breq->qidx >= bc->bq_num) {
		pr_err("%s: invalid qidx %d\n", __func__, breq->qidx);
		return ENOENT;
	}
	bq = bc->bqs + breq->qidx;

	pthread_mutex_lock(&bq->mtx);
	/*
	 * Check pending requests.
	 */
	TAILQ_FOREACH(be, &bq->pendq, link) {
		if (be->req == breq)
			break;
	}
	if (be != NULL) {
		/*
		 * Found it.
		 */
		blockif_complete(bq, be);
		pthread_mutex_unlock(&bq->mtx);

		return 0;
	}

	/*
	 * Check in-flight requests.
	 */
	TAILQ_FOREACH(be, &bq->busyq, link) {
		if (be->req == breq)
			break;
	}
	if (be == NULL) {
		/*
		 * Didn't find it.
		 */
		pthread_mutex_unlock(&bq->mtx);
		return -1;
	}

	/*
	 * Interrupt the processing thread to force it return
	 * prematurely via it's normal callback path.
	 */
	while (be->status == BST_BUSY) {
		struct blockif_sig_elem bse, *old_head;

		pthread_mutex_init(&bse.mtx, NULL);
		pthread_cond_init(&bse.cond, NULL);

		bse.pending = 1;

		do {
			old_head = blockif_bse_head;
			bse.next = old_head;
		} while (!__sync_bool_compare_and_swap((uintptr_t *)&
							blockif_bse_head,
					    (uintptr_t)old_head,
					    (uintptr_t)&bse));

		pthread_kill(be->tid, SIGCONT);

		pthread_mutex_lock(&bse.mtx);
		while (bse.pending)
			pthread_cond_wait(&bse.cond, &bse.mtx);
		pthread_mutex_unlock(&bse.mtx);
	}

	pthread_mutex_unlock(&bq->mtx);

	/*
	 * The processing thread has been interrupted.  Since it's not
	 * clear if the callback has been invoked yet, return EBUSY.
	 */
	return -EBUSY;
}

int
blockif_close(struct blockif_ctxt *bc)
{
	int j;

	sub_file_unlock(bc);

	/*
	 * Stop the block i/o thread
	 */
	for (j = 0; j < bc->bq_num; j++) {
		struct blockif_queue *bq = bc->bqs + j;

		pthread_mutex_lock(&bq->mtx);
		bq->closing = 1;
		pthread_cond_broadcast(&bq->cond);
		pthread_mutex_unlock(&bq->mtx);

		if (bc->ops->deinit) {
			bc->ops->deinit(bq);
		}
	}
	/* XXX Cancel queued i/o's ??? */

	/*
	 * Release resources
	 */
	close(bc->fd);
	if (bc->bqs)
		free(bc->bqs);
	free(bc);

	return 0;
}

/*
 * Return virtual C/H/S values for a given block. Use the algorithm
 * outlined in the VHD specification to calculate values.
 */
void
blockif_chs(struct blockif_ctxt *bc, uint16_t *c, uint8_t *h, uint8_t *s)
{
	off_t sectors;		/* total sectors of the block dev */
	off_t hcyl;		/* cylinders times heads */
	uint16_t secpt;		/* sectors per track */
	uint8_t heads;

	sectors = bc->size / bc->sectsz;

	/* Clamp the size to the largest possible with CHS */
	if (sectors > 65535UL*16*255)
		sectors = 65535UL*16*255;

	if (sectors >= 65536UL*16*63) {
		secpt = 255;
		heads = 16;
		hcyl = sectors / secpt;
	} else {
		secpt = 17;
		hcyl = sectors / secpt;
		heads = (hcyl + 1023) / 1024;

		if (heads < 4)
			heads = 4;

		if (hcyl >= (heads * 1024) || heads > 16) {
			secpt = 31;
			heads = 16;
			hcyl = sectors / secpt;
		}
		if (hcyl >= (heads * 1024)) {
			secpt = 63;
			heads = 16;
			hcyl = sectors / secpt;
		}
	}

	*c = hcyl / heads;
	*h = heads;
	*s = secpt;
}

/*
 * Accessors
 */
off_t
blockif_size(struct blockif_ctxt *bc)
{
	return bc->size;
}

int
blockif_sectsz(struct blockif_ctxt *bc)
{
	return bc->sectsz;
}

void
blockif_psectsz(struct blockif_ctxt *bc, int *size, int *off)
{
	*size = bc->psectsz;
	*off = bc->psectoff;
}

int
blockif_queuesz(struct blockif_ctxt *bc)
{
	return (BLOCKIF_MAXREQ - 1);
}

int
blockif_is_ro(struct blockif_ctxt *bc)
{
	return bc->rdonly;
}

int
blockif_candiscard(struct blockif_ctxt *bc)
{
	return bc->candiscard;
}

int
blockif_max_discard_sectors(struct blockif_ctxt *bc)
{
	return bc->max_discard_sectors;
}

int
blockif_max_discard_seg(struct blockif_ctxt *bc)
{
	return bc->max_discard_seg;
}

int
blockif_discard_sector_alignment(struct blockif_ctxt *bc)
{
	return bc->discard_sector_alignment;
}

uint8_t
blockif_get_wce(struct blockif_ctxt *bc)
{
	return bc->wce;
}

void
blockif_set_wce(struct blockif_ctxt *bc, uint8_t wce)
{
	bc->wce = wce;
}

int
blockif_flush_all(struct blockif_ctxt *bc)
{
	int err;

	err=0;
	if (fsync(bc->fd))
		err = errno;
	return err;
}
