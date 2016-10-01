#ifndef MSA_H_
#define MSA_H_

#include <string.h>
#include <mysql.h>
#include <my_global.h>
#include <my_sys.h>
#include <uv.h>

#include "list.h"
#include "uv_timeout_poll.h"


// TODO: maybe change to `msa_status_code` ?
enum msa_error_code {
	MSA_EINVAL = 1,
	MSA_EMYSQLERR = 2,
	MSA_ECONNFAIL = 4,
	MSA_EQUERYFAIL = 8,
	MSA_ETIMEOUT = 16,
	MSA_ENOACTIVECONNS = 32,
	MSA_EEXCEED_PENDING_QUERIES_LIMIT = 64,
	MSA_EHUP = 128,
	MSA_EOUTOFMEM = 256,
	MSA_ENOFREECONNS = 512,
	MSA_ENOPENDINGQUERIES = 1024,
	MSA_EEXCEED_FAIL_CONN_ATTEMPTS_LIMIT = 2048,
	MSA_EQUERYSTOP = 4096,
};

struct msa_pool_s;
typedef struct msa_pool_s msa_pool_t;
struct msa_connection_s;
typedef struct msa_connection_s msa_connection_t;
struct msa_query_s;
typedef struct msa_query_s msa_query_t;

typedef void (*msa_query_res_ready_cb)(msa_query_t *query, MYSQL_RES *result, MYSQL_ROW row);  /* TODO: maybe pass row nr? */
typedef void (*msa_after_query_cb)(msa_query_t *query, int status, int mysql_status);
typedef void (*msa_pool_error_cb)(msa_pool_t *pool, int status, int mysql_status);

/* Use zero-values for defaults. So that you can bzero the whole struct and set only the relevant fields. */
typedef struct msa_connection_details_s {
	char *db;
	char *user;
	char *password;
	char *host;
	//char *my_groups[2]; /* in example it is set to { "client", NULL }; */

	// zero for no limit
	size_t initial_nr_conns;
	size_t min_nr_conns;
	size_t max_nr_conns;

	msa_pool_error_cb error_cb; // can be NULL
	uint timeout; // zero for default

	int max_nr_successive_connection_fails; // zero for default
} msa_connection_details_t;

#include "types.h"


int msa_pool_init(msa_pool_t *pool, msa_connection_details_t* opts, uv_loop_t *loop);
int msa_pool_close(msa_pool_t *pool);
int msa_pool_nr_pending_queries(msa_pool_t *pool);
int msa_pool_nr_active_connections(msa_pool_t *pool);

int __msa_pool_add_connection(msa_pool_t *pool);



int msa_query_init(msa_query_t* query, const char *str, msa_query_res_ready_cb res_ready_cb, msa_after_query_cb after_query_cb, void* context);
int msa_query_start(msa_pool_t* pool, msa_query_t* query);
int msa_query_stop(msa_query_t* query);
const char* msa_query_get(msa_query_t* query);
void* msa_query_get_context(msa_query_t* query);
void msa_query_set_context(msa_query_t* query, void* context);

#endif // MSA_H_
