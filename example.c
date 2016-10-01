/* 
  you need masriadb-connector-c and libevent to compile this example:
 
  gcc -std=gnu11 -o async mysql_async_example.c -lmariadb -luv

	 -I"/usr/local/include/mariadb/"
	 -L"/usr/local/lib/mariadb"

 */ 

#include "msa.h"
#include <uv.h>

int nr_after_query_cb_calls = 0;

static void res_ready_cb(msa_query_t *query, MYSQL_RES *result, MYSQL_ROW row) {
	printf("res_ready_cb, query: %p\n", query);
}
static void after_query_cb(msa_query_t *query, int status, int mysql_status) {
	printf("after_query_cb,  query: %p\n", query);
	free((void*)query->query);
	++nr_after_query_cb_calls;
	assert(status == 0);
	assert(status == mysql_status);
}

static void pool_error_cb(msa_pool_t *pool, int status, int mysql_status) {
	fprintf(stderr, "pool_error_cb,  status: %d   mysql_status: %d\n", status, mysql_status);
}

int main(int argc, char *argv[]) {
	msa_connection_details_t opts;
	msa_query_t *queries;
	int res;
	int opt_query_num = (argc > 1)? atoi(argv[1]) : 1000;
	int opt_connections = (argc > 2)? atoi(argv[2]) : 1;
	uv_loop_t *loop = malloc(sizeof(uv_loop_t));
	char buffer[256];
	msa_pool_t pool;
	char *my_groups[2] = { "client", NULL };
	char *strQuery;

	memset(&opts, 0, sizeof(opts));
	opts.db = (argc > 3)? argv[3] : "telem";
	opts.user = (argc > 4)? argv[4] : "telem";
	opts.password = (argc > 5)? argv[5] : "A9JKdvdYQMdFg3pVFd9u";
	opts.host = (argc > 6)? argv[6] : "telem.czxotmctpc9j.us-west-2.rds.amazonaws.com";
	opts.initial_nr_conns = 32;
	opts.min_nr_conns = 32;
	opts.max_nr_conns = 32;
	opts.error_cb = NULL;
	opts.timeout = 0;
	opts.max_nr_successive_connection_fails = 0;
	
	if (!loop) {
		fprintf(stderr, "Out of memory");
		exit(1);
	}

	res = uv_loop_init(loop);
	assert(res == 0);

	res = msa_pool_init(&pool, &opts, loop);
	assert(res == 0);

	queries = (msa_query_t*)calloc(opt_query_num, sizeof(msa_query_t));
	if (!queries) {
		fprintf(stderr, "Out of memory");
		exit(1);
	}

	for (int i = 0; i < opt_query_num; i++) {
		sprintf(buffer, "insert into z1 values(%d)", i);
		strQuery = malloc(sizeof(buffer)+1);
		if (!strQuery) {
			fprintf(stderr, "Out of memory");
			exit(1);
		}
		strcpy(strQuery, buffer);
		res = msa_query_init(queries+i, strQuery, res_ready_cb, after_query_cb, NULL);
		assert(res == 0);
	}

	// todo: run this from an event
	for (int i = 0; i < opt_query_num; i++) {
		res = msa_query_start(&pool, queries+i);
		assert(res == 0);
	}

	int err = mysql_library_init(argc, argv, (char **)my_groups);
	if (err) {
		fprintf(stderr, "Fatal: mysql_library_init() returns error: %d\n", err);
		exit(1);
	}

	uv_run(loop, UV_RUN_DEFAULT);

	/*long diff = (t2.tv_sec - t1.tv_sec)*1000000 + t2.tv_usec - t1.tv_usec;
	printf("diff: %d\n", diff); */

	res = msa_pool_nr_pending_queries(&pool);
	assert(res == 0);

	res = msa_pool_close(&pool);
	assert(res == 0);

	assert(nr_after_query_cb_calls == opt_query_num);

	free(queries);
	mysql_library_end();

	uv_loop_close(loop);
	free(loop);

	return 0;
}
