#ifndef MSA_H_
#define MSA_H_

#include <stdlib.h>
#include <string.h>
#include <mysql.h>
//#include <my_global.h>
//#include <my_sys.h>
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
	MSA_EQUERYSTOPPING = 8192,
	MSA_ECLOSING = 16384,
};

struct msa_dal_s;
typedef struct msa_dal_s msa_dal_t;
struct msa_pool_s;
typedef struct msa_pool_s msa_pool_t;
struct msa_connection_s;
typedef struct msa_connection_s msa_connection_t;
struct msa_query_s;
typedef struct msa_query_s msa_query_t;

typedef void (*msa_query_res_ready_cb)(msa_query_t *query, MYSQL_RES *result, MYSQL_ROW row);  /* TODO: maybe pass row nr? */
typedef void (*msa_after_query_cb)(msa_query_t *query, int status, int mysql_status);
typedef void (*msa_pool_error_cb)(msa_pool_t *pool, int status, int mysql_status);
typedef void (*msa_pool_close_cb)(msa_pool_t *pool);
typedef void (*msa_dal_close_cb)(msa_dal_t *pool);

/* Use zero-values for defaults. So that you can bzero(..) the whole struct and set only the relevant fields. */
typedef struct msa_connection_details_s {
	char *db;
	char *user;
	char *password;
	char *host;

	size_t initial_nr_conns; // zero for default
	size_t min_nr_conns; // zero for default
	size_t max_nr_conns; // zero for no limit

	msa_pool_error_cb error_cb; // can be NULL
	uint timeout; // zero for default

	int max_nr_successive_connection_fails; // zero for default
} msa_connection_details_t;


/**
 * Initializes a connections pool. Assume all the params are properly allocated (and not NULL).
 *
 * @param pool - 	Pre-allocated connections pool, to be initialized.
 * @param opts - 	Pre-allocated & initialized options struct.
 * 					All fields can be zero for default value, so user can memset the whole estruct to zero.
 * 					Should be available (not freed) as long as the pool is alive (not closed).
 * @param loop - 	UV events loop to run poll & timer event handles on.
 * 
 * @return			Error code. zero for success.
 *					On error, the pool needs to be closed properly with `msa_pool_close(..)`.
 */
int msa_pool_init(msa_pool_t *pool, msa_connection_details_t* opts, uv_loop_t *loop);

/**
 * Closes an already initialized connections pool. Assume all the params are properly allocated (and not NULL).
 *
 * @param pool - 	Pre-initialized connections pool, to be closed.
 * @param close_cb 	Callback to be called when the pool is successfully closed.
 *					`pool` (and additional resources) can be freed by this callback.
 *					Before the callback is called, the pool mustn't be freed.
 * 
 * @return			Error code. zero for success.
 */
int msa_pool_close(msa_pool_t *pool, msa_pool_close_cb close_cb);

/**
 * @param pool - 	Pre-initialized connections pool.
 * 
 * @return			Number of queries attached to this pool with `msa_query_start(..)`
 *					but has not been starting to process yet (waiting to a free connection).
 */
int msa_pool_nr_pending_queries(msa_pool_t *pool);

/**
 * @param pool - 	Pre-initialized connections pool.
 * 
 * @return			Number of queries attached to this pool with `msa_query_start(..)`
 *					and already while processing by a connection.
 */
int msa_pool_nr_active_connections(msa_pool_t *pool);


/**
 * Initializes a query. Assume all the params are properly allocated (and not NULL).
 *
 * @param query  		Pre-allocated query structure, to be initialized.
 * @param query_str		Actual sql query to be executed on server.
 *						Should be available (not freed) as long as the query is running (not stopped).
 * @param res_ready_cb  Callback to be called after each row is fetched.
 * @param after_cb 		Callback to be called after the query execution is complited.
 *						If an error has been occoured while executing the query, the callback is called
 *						with a proper status that indicates the error number (see `enum msa_error_code` for details),
 *						and mysql_status to indicate the error code retuened by MySQL.
 *						`query` and other resources may be freed by this callback.
 *						No other callbacks will be called on this query afterwards.
 * 
 * @return				Error code. zero for success.
 */
int msa_query_init(msa_query_t* query, const char *query_str, msa_query_res_ready_cb res_ready_cb, msa_after_query_cb after_cb);

/**
 * Start a pre-initialized query. Assume all the params are properly allocated (and not NULL).
 * The query will wait (in a pending queue) to a free connection (with respect to the open connections policy of this pool)
 * After a free connection is available for this query, it will be executed.
 *
 * @param pool  	Pre-initialized conncetions pool structure, that the query would be executed on.
 * @param query  	Pre-initialized query structure, to be started.
 * 
 * @return			Error code. zero for success.
 */
