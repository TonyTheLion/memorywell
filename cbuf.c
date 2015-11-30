#include "cbuf_int.h"

#ifdef Z_BLK_LVL
#undef Z_BLK_LVL
#endif
#define Z_BLK_LVL 1

cbuf_t *cbuf_create(uint32_t obj_sz, uint32_t obj_cnt)
{
	return cbuf_create_(obj_sz, obj_cnt, 0x0);
}
cbuf_t *cbuf_create_p(uint32_t obj_sz, uint32_t obj_cnt, char *backing_store)
{
	cbuf_t *ret = NULL;
	Z_die_if(!backing_store, "please provide a path for the backing store");

	/* create cbuf */
	ret = cbuf_create_(sizeof(cbufp_t), obj_cnt, CBUF_P);
	Z_die_if(!ret, "cbuf create failed");
	/* cbuf_create_() will have padded the obj size and obj count to 
		fit  into powers of 2.
	The backing store MUST have sufficient space for EACH cbufp_t in cbuf 
		to  point to a unique area of `obj_sz` length.
		*/
	obj_cnt = cbuf_obj_cnt(ret);	

	/* make accounting structure */
	cbufp_t f;	
	memset(&f, 0x0, sizeof(f));
	/* string masturbation */
	size_t len = strlen(backing_store) + 1;
	Z_die_if(!(f.file_path = malloc(len)), "");
	memcpy(f.file_path, backing_store, len);

	/* Map backing store.
		Typecasts because insidious overflow.
		*/
	f.iov.iov_len = ((uint64_t)obj_sz * (uint64_t)obj_cnt);
	Z_die_if(!(f.fd = sbfu_dst_map(&f.iov, f.file_path)), "");
	f.blk_iov.iov_len = obj_sz;
	f.blk_iov.iov_base = f.iov.iov_base;

	/* populate tracking structures 
	Go through the motions of reserving cbuf blocks rather than
		accessing ret->buf directly.
	This is because cbuf will likely fudge the block size on creation, 
		and we don't want to care about that.
		*/
	uint32_t pos = cbuf_snd_res_m(ret, obj_cnt);
	Z_die_if(pos == -1, "");
	cbufp_t *p;
	for (f.blk_id=0; f.blk_id < obj_cnt; 
		f.blk_id++, f.blk_iov.iov_base += obj_sz) 
	{
		p = cbuf_offt(ret, pos, f.blk_id);
		memcpy(p, &f, sizeof(f));
		p->blk_offset = f.blk_iov.iov_base - f.iov.iov_base;
	};
	cbuf_snd_rls_m(ret, obj_cnt);
	/* 'receive' so all blocks are marked as unused */
	pos = cbuf_rcv_res_m(ret, obj_cnt);
	Z_die_if(pos == -1, "");
	cbuf_rcv_rls_m(ret, obj_cnt);

	return ret;
out:
	if (f.file_path)
		free(f.file_path);
	cbuf_free_(ret);
	return NULL;
}

void cbuf_free(cbuf_t *buf)
{
	cbuf_free_(buf);
}

/*	cbuf_(snd|rcv)_reserve()
Obtain exclusive title to an obj_sz chunk of memory in the circular buffer.
This memory is no longer available for either sending or receiving
	... until the corresponsing "release" is called.
*/
void *cbuf_snd_res(cbuf_t *buf)
{
	/* pos is already masked to cycle back to 0 */
	uint32_t pos = cbuf_reserve__(buf, 1 << buf->sz_bitshift_, 
					&buf->sz_unused, 
					&buf->snd_reserved, 
					&buf->snd_pos);
	if (pos == -1)
		return NULL;
	else
		return buf->buf + pos; /* does masking */
}

/*	cbuf_(snd|srv)_serve_multi()
Obtain multiple contiguous (obj_sz * cnt) chunks of memory.
Returns the POSITION of the reservation, not a memory address.
`pos` can then be fed to cbuf_offt() at each access loop iteration to get a 
	proper memory address (and cleanly loop through the end of the buffer).
`pos` as returned MAY be an overflow - use it only as a token to be passed
	to cbuf_offt().
Returns -1 on fail.
	*/
uint32_t cbuf_snd_res_m(cbuf_t *buf, size_t cnt)
{
	/* sanity */
	if (!cnt)
		return -1;
	/* attempt a reservation, get position */
	//return cbuf_reserve__(buf, buf->obj_sz * cnt, 
	return cbuf_reserve__(buf, cnt << buf->sz_bitshift_, 
					&buf->sz_unused, 
					&buf->snd_reserved, 
					&buf->snd_pos);
}

/*	cbuf_snd_res_m_cap()
Tries to reserve a variable number of buffer slots up to *res_cnt.
If successful, returns `pos` and *res_cnt is set to the
	amount of slots reserved.
May fail, which returns -1 and leaves the value of *res_cnt
	as the reservation size attempted.
	*/
