/* 
  you need masriadb-connector-c and libevent to compile this example:
 
  gcc -std=gnu11 -o async test1.c msa.c -lmariadb -luv

	 -I"/usr/local/include/mariadb/"
	 -L"/usr/local/lib/mariadb"

 */ 

#include <assert.h>
#include "msa.h"
#include <uv.h>
#include "test_utilities.h"

#define TEST_TABLE_NAME "msa_test_table"

enum test1_states {
	TEST1_DROP_TEST_TABLE,
	TEST1_CREATE_TEST_TABLE,
	TEST1_INSERT_TO_TEST_TABLE,
	TEST1_SELECT_FROM_TEST_TABLE,
	TEST1_CLOSING_POOL,
	TEST1_DONE
};

typedef struct test_context {
	uv_loop_t loop;
	msa_pool_t pool;
	msa_connection_details_t opts;
	msa_query_t drop_query, create_query, *insert_queries, *select_queries;
	
	uv_timer_t closing_timer;
	int nr_after_query_cb_calls;
	int nr_failed_conns;
	int nr_queries;
	int *nr_fetched_rows_per_select_query;

	int current_state;
} test_context_t;


static void pool_close_cb(msa_pool_t *pool) {
	test_context_t *test = (test_context_t*)pool->context;

	ASSERT_EQUALS(test->current_state, TEST1_CLOSING_POOL);
	test->current_state = TEST1_DONE;
}

static void test1_res_ready_cb(msa_query_t *query, MYSQL_RES *result, MYSQL_ROW row) {
	test_context_t *test = (test_context_t*)query->context;
	int query_idx = (query - test->select_queries);

	ASSERT_GE(query_idx, 0);
	ASSERT_LT(query_idx, test->nr_queries);

	test->nr_fetched_rows_per_select_query[query_idx]++;

	// todo: read fetched values
}
static void test1_after_query_cb(msa_query_t *query, int status, int mysql_status) {
	test_context_t *test = (test_context_t*)query->context;
	int res;

	if (status != 0 || mysql_status != 0) {
		printf("test1_after_query_cb: state: %d status:%d mysql:%d query: `%s`\n", test->current_state, status, mysql_status, query->query);
		exit(1);
	}

	switch(test->current_state) {
		case TEST1_DROP_TEST_TABLE:
			ASSERT_EQUALS(query, &test->drop_query);

			res = msa_query_start(&test->pool, &test->create_query);
			ASSERT_ZERO(res);
			test->current_state = TEST1_CREATE_TEST_TABLE;
			break;

		case TEST1_CREATE_TEST_TABLE:
			for (int i = 0; i < test->nr_queries; i++) {
				res = msa_query_start(&test->pool, test->insert_queries+i);
				ASSERT_ZERO(res);
			}
			test->current_state = TEST1_INSERT_TO_TEST_TABLE;
			break;

		case TEST1_INSERT_TO_TEST_TABLE:
			free((void*)query->query);
			test->nr_after_query_cb_calls++;
			if (test->nr_after_query_cb_calls == test->nr_queries) {
				test->current_state = TEST1_SELECT_FROM_TEST_TABLE;
				test->nr_after_query_cb_calls = 0;
				for (int i = 0; i < test->nr_queries; i++) {
					res = msa_query_start(&test->pool, test->select_queries+i);
					ASSERT_ZERO(res);
				}
			}
			break;

		case TEST1_SELECT_FROM_TEST_TABLE:
			free((void*)query->query);
			test->nr_after_query_cb_calls++;

			if (test->nr_after_query_cb_calls == test->nr_queries) {
				for(int i = 0; i < test->nr_queries; i++) {
					ASSERT_EQUALS(test->nr_fetched_rows_per_select_query[i], min((i%5)+1, test->nr_queries-i));
				}
				test->current_state = TEST1_CLOSING_POOL;
				msa_pool_close(&test->pool, pool_close_cb);
			}
			break;

		case TEST1_CLOSING_POOL:
		case TEST1_DONE:
		default:
			ASSERT(0);
			break;
	}
	
	ASSERT_ZERO(status);
	ASSERT_ZERO(mysql_status);
}

static void pool_error_cb(msa_pool_t *pool, int status, int mysql_status) {
	test_context_t *test = (test_context_t*)pool->context;

	fprintf(stderr, "pool_error_cb,  status: %d   mysql_status: %d\n", status, mysql_status);
	if (status & MSA_ECONNFAIL) {
		test->nr_failed_conns++;
	}
}

static void closing_timer_cb(uv_timer_t* handle) {
	test_context_t *test = (test_context_t*)handle->data;

	if (test->nr_after_query_cb_calls > 0) {
		msa_pool_close(&test->pool, pool_close_cb);
		uv_timer_stop(handle);
	} else if (test->nr_failed_conns >= test->opts.initial_nr_conns) {
		msa_pool_close(&test->pool, pool_close_cb);
		uv_timer_stop(handle);
	}
}

static void closing_timer_close_cb(uv_handle_t *handle) {}

static char *my_groups[2] = { "client", NULL };

static int _argc;
static char** _argv;

