#include <nbuf.h>

#include <zed_dbg.h>
#include <stdlib.h>
#include <pthread.h>
#include <getopt.h>
#include <nonlibc.h> /* timing */

/* TODO:
	- yield to a specific thread (eventing) instead of general yield()?
	- validate reserve and release with '0' size.
*/


static size_t numiter = 100000000;
static size_t blk_cnt = 256; /* how many blocks in the cbuf */
static size_t blk_size = 8; /* in Bytes */

static size_t tx_thread_cnt = 1;
static pthread_t *tx = NULL;
static size_t rx_thread_cnt = 1;
static pthread_t *rx = NULL;

static size_t reservation = 1; /* how many blocks to reserve at once */


/*	tx_thread()
*/
void *tx_single(void* arg)
{
	struct nbuf *nb = arg;
	size_t tally = 0;

	/* loop on TX */
	for (size_t i=0, res=0; i < numiter; i += res) {
		size_t pos;
		while (!(res = nbuf_reserve(&nb->ct, &nb->tx, &pos, reservation)))
			sched_yield(); /* no scheduling decisions taken by nbuf */
		for (size_t j=0; j < res; j++)
			tally += NBUF_DEREF(size_t, pos, j, nb) = i + j;
		nbuf_release_single(&nb->ct, &nb->rx, res);
	}
	return (void *)tally;
}
void *tx_multi(void* arg)
{
	struct nbuf *nb = arg;
	size_t tally = 0;
	size_t num = numiter / tx_thread_cnt;

	/* loop on TX */
	for (size_t i=0, res=0; i < num; i += res) {
		size_t pos;
		while (!(res = nbuf_reserve(&nb->ct, &nb->tx, &pos, reservation)))
			sched_yield(); /* no scheduling decisions taken by nbuf */
		for (size_t j=0; j < res; j++)
			tally += NBUF_DEREF(size_t, pos, j, nb) = i + j;
		nbuf_release_multi(&nb->ct, &nb->rx, res, pos);
	}
	return (void *)tally;
}


/*	rx_thread()
*/
void *rx_single(void* arg)
{
	struct nbuf *nb = arg;
	size_t tally = 0;

	/* loop on RX */
	for (size_t i=0, res=0; i < numiter; i += res) {
		size_t pos;
		while (!(res = nbuf_reserve(&nb->ct, &nb->rx, &pos, reservation)))
			sched_yield(); /* no scheduling decisions taken by nbuf */
		for (size_t j=0; j < res; j++) {
			size_t temp = NBUF_DEREF(size_t, pos, j, nb);
			tally += temp;
		}
		nbuf_release_single(&nb->ct, &nb->tx, res);
	}
	return (void *)tally;
}
void *rx_multi(void* arg)
{
	struct nbuf *nb = arg;
	size_t tally = 0;
	size_t num = numiter / rx_thread_cnt;

	/* loop on RX */
	for (size_t i=0, res=0; i < num; i += res) {
		size_t pos;
		while (!(res = nbuf_reserve(&nb->ct, &nb->rx, &pos, reservation)))
			sched_yield(); /* no scheduling decisions taken by nbuf */
		for (size_t j=0; j < res; j++) {
			size_t temp = NBUF_DEREF(size_t, pos, j, nb);
			tally += temp;
		}
		nbuf_release_multi(&nb->ct, &nb->tx, res, pos);
	}
	return (void *)tally;
}


/*	usage()
*/
void usage(const char *pgm_name)
{
	fprintf(stderr, "Usage: %s [OPTIONS]\n\
Test Single-Producer|Single-Consumer correctness/performance.\n\
\n\
Options:\n\
-n, --numiter <iter>	:	Push <iter> blocks through the buffer.\n\
-c, --count <blk_count>	:	How many blocks in the circular buffer.\n\
-s, --size <blk_size>	:	Size of each block (bytes).\n\
-r, --reservation <res>	:	(Attempt to) reserve <res> blocks at once.\n\
-t, --tx-threads	:	Number of TX threads.\n\
-x, --rx-threads	:	Number of RX threads.\n\
-h, --help		:	Print this message and exit.\n",
		pgm_name);
}