uint32_t cbuf_snd_res_m_cap(cbuf_t *buf, size_t *res_cnt)
{
	/* bitshift sz_ready so it gives an OBJECT COUNT
		as opposed to a count of BYTES
		*/
	size_t possible = buf->sz_unused >> buf->sz_bitshift_;
	/* what CAN we ask for? */
	if (*res_cnt > possible)
		*res_cnt = possible; /* if it's less than ideal, reflect that */
	/* say please */
	return cbuf_snd_res_m(buf, *res_cnt);
}

/*	The _rcv_ family of functions is symmetric to the _snd_ ones
		above. Look there for detailed comments.
	*/
void *cbuf_rcv_res(cbuf_t *buf)
{
	/* attempt a reservation, get position */
	uint32_t pos = cbuf_reserve__(buf, 1 << buf->sz_bitshift_, 
					&buf->sz_ready, 
					&buf->rcv_reserved, 
					&buf->rcv_pos);
	/* turn pos into an address */
	if (pos == -1)
		return NULL;
	else
		return buf->buf + pos;
}

uint32_t cbuf_rcv_res_m(cbuf_t *buf, size_t cnt)
{
	/* not asking for anything? bye. */
	if (!cnt)
		return -1;
	/* attempt a reservation, get position */
	//return cbuf_reserve__(buf, buf->obj_sz * cnt, 
	return cbuf_reserve__(buf, cnt << buf->sz_bitshift_, 
					&buf->sz_ready, 
					&buf->rcv_reserved, 
					&buf->rcv_pos);
}

uint32_t cbuf_rcv_res_m_cap(cbuf_t *buf, size_t *res_cnt)
{
	size_t possible = buf->sz_ready >> buf->sz_bitshift_;
	if (*res_cnt > possible)
		*res_cnt = possible;
	return cbuf_rcv_res_m(buf, *res_cnt);
}

void cbuf_snd_rls(cbuf_t *buf)
{
	cbuf_release__(buf, 1 << buf->sz_bitshift_, 
			&buf->snd_reserved, 
			&buf->snd_uncommit,
			&buf->sz_ready);
}

void cbuf_snd_rls_m(cbuf_t *buf, size_t cnt)
{
	if (!cnt)
		return;
	//cbuf_release__(buf, buf->obj_sz * cnt, 
	cbuf_release__(buf, cnt << buf->sz_bitshift_, 
			&buf->snd_reserved, 
			&buf->snd_uncommit,
			&buf->sz_ready);
}

void cbuf_rcv_rls(cbuf_t *buf)
{
	cbuf_release__(buf, 1 << buf->sz_bitshift_, 
			&buf->rcv_reserved, 
			&buf->rcv_uncommit,
			&buf->sz_unused);
}

void cbuf_rcv_rls_m(cbuf_t *buf, size_t cnt)
{
	if (!cnt)
		return;
	cbuf_release__(buf, cnt << buf->sz_bitshift_, 
			&buf->rcv_reserved, 
			&buf->rcv_uncommit,
			&buf->sz_unused);
}

void cbuf_rcv_rls_mscary(cbuf_t *buf, size_t cnt)
{
	if (!cnt)
		return;
	return cbuf_release_scary__(buf, cnt << buf->sz_bitshift_, 
			&buf->rcv_reserved, 
			&buf->rcv_uncommit,
			&buf->sz_unused);
}

/*	cbuf_checkpoint_snapshot()

Take a "snapshot" of the current sender position IN RELATION TO the "actual receiver".
Note that this checkpoint concept relies on NOT overflowing, or "unrolling" the count.
uint64_t for the edge case where size of cbuf (overflow_ +1) == UINT32_MAX.

some interesting equations:
	snd_pos = "actual sender" + (snd_reserved + snd_uncommitted);
		"actual sender" = rcv_pos + ready;
	rcv_pos = "actual receiver" + (rcv_reserved + rcv_uncommitted);
		"actual receiver" = snd_pos + unused;

The concept is that once "actual receiver" moves PAST the "snapshot" of
	a previous sender position, all bytes up to that snapshot
	will have been "consumed" (aka: reserved AND released by receiver).

RETURNS: a uint64_t which can be fed to `cbuf_checkpoint_verif()` later.
	*/
uint64_t	cbuf_checkpoint_snapshot(cbuf_t *b)
{
	/*  save values at a single point in time */
	uint64_t ret = b->snd_pos;
	/* If sender is smaller than "actual receiver", it actually rolled over.
		Add a full buffer size to it */
	if (ret < ((ret + (uint64_t)b->sz_unused) & b->overflow_))
		ret += b->overflow_ +1;
	return ret;
}

/*	cbuf_checkpoint_verif()

Verifies state of the current cbuf at `b` against a checkpoint previously
	produced by `cbuf_checkpoint_snapshot()`.

RETURNS 1 if all data through `snd_pos` at the time snapshot was taken 
	has been consumed by receiver.
	*/
int cbuf_checkpoint_verif(cbuf_t *b, uint64_t checkpoint)
{
	/* "actual receiver" is the concept of 
		"what does the receiver NO LONGER CARE ABOUT."
	This is also: `rcv_pos - (rcv_reserved + rcv_uncommit)`
		... but avoiding subtraction which is messy to roll over "before 0".
		*/
	uint64_t actual_rcv = (b->snd_pos + b->sz_unused); /* notice we don't mask to overflow */
	/* if this rolled over, add a full buffer size to it */
	return actual_rcv >= checkpoint;
}