int msa_query_start(msa_pool_t* pool, msa_query_t* query);

/**
 * Stop a query. Assume the given param is properly allocated&initiallized (and not NULL).
 * If the query is not started yet (pending), it will be stopped immediatly.
 * Otherwise, if the query is during execution, it will be stopped only after the current execution stage is done.
 * If the connection is currently fetching rows, it will be stopped after the current fetch is done.
 * Nevertheless it will not be stopped as long as the connection is waiting for the server to respond or the socket to be closed.
 * The `after_cb` (given at auery initialize) will be called after the query is really stopped.
 * No additional callbacks (of this query) will be called this call.
 *
 * @param query  	Query structure that has been already started.
 * 
 * @return			Error code. zero for success.
 */
int msa_query_stop(msa_query_t* query);

/**
 * @param query  	Query structure that has been already initialized.
 * 
 * @return			The query string given by the user on initialization.
 */
const char* msa_query_get(msa_query_t* query);

void* msa_query_get_context(msa_query_t* query);
void msa_query_set_context(msa_query_t* query, void* context);

/**
TODO: fix this doc!!
 * Initializes a DAL (Data Access Level). Assume all the params are properly allocated (and not NULL).
 *
 * @param pool - 	Pre-allocated connections pool, to be initialized.
 * @param opts - 	Pre-allocated & initialized options struct.
 * 					All fields can be zero for default value, so user can memset the whole estruct to zero.
 * 					Should be available (not freed) as long as the pool is alive (not closed).
 * @param loop - 	UV events loop to run poll & timer event handles on.
 * 
 * @return			Error code. zero for success.
 *					On error, the pool needs to be closed properly with `msa_pool_close(..)`.
 */
int msa_dal_init(msa_dal_t* dal, uv_loop_t *loop, const char* host, const char* user, const char* password, const char* db);

// todo: add doc!
int msa_dal_execute(msa_dal_t *dal, const char* query_str, msa_query_res_ready_cb res_ready_cb, msa_after_query_cb after_cb, void* context);

// todo: add doc!
int msa_dal_close(msa_dal_t *dal, msa_dal_close_cb close_cb);



/* structures - defined here so the user will know the size of each type, in order to allocate it properly */

struct msa_query_s {
	/* public */
	void* context; // not accessed and not initialized by the library.

	/* private */
	const char *query;
	msa_query_res_ready_cb res_ready_cb;
	msa_after_query_cb after_query_cb;

	list_t query_list;
	msa_connection_t *conn;
	msa_pool_t *pool; // used for the period that the query is pending, and has no conn yet.
	int stopping;
#ifdef MSA_USE_STATISTICS
	query_stats_category_t stat_category;
#endif // MSA_USE_STATISTICS
};

struct msa_pool_s {
	/* public */
	void* context; // not accessed and not initialized by the library.

	/* private */
	uv_loop_t* loop;
	msa_connection_details_t *opts;

	list_t pending_queries_list_head;  // waiting for connections to be freed.
	size_t nr_pending_queries;
	list_t active_queries_list_head;   // already assigned to a conn & being processed.
	size_t nr_active_queries;

	/* connections. each element in the list is a `msa_connection_t` */
	/* the following 2 lists are mutual exclusive */
	list_t nonactive_conns_list_head;	// during initializing, have not been connected yet.
	list_t active_conns_list_head;		// connected, ready to process query or already processing a query.
	size_t nr_nonactive_conns;
	size_t nr_active_conns;

	/* a free conn is also active */
	list_t free_conns_list_head;		// connected, and available to process a query (currently not processing)
	size_t nr_free_conns;

	unsigned long last_freed_conn_time; // todo: use right time type
	size_t new_pending_queries;  // being reset when a conn is freed. used to control num of opened conns.

  	int nr_successive_connection_fails;

  	int closing;
  	msa_pool_close_cb close_cb;

#ifdef MSA_USE_STATISTICS
	// todo: use right time type
	unsigned long avg_query_time;
	unsigned long avg_query_time_by_type[NR_QUERY_CATEGORIES_FOR_STATS];
#endif // MSA_USE_STATISTICS
};

struct msa_dal_s {
	msa_pool_t pool;
	msa_connection_details_t opts;
	msa_dal_close_cb close_cb;
	// TODO: list of available memory resources
};


#endif // MSA_H_
