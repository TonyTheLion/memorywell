#ifndef cbuf_h_
#define cbuf_h_

/* cbuf - thread-safe circular buffer 

	Built to cope with:
		- arbitrary object sizes
		- multiple senders, multiple receivers

	The philosophy is that a cbuf has a send side and a receive side.
	One or more threads on each 'side' can reserve one or more 'blocks'. 
	Reserved blocks are exclusively held until they are released.
	Once released, they available for the 'other side' to reserve and utilize.

	The only caveat is the case where multiple threads all have blocks
		reserved on one side (either senders or receivers).
	In this case, blocks released by one thread become 'uncommitted'
		until ALL threads have released their reserved blocks, 
		at which point the uncommitted blocks become 
		available for reservation by the opposite side.
	This is because there is no way to force that blocks will be released
		in the same order they were reserved.
	The only exception to this is the "release_scary(n)" call, which
		simply releases the first n reserved blocks.
	Use "scary" ONLY if the thread releasing can guarantee it is
		the only thread reading from that side of the buffer.
		

	The block state-counts and `positions` are: (`pos` variables move DOWN)

		- rcv_reserved | [rcv_uncommitted]
	<< rcv_pos >>
		- ready (available for receiver)
		- snd_reserved | [snd_uncommitted]
	<< snd_pos >>
		- unused (available for sender)

	... at creation time, all blocks are `unused`.


cbufp_ == CBUF of Pointers

	cbuf's have the following limitations:
	a.) They are limited to UINT32_MAX in size
	b.) Caller has no control over where cbuf mmap()s it's backing store
	c.) cbuf's backing store is memory-locked (kernel not allowed to page).

	To overcome these without compromising the design of cbuf itself,
		there is cbufp_.

	Essentially, a file is mmap()'ed at a path of the caller's choosing.
		Let's call this file the "backing store".
	Then a separate cbuf is created, it's blocks containing only
		accounting data and pointers into the backing store.

	The splice() operations on the cbuf see a flag in cbuf_t and do I/O
		from the backing store instead of the cbuf_t's memory.

	WARNING:
	cbufp_ ONLY WORKS with the splice() calls. 
	Writing into any of the blocks in the cbuf erases the necessary 
		accounting structs and likely overflows into neighboring blocks.


TERMINOLOGY

	The terms "block" "obj" and "object" are used interchangeably to
		refer to an area of memory which can be "reserved" or "released"
		in a circular buffer.
	The connotation is that cbuf doesn't care what's in that memory,
		simply what it's size is and wheather it's in any one of
		the following states:

		- unused (ready for writing).
		- reserved by sender for writing.
		- ready (ready for reading).
		- reserved by receiver fir reading.
	
*/

#ifndef _GNU_SOURCE
	#define _GNU_SOURCE
	/* mkostemp, splice */
#endif
#include <fcntl.h>
#include <stdint.h> /* [u]int[blah] */
#include <stdlib.h> /* mkostemp */
#include <sys/mman.h> /* mmap */
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
 
#include "sbfu.h" /* for backing store operations: cbufp_ only */
#include "zed_dbg.h"

/* effectively a `pthread_yield()` but without having to include threading libraries */
#define CBUF_YIELD() usleep(100)

#define CBUF_P		0x01	/* this cbuf contains pointers to the data,
					not the data itself.
					*/

typedef struct {
	void		*buf;		/* Contiguous block of memory over which
						the circular buffer iterates.
						*/
	uint8_t		sz_bitshift_;	/* Used to quickly multiply or divide by obj_sz.
					... or derive obj_sz by bitshifting `1`.
					NOTE that all sizes and positions in this 
						struct are in BYTES, not 'blocks' or 'cells'.
					This is OK as a uint8_t: it will never be
						more than 32.
						*/
	uint8_t		cbuf_flags;
	uint16_t	mmap_fd;	/* This SHOULD be 32bits, but I ran out
						of space on a 64B cacheline :(
					All structures manipulating mmmap'ed file 
						need to work on their own internal
						signed integer fd and do
						error checking on that one.
						*/
	uint32_t	overflow_;	/* Used for quick masking of `pos` variables. 
					It's also `buf_sz -1`, and is used 
						in lieu of a dedicated `buf_sz` 
						variable to keep struct size <=64B.
					       */

	/* byte offsets into buffer - MUST be masked: use only with cbuf_offt() */
	uint32_t	snd_pos; /* ... unsigned so they can roll over harmlessly. */
	uint32_t	rcv_pos;

	/* some sizes are SIGNED: atomic subtractions may push them negative */
	int64_t		sz_unused;	/* Can be written. */
	int64_t		sz_ready;	/* Ready for reading. */

	uint32_t	snd_reserved;	/* Reserved by writer(s). */
	uint32_t	snd_uncommit;	/* Not committed because other writers I/P. */
	uint32_t	rcv_reserved;	/* Reserved by readers(s). */
	uint32_t	rcv_uncommit;	/* Not committed because other readers I/P. */

	int		plumbing[2];	/* Pipe for splice operations.
					NOTE that access to plumbing is NOT safe
					in the context of multiple senders|receivers.
						*/
}__attribute__ ((packed))	cbuf_t;