/*	cbuf_splice_from_pipe()
Pulls at most `size` bytes from `a_pipe[0]` into the cbuf block at `pos`, offset `i`.

In the case of a regular cbuf:
	The amount of bytes actually pushed is written in the first 8B of 
		the cbuf block itself, aka `cbuf_head`.
	`cbuf_head` may be 0 but will be AT MOST the size of cbuf block (minus 8B).
	Returns `cbuf_head` - which on error will be 0, NOT -1(!).

In the case that cbuf has a backing store (the block only contains a cbufp_t):
	The amount of bytes to push is limited to 'blk_iov.iov_len'.
	The amount of bytes actually pushed is stored in the 'data_len' variable.
	data_len is returned but is never less than 0.
	*/
size_t	cbuf_splice_from_pipe(int fd_pipe_read, cbuf_t *b, uint32_t pos, int i, size_t size)
{
	if (b->cbuf_flags & CBUF_P) {
		cbufp_t *f = cbuf_offt(b, pos, i);
		/* sanity */
		if (size > f->blk_iov.iov_len){
			Z_err("size %d larger than iov_len %d", (int)size, (int)f->blk_iov.iov_len);
			size = f->blk_iov.iov_len;
		}
		
		loff_t temp_offset = f->blk_offset;
		do {
			f->data_len = splice(fd_pipe_read, NULL, 
				f->fd, &temp_offset, size, SPLICE_F_NONBLOCK);
			if (f->data_len == 0){
				Z_err("cbuf_splice_from_pipe: failed to splice with: cbuf %lx, fd_pipe_read %d, pos %d, i %d, f %lx, f->fd %d, f->blk_offset %d, size %d, errno %d",
				      (uint64_t)b, (int)fd_pipe_read, (int) pos, (int) i, (uint64_t)f, (int)f->fd, (int)f->blk_offset, (int)size, (int)errno);
			}
			/* ENOMEM seems about the only "should retry this" error.
			Everything else, we should just return "no bytes copied".
			While we're evaluating data_len, set it to 0 to avoid another
				branch operation immediately following.
				*/
		} while (f->data_len == -1 && (!(f->data_len = 0)) && errno == ENOMEM);

		return f->data_len;
	} else {
		/* sanity */
		if (size > (cbuf_sz_obj(b) - sizeof(ssize_t)))
			size = cbuf_sz_obj(b) - sizeof(ssize_t);

		/* get head of buffer block, put offset from buf fd info cbuf_off */
		ssize_t *cbuf_head;
		loff_t cbuf_off = cbuf_lofft(b, pos, i, &cbuf_head);
		/* Pull data from pipe fitting,  put into buffer block,
			put transfer length at head of buffer block.
			*/	
		*cbuf_head = splice(fd_pipe_read, NULL, b->mmap_fd, 
			&cbuf_off, size, SPLICE_F_NONBLOCK);
		/* if got error, tell receiver "nothing here" */
		if (*cbuf_head == -1) {
			Z_err("size %ld", size);
			*cbuf_head = 0;
		}
		return *cbuf_head;
	}
}

/*	cbuf_splice_to_pipe()
Reads `cbuf_head` (see `cbuf_splice_from_pipe()` above) @(pos +i).
Splices `*cbuf_head` byted from the cbuf into `fd_pipe_write`.
`fd_pipe_write` MUST be a pipe, not a file or mmap'ed region.
If there is an error will return 0, not -1.
	*/
size_t	cbuf_splice_to_pipe(cbuf_t *b, uint32_t pos, int i, int fd_pipe_write)
{
	if (b->cbuf_flags & CBUF_P) {
		cbufp_t *f = cbuf_offt(b, pos, i);

		/* do data, no copy */
		if (!f->data_len)
			return 0;	

		/* Pull chunk from buffer. */
		loff_t temp_offset = f->blk_offset;
		ssize_t temp = splice(f->fd, &temp_offset, 
				fd_pipe_write, NULL, f->data_len, SPLICE_F_NONBLOCK);
		if (temp == -1)
			temp = 0;
		return temp;
	}else {
		ssize_t *cbuf_head;
		loff_t cbuf_off = cbuf_lofft(b, pos, i, &cbuf_head);

		/* ALWAYS check for an opportunity to slack ;) */
		if (!cbuf_head[0])
			return 0;

		/* Pull chunk from buffer.
			Could return -1 if dest. pipe is full.
			Have pipe empty before running this, then evacuate pipe.
			*/
		ssize_t temp = splice(b->mmap_fd, &cbuf_off, 
				fd_pipe_write, NULL, *cbuf_head, SPLICE_F_NONBLOCK);
		if (temp == -1)
			temp = 0;
		return temp;
	}
}

#undef Z_BLK_LVL
#define Z_BLK_LVL 0