int test1() {
	test_context_t test;

	int res;
	int opt_query_num = (_argc > 1)? atoi(_argv[1]) : 1000;
	int opt_nr_connections = (_argc > 2)? atoi(_argv[2]) : 1;
	char buffer[256];
	char *strQuery;

	test.nr_queries = opt_query_num;
	test.nr_after_query_cb_calls = 0;
	test.nr_failed_conns = 0;
	memset(&test.opts, 0, sizeof(test.opts));
	test.opts.db = (_argc > 3)? _argv[3] : "";
	test.opts.user = (_argc > 4)? _argv[4] : "";
	test.opts.password = (_argc > 5)? _argv[5] : "";
	test.opts.host = (_argc > 6)? _argv[6] : "";
	test.opts.initial_nr_conns = opt_nr_connections;
	test.opts.min_nr_conns = opt_nr_connections;
	test.opts.max_nr_conns = opt_nr_connections;
	test.opts.error_cb = pool_error_cb;
	test.opts.timeout = 0;
	test.opts.max_nr_successive_connection_fails = 0;

	test.current_state = TEST1_DROP_TEST_TABLE;

	res = uv_loop_init(&test.loop);
	ASSERT_ZERO(res);

	int err = mysql_library_init(_argc, _argv, (char **)my_groups);
	if (err) {
		fprintf(stderr, "Fatal: mysql_library_init() returns error: %d\n", err);
		exit(1);
	}

	res = msa_pool_init(&test.pool, &test.opts, &test.loop);
	ASSERT_ZERO(res);
	test.pool.context = (void*)&test;

	res = msa_query_init(&test.drop_query, "DROP TABLE IF EXISTS " TEST_TABLE_NAME ";", test1_res_ready_cb, test1_after_query_cb, NULL);
	test.drop_query.context = (void*)&test;
	ASSERT_ZERO(res);

	res = msa_query_init(&test.create_query, "CREATE TABLE " TEST_TABLE_NAME " (`id` int(11) NOT NULL) ENGINE=InnoDB DEFAULT CHARSET=latin1;", test1_res_ready_cb, test1_after_query_cb, NULL);
	test.create_query.context = (void*)&test;
	ASSERT_ZERO(res);

	test.insert_queries = (msa_query_t*)calloc(opt_query_num, sizeof(msa_query_t));
	if (!test.insert_queries) {
		fprintf(stderr, "Out of memory");
		exit(1);
	}

	test.select_queries = (msa_query_t*)calloc(opt_query_num, sizeof(msa_query_t));
	test.nr_fetched_rows_per_select_query = (int*)calloc(opt_query_num, sizeof(int));
	if (!test.select_queries) {
		fprintf(stderr, "Out of memory");
		exit(1);
	}

	for (int i = 0; i < opt_query_num; i++) {
		sprintf(buffer, "insert into " TEST_TABLE_NAME " values(%d)", i);
		strQuery = malloc(sizeof(buffer)+1);
		if (!strQuery) {
			fprintf(stderr, "Out of memory");
			exit(1);
		}
		strcpy(strQuery, buffer);
		res = msa_query_init(test.insert_queries+i, strQuery, test1_res_ready_cb, test1_after_query_cb, NULL);
		test.insert_queries[i].context = (void*)&test;
		ASSERT_ZERO(res);
	}

	for (int i = 0; i < opt_query_num; i++) {
		test.nr_fetched_rows_per_select_query[i] = 0;
		sprintf(buffer, "SELECT id FROM " TEST_TABLE_NAME " WHERE id >= %d AND id <= %d;", i, i+(i%5));
		strQuery = malloc(sizeof(buffer)+1);
		if (!strQuery) {
			fprintf(stderr, "Out of memory");
			exit(1);
		}
		strcpy(strQuery, buffer);
		res = msa_query_init(test.select_queries+i, strQuery, test1_res_ready_cb, test1_after_query_cb, NULL);
		test.select_queries[i].context = (void*)&test;
		ASSERT_ZERO(res);
	}

	// set a timer to close the pool before it is done.
	/*uv_timer_init(&test.loop, &closing_timer);
	closing_timer.data = (void*)&test;
	uv_timer_start(&closing_timer, closing_timer_cb, 100, 100);*/

	res = msa_query_start(&test.pool, &test.drop_query);
	ASSERT_ZERO(res);

	// to run queries
	uv_run(&test.loop, UV_RUN_DEFAULT);

	/*uv_close((uv_handle_t*)(&closing_timer), closing_timer_close_cb);*/

	res = msa_pool_nr_pending_queries(&test.pool);
	ASSERT_ZERO(res);

	//res = msa_pool_close(&test.pool, pool_close_cb);
	//ASSERT_ZERO(res);  /*-MSA_EPOOLCLOSING*/

	uv_run(&test.loop, UV_RUN_DEFAULT); // to close all conns

	ASSERT_EQUALS(test.current_state, TEST1_DONE);
	//ASSERT_EQUALS(nr_after_query_cb_calls, opt_query_num);

	free(test.insert_queries);
	free(test.select_queries);
	free(test.nr_fetched_rows_per_select_query);

	mysql_library_end();

	uv_loop_close(&test.loop);

	return 1;
}

int main(int argc, char** argv) {
	_argc = argc;
	_argv = argv;

	RUN_TEST(test1);

	return 0;
}