/*	main()
*/
int main(int argc, char **argv)
{
	/* Use global error count so that threads can error
		and directly affect the return code of main()
	int err_cnt = 0;
	*/


	/*
		options
	*/
	int opt = 0;
	static struct option long_options[] = {
		{ "numiter",	required_argument,	0,	'n'},
		{ "size",	required_argument,	0,	's'},
		{ "count",	required_argument,	0,	'c'},
		{ "reservation",required_argument,	0,	'r'},
		{ "tx-threads",	required_argument,	0,	't'},
		{ "rx-threads",	required_argument,	0,	'x'},
		{ "help",	no_argument,		0,	'h'}
	};

	while ((opt = getopt_long(argc, argv, "n:s:c:r:t:x:h", long_options, NULL)) != -1) {
		switch(opt)
		{
			case 'n':
				opt = sscanf(optarg, "%zu", &numiter);
				Z_die_if(opt != 1, "invalid numiter '%s'", optarg);
				break;

			case 's':
				opt = sscanf(optarg, "%zu", &blk_size);
				Z_die_if(opt != 1, "invalid blk_size '%s'", optarg);
				Z_die_if(blk_size < 8 || blk_size > UINT32_MAX,
					"blk_size %zu impossible", blk_size);
				break;

			case 'c':
				opt = sscanf(optarg, "%zu", &blk_cnt);
				Z_die_if(opt != 1, "invalid blk_cnt '%s'", optarg);
				Z_die_if(blk_cnt < 2,
					"blk_cnt %zu impossible", blk_cnt);
				break;

			case 'r':
				opt = sscanf(optarg, "%zu", &reservation);
				Z_die_if(opt != 1, "invalid reservation '%s'", optarg);
				Z_die_if(!reservation || reservation > blk_cnt,
					"reservation %zu; blk_cnt %zu", reservation, blk_cnt);
				break;

			case 't':
				opt = sscanf(optarg, "%zu", &tx_thread_cnt);
				Z_die_if(opt != 1, "invalid tx_thread_cnt '%s'", optarg);
				break;

			case 'x':
				opt = sscanf(optarg, "%zu", &rx_thread_cnt);
				Z_die_if(opt != 1, "invalid rx_thread_cnt '%s'", optarg);
				break;

			case 'h':
				usage(argv[0]);
				goto out;

			default:
				usage(argv[0]);
				Z_die("option '%c' invalid", opt);
		}
	}
	/* sanity check thread counts */
	Z_die_if(numiter != nm_next_mult64(numiter, tx_thread_cnt),
		"numiter %zu doesn't evenly divide into %zu tx threads",
		numiter, tx_thread_cnt);
	Z_die_if(numiter != nm_next_mult64(numiter, rx_thread_cnt),
		"numiter %zu doesn't evenly divide into %zu rx threads",
		numiter, rx_thread_cnt);
	/* sanity check reservation sizes */
	size_t num = numiter / tx_thread_cnt;
	Z_die_if(num != nm_next_mult64(num, reservation),
		"TX: num %zu doesn't evenly divide into %zu reservation blocks",
		num, reservation);
	num = numiter / rx_thread_cnt;
	Z_die_if(num != nm_next_mult64(num, reservation),
		"RX: num %zu doesn't evenly divide into %zu reservation blocks",
		num, reservation);


	/* create buffer */
	struct nbuf nb = { {0} };
	Z_die_if(
		nbuf_params(blk_size, blk_cnt, &nb)
		, "");
	Z_die_if(
		nbuf_init(&nb, malloc(nbuf_size(&nb)))
		, "size %zu", nbuf_size(&nb));

	void *(*tx_t)(void *) = tx_single;
	if (tx_thread_cnt > 1)
		tx_t = tx_multi;
	Z_die_if(!(
		tx = malloc(sizeof(pthread_t) + tx_thread_cnt)
		), "");

	void *(*rx_t)(void *) = rx_single;
	if (rx_thread_cnt > 1)
		rx_t = rx_multi;
	Z_die_if(!(
		rx = malloc(sizeof(pthread_t) + rx_thread_cnt)
		), "");

	nlc_timing_start(t);
		/* fire reader-writer threads */
		for (size_t i=0; i < tx_thread_cnt; i++)
			pthread_create(&tx[i], NULL, tx_t, &nb);
		for (size_t i=0; i < rx_thread_cnt; i++)
			pthread_create(&rx[i], NULL, rx_t, &nb);

		/* wait for threads to finish */
		size_t tx_i_sum = 0, rx_i_sum = 0;
		for (size_t i=0; i < tx_thread_cnt; i++) {
			void *tmp;
			pthread_join(tx[i], &tmp);
			tx_i_sum += (size_t)tmp;
		}
		for (size_t i=0; i < rx_thread_cnt; i++) {
			void *tmp;
			pthread_join(rx[i], &tmp);
			rx_i_sum += (size_t)tmp;
		}
	nlc_timing_stop(t);

	/* verify sums */
	Z_die_if(tx_i_sum != rx_i_sum, "%zu != %zu", tx_i_sum, rx_i_sum);
	/* This is the expected sum of all 'i' loop iterators for all threads
		The logic to arrive at this was:
			@1 : i = 0		0/1 = 0
			@2 : i = 0+1		2/1 = 0.5
			@3 : i = 0+1+2		3/3 = 1
			@4 : i = 0+1+2+3	6/4 = 1.5
			@5 : i = 0+1+2+3+4	10/5 = 2
			...
			@8 : i = 0+1+2+3+4+5+6+7	28/8	= 3.5
			@8 : (8-1)*0.5				= 3.5
			@8 : 0+1+2+3+4+5+6+7 = (8-1)*0.5*8 = 28
	*/
	numiter /= tx_thread_cnt;
	size_t verif_i_sum = (numiter -1) * 0.5 * numiter * tx_thread_cnt;
	Z_die_if(verif_i_sum != tx_i_sum, "%zu != %zu", verif_i_sum, tx_i_sum);

	/* print stats */
	printf("numiter %zu; blk_size %zu; blk_count %zu; reservation %zu\n",
		numiter, blk_size, blk_cnt, reservation);
	printf("TX threads %zu; RX threads %zu\n",
		tx_thread_cnt, rx_thread_cnt);
	printf("cpu time: %.4lfs\n", nlc_timing_secs(t));

out:
	nbuf_deinit(&nb);
	free(nb.ct.buf);
	free(tx);
	free(rx);
	return err_cnt;
}