typedef struct {
	/* backing store variables: identical for all cbufp_t instances */
	int		fd;
	struct iovec	iov;
	char		*file_path;
	/* block-specific variables: different from block to block */
	uint64_t	blk_id;
	struct iovec	blk_iov;
	loff_t		blk_offset;
	size_t		data_len;
} cbufp_t;

/* compute some basic values out of a cbuf struct */
Z_INL_FORCE uint32_t cbuf_sz_buf(cbuf_t *b) { return b->overflow_ + 1; }
Z_INL_FORCE uint32_t cbuf_sz_obj(cbuf_t *b) { return 1 << b->sz_bitshift_; }
Z_INL_FORCE uint64_t cbuf_sz_p(cbuf_t *b) 
{ 
	if (!(b->cbuf_flags & CBUF_P)) 
		return 0;
	return ((cbufp_t *)b->buf)->iov.iov_len;
}
Z_INL_FORCE uint32_t cbuf_obj_cnt(cbuf_t *b) { return cbuf_sz_buf(b) >> b->sz_bitshift_; }

/* create/free */
cbuf_t *cbuf_create(uint32_t obj_sz, uint32_t obj_cnt);
cbuf_t *cbuf_create_p(uint32_t obj_sz, uint32_t obj_cnt, char *backing_store);
void	cbuf_free(cbuf_t *buf);

/* reserve */
void	*cbuf_snd_res(cbuf_t *buf);
uint32_t cbuf_snd_res_m(cbuf_t *buf, size_t cnt);
uint32_t cbuf_snd_res_m_cap(cbuf_t *buf, size_t *res_cnt); 
void	*cbuf_rcv_res(cbuf_t *buf);
uint32_t cbuf_rcv_res_m(cbuf_t *buf, size_t cnt);
uint32_t cbuf_rcv_res_m_cap(cbuf_t *buf, size_t *res_cnt);

/* release */
void cbuf_snd_rls(cbuf_t *buf);
void cbuf_snd_rls_m(cbuf_t *buf, size_t cnt);
void cbuf_rcv_rls(cbuf_t *buf);
void cbuf_rcv_rls_m(cbuf_t *buf, size_t cnt);

/* sophisticated buffer tricks */
void		cbuf_rcv_rls_mscary(cbuf_t *buf, size_t cnt);
uint32_t	cbuf_rcv_held(cbuf_t *buf, size_t *out_cnt);

/* checkpoint */
uint64_t	cbuf_checkpoint_snapshot(cbuf_t *b);
int		cbuf_checkpoint_verif(cbuf_t *b, uint64_t checkpoint);

/* splice */
size_t	cbuf_splice_sz(cbuf_t *b, uint32_t pos, int i);
size_t	cbuf_splice_from_pipe(int fd_pipe_read, cbuf_t *b, uint32_t pos, int i, size_t size);
size_t	cbuf_splice_to_pipe(cbuf_t *b, uint32_t pos, int i, int fd_pipe_write);

/*	cbuf_offt()
Deliver the memory address at the beginning of the nth in a 
	contiguous set of buffer blocks which starts at `pos`.
The contiguous set of buffer blocks may exist partly at the end of the
	buffer memory block, and the rest of the way starting at the beginning.
This function exists to hide the masking necessary to roll over from the end to
	the beginning of the buffer.
	*/
Z_INL_FORCE void *cbuf_offt(cbuf_t *buf, uint32_t start_pos, uint32_t n)
{
	start_pos += n << buf->sz_bitshift_; /* purrformance */
	return buf->buf + (start_pos & buf->overflow_);
}

/*	cbuf_lofft()
Does an offset calculation as in cbuf_offt() above.
`*head` is set to the start of the memory region, where we store a "length" variable.
We return an offset value from the base address of `buf->buf`, pointing to the 
	space immediately following `*head`. 
Returned offset value is suitable for use with splice() memcpy(), etc.
	*/
Z_INL_FORCE loff_t cbuf_lofft(cbuf_t *buf, uint32_t start_pos, uint32_t n, size_t **head)
{
	start_pos += n << buf->sz_bitshift_;
	loff_t ret = (start_pos & buf->overflow_);
	*head = buf->buf + ret;
	return ret + sizeof(ssize_t);
}

#endif /* cbuf_h_ */