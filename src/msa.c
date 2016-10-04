/* 
  you need masriadb-connector-c and libevent to compile this example:
 
  gcc -std=gnu11 -c msa.c -I"/usr/include/mysql"

	 test:
	 https://ideone.com/5w8uCu

 */ 

/**
	Questions:
	Does the mysql fd stays the same?
**/

#include <assert.h>
#include <string.h>
#include "msa.h"
#include "list.h"

#define DEFAULT_TIMEOUT 300
#define DEFAULT_MAX_NR_SUCCESSIVE_CONNECTION_FAILS 3

enum msa_async_state {
  CONNECT_START,
  CONNECT_WAITING,
  CONNECT_DONE,

  QUERY_START,
  QUERY_WAITING,
  QUERY_RESULT_READY,
  QUERY_FREE_RESULT_START,
  QUERY_FREE_RESULT_WAITING,
  QUERY_FREE_RESULT_DONE,

  FETCH_ROW_START,
  FETCH_ROW_WAITING,
  FETCH_ROW_RESULT_READY,

  CLOSE_START,
  CLOSE_WAITING,
  CLOSE_DONE
};

#ifdef MSA_USE_STATISTICS
typedef char query_stats_category_t;
#define NR_QUERY_CATEGORIES_FOR_STATS (1<<(8*sizeof(query_stats_category_t)))
#endif // MSA_USE_STATISTICS

enum msa_conn_open_reason {
  MSA_OCONN_INIT,
  MSA_OCONN_BALANCE_POLICY,
};

#define __msa_conn_is_active(conn) (conn->is_active)
#define __msa_conn_is_free(conn) (!msa_list_empty(&conn->free_conns_list))
#define __msa_pool_nr_conns(pool) (pool->nr_active_conns + pool->nr_nonactive_conns)
#define __msa_query_is_pending(query) (query->conn == NULL)


static void __msa_connection_state_machine_handler(uv_timeout_poll_t* handle, int status, int event);
static inline int add_event(int status, msa_connection_t *conn);
static inline int decide_next_state(int op_status, msa_connection_t* conn, 
    int state_wait, int state_go_on);
static inline int mysql_status(int stat, int event);
static inline int init_poll_handle(msa_connection_t* conn);


static int __msa_pool_try_to_close_conns(msa_pool_t* pool);
static int __msa_pool_try_to_open_conns(msa_pool_t* pool);
static int __msa_connection_init(msa_connection_t *conn, msa_pool_t *pool, int openning_reason);
static int __msa_connection_del_current_query(msa_connection_t *conn);
static int __msa_pool_try_wake_free_conn(msa_pool_t *pool);
static int __msa_pool_del_closed_connection(msa_connection_t *conn);
static inline int __msa_pool_del_closed_connection_wo_poll_handle(msa_connection_t *conn);
static int __msa_conn_close(msa_connection_t* conn);
static inline int __msa_pending_query_close(msa_query_t *query);
static inline int __msa_conn_wake_up(msa_connection_t *conn);



/**

  Notes:
  1. when wrting the new conns open policy, and closing last chance: keep in mind to check `pool->closing`, if it's on we don't want to open new conns.

**/



// add event to libuv
static inline int add_event(int status, msa_connection_t *conn) {
  int wait_events = 0;
  int ret;
  uint64_t timeout = 0;

  if ((status & MYSQL_WAIT_READ) || (status & MYSQL_WAIT_EXCEPT))
    wait_events|= UV_READABLE;
  if (status & MYSQL_WAIT_WRITE)
    wait_events|= UV_WRITABLE;

// TODO: do this check on production and handle errors somehow.
#ifndef NDEBUG
	// verify fd not changed
	uv_os_fd_t ofd = 0;
	uv_fileno((uv_handle_t*)(&conn->timeout_poll_handle.poll), &ofd);
	assert(mysql_get_socket(&conn->mysql) == ofd);
#endif // NDEBUG
	
  if (status & MYSQL_WAIT_TIMEOUT) {
  	timeout = mysql_get_timeout_value_ms(&conn->mysql);
  }

  ret = uv_timeout_poll_start(&conn->timeout_poll_handle, wait_events, __msa_connection_state_machine_handler, timeout);

  return ret;
}

// op_status  : return value of a db operation
// state_wait : one of ( CONNECT_WAITING, QUERY_WAITING, FETCH_ROW_WAITING, CLOSE_WAITING)
static inline int decide_next_state(int op_status, msa_connection_t* conn, 
    int state_wait, int state_go_on){ 
  if (op_status){  
    // need to wait for data(add event to libevent)
    conn->current_state = state_wait;
    add_event(op_status, conn);   
    return 0;
  } else {        
    // no need to wait, go to next state immediately
    conn->current_state = state_go_on;
    return 1;
  }
}

// event : libevent -> mysql
static inline int mysql_status(int stat, int event) {
  int status = 0;
  if (event & UV_READABLE)
    status|= MYSQL_WAIT_READ;
  if (event & UV_WRITABLE)
    status|= MYSQL_WAIT_WRITE;
  if (stat == UV_ETIMEDOUT || event & UV_DISCONNECT)  // TODO: is it what we want here?
    status|= MYSQL_WAIT_TIMEOUT;
  return status;
}

static inline int init_poll_handle(msa_connection_t* conn) {
	int fd = mysql_get_socket(&conn->mysql);
	assert(fd >= 0);
	return uv_timeout_poll_init(conn->pool->loop, &conn->timeout_poll_handle, fd, UV_STOP_HANDLES_EACH_EVENT);
}

// Called when a conn has been added to free-conns list.
static int __msa_pool_try_to_close_conns(msa_pool_t* pool) {
	// TODO: implement
	return 0;
}

// Called when a new query has been added. The ratio #queries : #conns has increased.
// TODO: Define a policy for opening new connections, with respect to this ratio.
//       Save history in order to make decision considering stable data (to avoid unstable picks)
static int __msa_pool_try_to_open_conns(msa_pool_t* pool) {
	// TODO: implement
	return 0;
}

// call this to entry state_machine
static void __msa_connection_state_machine_handler(uv_timeout_poll_t* handle, int status, int event) {
  msa_connection_t *conn = (msa_connection_t*)handle->data;
  int state_machine_continue = 1;
  list_t* pending_query_node;
  msa_query_t* prev_query;

  if (event & UV_DISCONNECT) {
    // TODO: implement
  }

  while (state_machine_continue){
    switch(conn->current_state) {

      /*
        The mysql async methods return value is a bitmask of events that the lib is waiting for.
        The first param is the real output parameter of the method (passed as pointer).
      */

      case CONNECT_START:   
        status = mysql_real_connect_start(&conn->ret, &conn->mysql, conn->pool->opts->host, 
        	conn->pool->opts->user, conn->pool->opts->password, conn->pool->opts->db, 0, NULL, CLIENT_REMEMBER_OPTIONS /*to avoid segfault on fail*/);
        if (status == 0 && !conn->ret) {
          conn->pool->nr_successive_connection_fails++;
          if (conn->pool->opts->error_cb != NULL)
            conn->pool->opts->error_cb(conn->pool, MSA_EMYSQLERR | MSA_ECONNFAIL, mysql_errno(&conn->mysql));
          __msa_pool_del_closed_connection_wo_poll_handle(conn); /* poll handle is not initialized here yet */
          return;
        }
        init_poll_handle(conn);  // TODO: handle return errors
        state_machine_continue = decide_next_state(status, conn, CONNECT_WAITING, CONNECT_DONE);   
        break;

      case CONNECT_WAITING: 
        status = mysql_real_connect_cont(&conn->ret, &conn->mysql, mysql_status(status, event));
        state_machine_continue = decide_next_state(status, conn, CONNECT_WAITING, CONNECT_DONE);   
        break;

      case CONNECT_DONE: 
        assert(conn->current_query_entry == NULL);
        if (!conn->ret) {
          conn->pool->nr_successive_connection_fails++;

          //fatal(conn, "Failed to mysql_real_connect()");
          if (conn->pool->opts->error_cb != NULL)
            conn->pool->opts->error_cb(conn->pool, MSA_EMYSQLERR | MSA_ECONNFAIL, mysql_errno(&conn->mysql));

          // remove this conn from pool
          __msa_pool_del_closed_connection(conn);

          // TODO: try create new conn if needed & not exceeded max num of successive conn create failure (conn->pool->nr_successive_connection_fails < conn->pool->opts->max_nr_successive_connection_fails).
          //       conn->pool->opts->error_cb(conn->pool, MSA_EEXCEED_FAIL_CONN_ATTEMPTS_LIMIT, mysql_errno(&conn->mysql)); 
          //       maybe we should relate to create_reason.
          return;
        }
        conn->pool->nr_successive_connection_fails = 0;
        conn->current_state = QUERY_START; 

        // make conn active.
        assert(!__msa_conn_is_active(conn));
        msa_list_del(&conn->conns_list);
        conn->pool->nr_nonactive_conns--;
        msa_list_add_tail(&conn->conns_list, &conn->pool->active_conns_list_head);
        conn->pool->nr_active_conns++;
        conn->is_active = 1;

        break;

      case QUERY_START: 
        assert(conn->current_query_entry == NULL);

        if (conn->closing) {
            // TODO: give last chance here depending on the closing reason (the value of `conn->closing`).
            //        means the load balancer should check here again if we need that conn or not.
            conn->current_state = CLOSE_START;
            state_machine_continue = 1;
            break;
        }

      	// TODO: lock query_list.
        if (msa_list_empty(&conn->pool->pending_queries_list_head)) {  
        	// add this conn to pool free connections. it will be used again when new queries will arrive.
          /*conn->current_query_entry = NULL;*/
        	msa_list_add_tail(&conn->free_conns_list, &conn->pool->free_conns_list_head);
        	conn->pool->nr_free_conns++;

        	__msa_pool_try_to_close_conns(conn->pool);

          // this conn might be closed&freed by now. ensure we exit this handle.
          return;
        }

        pending_query_node = conn->pool->pending_queries_list_head.next;
        assert(pending_query_node != &conn->pool->pending_queries_list_head);  // we verified that the list is not empty.
        conn->current_query_entry = msa_list_entry(pending_query_node, msa_query_t, query_list);
        msa_list_del(pending_query_node);
        conn->pool->nr_pending_queries--;
        msa_list_add_tail(pending_query_node, &conn->pool->active_queries_list_head);
        conn->pool->nr_active_queries++;

        status = mysql_real_query_start(&conn->err, &conn->mysql, conn->current_query_entry->query,
            strlen(conn->current_query_entry->query));
        state_machine_continue = decide_next_state(status, conn, QUERY_WAITING, QUERY_RESULT_READY);   
        break;

      case QUERY_WAITING:
        status = mysql_real_query_cont(&conn->err, &conn->mysql, mysql_status(status, event));
        state_machine_continue = decide_next_state(status, conn, QUERY_WAITING, QUERY_RESULT_READY);   
        break;

      case QUERY_RESULT_READY:
        assert(conn->current_query_entry != NULL);
        if (conn->current_query_entry->stopping > 0) {
            prev_query = conn->current_query_entry;
            __msa_connection_del_current_query(conn);
            prev_query->after_query_cb(prev_query, MSA_EQUERYSTOP, mysql_errno(&conn->mysql));

            /* 
              We cannot procceed to new SRART_QUERY now.
              We must first free the current result if exists ( https://mariadb.com/kb/en/mariadb/mysql_use_result/ )
            */
            conn->result = mysql_use_result(&conn->mysql);
            if (conn->result) {
              conn->current_state = QUERY_FREE_RESULT_START;
            } else {
              // end of query (no results)
              conn->current_state = QUERY_START;
            }
            break;
        }
        if (conn->err) {
          prev_query = conn->current_query_entry;
          __msa_connection_del_current_query(conn);
          prev_query->after_query_cb(prev_query, MSA_EMYSQLERR | MSA_EQUERYFAIL, mysql_errno(&conn->mysql));
          if (conn->pool->opts->error_cb != NULL)
            conn->pool->opts->error_cb(conn->pool, MSA_EMYSQLERR | MSA_EQUERYFAIL, mysql_errno(&conn->mysql));
          conn->current_state = QUERY_START;  // Notice: originally was CONNECT_DONE (don't know why)
                                              // now each conn can arrive CONNECT_DONE only one time during first initialization.
                                              // maybe we need to free result here if exist..
          break;
        }
        /*
        if (conn->current_query_entry->index == query_counter - 1)
          gettimeofday(&t2,NULL);  // finish time
        */

        // TODO: can we assume mysql_errno(&conn->mysql) == 0 ? (because conn->err is zero).
        assert(mysql_errno(&conn->mysql) == 0);

        conn->result = mysql_use_result(&conn->mysql);
        if (conn->result) {
          conn->current_state = FETCH_ROW_START;
        } else {
          // end of query (no results)
          conn->current_state = QUERY_START;
          prev_query = conn->current_query_entry;
          __msa_connection_del_current_query(conn);
          prev_query->after_query_cb(prev_query, 0, mysql_errno(&conn->mysql)); // TODO: should pass somehow that there are no results?
          // TODO: should i do here something else?
        }
        break;

      case FETCH_ROW_START:
        status = mysql_fetch_row_start(&conn->row, conn->result);
        state_machine_continue = decide_next_state(status, conn, 
            FETCH_ROW_WAITING, FETCH_ROW_RESULT_READY);   
        break;

      case FETCH_ROW_WAITING:
        status = mysql_fetch_row_cont(&conn->row, conn->result, mysql_status(status, event));
        state_machine_continue = decide_next_state(status, conn, FETCH_ROW_WAITING, FETCH_ROW_RESULT_READY);   
        break;

      case FETCH_ROW_RESULT_READY: 
        if (conn->current_query_entry->stopping > 0) {
            prev_query = conn->current_query_entry;
            __msa_connection_del_current_query(conn);
            prev_query->after_query_cb(prev_query, MSA_EQUERYSTOP, mysql_errno(&conn->mysql));
            conn->current_state = QUERY_FREE_RESULT_START;
            conn->row = NULL; // TODO: is it ok or we have to free it? (assume it is freed on free results)
            break;
        }
        if (!conn->row){
          prev_query = conn->current_query_entry;
          __msa_connection_del_current_query(conn);
          if (mysql_errno(&conn->mysql)) {
            //printf("%d | Error: %s\n", conn->index, mysql_error(&conn->mysql));
            prev_query->after_query_cb(prev_query, MSA_EMYSQLERR | MSA_EQUERYFAIL, mysql_errno(&conn->mysql));
          } else {
            /* EOF - no more rows */
            prev_query->after_query_cb(prev_query, /*TODO: use right status */0, 0);
          }
          
          conn->current_state = QUERY_FREE_RESULT_START; 
          break;
        }
        
		    conn->current_query_entry->res_ready_cb(conn->current_query_entry, conn->result, conn->row);

        // print fields in the row
        /*
        printf("%d : %d  - ", conn->index, conn->current_query_entry->index);
        for (int i= 0; i < mysql_num_fields(conn->result); i++)
          printf("%s%s", (i ? "\t" : ""), (conn->row[i] ? conn->row[i] : "(null)"));
        printf ("\n");
        */
        conn->current_state = FETCH_ROW_START;
        break;

      case QUERY_FREE_RESULT_START:
        assert(conn->result != NULL);
        status = mysql_free_result_start(conn->result);
        state_machine_continue = decide_next_state(status, conn, QUERY_FREE_RESULT_WAITING, QUERY_FREE_RESULT_DONE);   
        break;

      case QUERY_FREE_RESULT_WAITING:
        assert(conn->result != NULL);
        status = mysql_free_result_cont(conn->result, mysql_status(status, event));
        state_machine_continue = decide_next_state(status, conn, QUERY_FREE_RESULT_WAITING, QUERY_FREE_RESULT_DONE);   
        break;

      case QUERY_FREE_RESULT_DONE:
        assert(conn->row == NULL);
        conn->result = NULL;
        conn->current_state = QUERY_START;
        break;

      case CLOSE_START:
        assert(conn->current_query_entry == NULL);
        assert(conn->result == NULL);
        assert(conn->row == NULL);

        // make conn not active.
        assert(__msa_conn_is_active(conn));
        msa_list_del(&conn->conns_list);
        conn->pool->nr_active_conns--;
        msa_list_add_tail(&conn->conns_list, &conn->pool->nonactive_conns_list_head);
        conn->pool->nr_nonactive_conns++;
        conn->is_active = 0;

        // TODO: check the number of active conns is ok.

        status = mysql_close_start(&conn->mysql);
        state_machine_continue = decide_next_state(status, conn, CLOSE_WAITING, CLOSE_DONE);   
        break;

      case CLOSE_WAITING: 
        status = mysql_close_cont(&conn->mysql, mysql_status(status, event));
        state_machine_continue = decide_next_state(status, conn, CLOSE_WAITING, CLOSE_DONE);   
        break;

      case CLOSE_DONE:
        __msa_pool_del_closed_connection(conn);
        state_machine_continue = 0;   
        break;

      default:
        abort(); // TODO: handle this case.
    }
  }
}


int msa_pool_init(msa_pool_t *pool, msa_connection_details_t* opts, uv_loop_t *loop) {
  int i;
  msa_connection_t *conn;
  int ret;

  assert(pool != NULL);

  pool->opts = opts;
  pool->loop = loop;

  opts->timeout = (opts->timeout > 0 ? opts->timeout : DEFAULT_TIMEOUT);
  opts->max_nr_successive_connection_fails = (opts->max_nr_successive_connection_fails > 0 
      ? opts->max_nr_successive_connection_fails : DEFAULT_MAX_NR_SUCCESSIVE_CONNECTION_FAILS);

  if (opts->initial_nr_conns < 1)
    opts->initial_nr_conns = 1;

  if (opts->min_nr_conns < 1)
    opts->min_nr_conns = 1;

  if (opts->max_nr_conns > 0 && opts->initial_nr_conns < opts->max_nr_conns)
    return -MSA_EINVAL;

  // TODO: check that `initial_nr_conns` is reasonable.

  MSA_INIT_LIST_HEAD(&pool->pending_queries_list_head);  // waiting for connections to be freed.
  pool->nr_pending_queries = 0;
  MSA_INIT_LIST_HEAD(&pool->active_queries_list_head);   // already assigned to a conn & being processed.
  pool->nr_active_queries = 0;

  MSA_INIT_LIST_HEAD(&pool->nonactive_conns_list_head);
  MSA_INIT_LIST_HEAD(&pool->active_conns_list_head);
  MSA_INIT_LIST_HEAD(&pool->free_conns_list_head);
  pool->nr_nonactive_conns = 0;
  pool->nr_active_conns = 0;
  pool->nr_free_conns = 0;

  pool->last_freed_conn_time = 0; // todo: use right time type
  pool->new_pending_queries = 0;  // being reset when a conn is freed. used to control num of opened conns.

  pool->nr_successive_connection_fails = 0;

  pool->closing = 0;
  pool->close_cb = NULL;

#ifdef MSA_USE_STATISTICS
  // todo: use right time type
  pool->avg_query_time = 0;
  for (i = 0; i < NR_QUERY_CATEGORIES_FOR_STATS; ++i) {
    pool->avg_query_time_by_type[i] = 0;
  }
#endif // MSA_USE_STATISTICS

  // open initial connections:
  for (i = 1; i <= pool->opts->initial_nr_conns; ++i) {
    conn = malloc(sizeof(msa_connection_t));
    if (conn == NULL) {
      msa_pool_close(pool, NULL);  // TODO: null is invalid. what to do here?
      return -MSA_EOUTOFMEM;
    }
    ret = __msa_connection_init(conn, pool, MSA_OCONN_INIT);
    if (ret != 0) {
      msa_pool_close(pool, NULL);  // TODO: null is invalid. what to do here?
      return ret;
    }
  }

  return 0;
}

int msa_pool_close(msa_pool_t *pool, msa_pool_close_cb close_cb) {
  list_t *pos, *n;
  msa_connection_t *conn;
  msa_query_t *query;

  assert(close_cb != NULL);

  if (pool->closing) return 0;

  pool->closing = 1;
  pool->close_cb = close_cb;

  msa_list_for_each_safe(pos, n, &pool->active_conns_list_head) {
      conn = msa_list_entry(pos, msa_connection_t, conns_list);
      __msa_conn_close(conn);
  }

  msa_list_for_each_safe(pos, n, &pool->pending_queries_list_head) {
      query = msa_list_entry(pos, msa_query_t, query_list);
      __msa_pending_query_close(query);
  }

  return 0;
}

int msa_pool_nr_pending_queries(msa_pool_t *pool) {
  assert(pool != NULL);
  return pool->nr_pending_queries;
}

int msa_pool_nr_active_connections(msa_pool_t *pool) {
  assert(pool != NULL);
  return pool->nr_active_conns;
}


int msa_query_init(msa_query_t* query, const char *strQuery, msa_query_res_ready_cb res_ready_cb, msa_after_query_cb after_query_cb, void* context) {
    assert(query != NULL && strQuery != NULL);

    // TODO: here we malloc for the query.
    //        we might want it to be the user's responsibility.
    /*query->query = malloc(strlen(strQuery) + 1);
    if (query->query == NULL) {
        return -MSA_EOUTOFMEM;
    }
    strcpy(query->query, strQuery);*/

    query->query = strQuery;
    query->context = context;
    query->res_ready_cb = res_ready_cb;
    query->after_query_cb = after_query_cb;

    MSA_INIT_LIST_HEAD(&query->query_list);
    query->conn = NULL; // will remain null until a free conn is available, and set to process this query.
    query->pool = NULL;
    query->stopping = 0;

#ifdef MSA_USE_STATISTICS
    stat_category = 0;
#endif // MSA_USE_STATISTICS

    return 0;
}

int msa_query_start(msa_pool_t* pool, msa_query_t* query) {
    assert(pool != NULL && query != NULL);

    if (pool->closing) {
      return -MSA_EPOOLCLOSING;
    }

    if (query->stopping == 1) {
      return -MSA_EQUERYSTOPPING;
    }

    query->stopping = 0;
    query->pool = pool;
    msa_list_add_tail(&query->query_list, &pool->pending_queries_list_head);
    pool->nr_pending_queries++;

    // if there are free conns - set one of them to process this query.
    __msa_pool_try_wake_free_conn(pool);

    // At this stage, is we found a free conn (means the wakeup successed),
    //  not this free conn is processing a query took from the pending list.
    // If there was a free conn, we might assume that the pending list was empty before inserting this query.
    //  so now our query is being processed by a free conn in this case.

    // the ratio #queries : #conns has been changed, hence we need to check whether to open new conns.
    __msa_pool_try_to_open_conns(pool);

    return 0;
}

/**
  The after_cb will be called with `MSA_EQUERYSTOP` status when the query will be stopped.
  Exept this call, callbacks won't be called after calling msa_query_stop().
  If the query is still pending (not assigned with a conn), it will be stopped immediatly.
  Otherwise, it will be stopped after the current stage of its conn state-machine.
  Post stopping, conn's state machine will be cleaned from results.
**/
int msa_query_stop(msa_query_t* query) {

  if (query->stopping > 0) {  // already stopping / stopped
    return -MSA_EQUERYSTOPPING;
  }

  query->stopping = 1;

  // pending query (conn is NULL)
  if (__msa_query_is_pending(query)) {
    __msa_pending_query_close(query);
    query->stopping = 2;  // stopped. TODO: decide what values will be in stopping field.
    query->after_query_cb(query, MSA_EQUERYSTOP, 0);
    return 0;
  }

  assert(query->conn != NULL);
  assert(query == query->conn->current_query_entry);
  assert(__msa_conn_is_active(query->conn));
  assert(query->conn->current_state == QUERY_WAITING || query->conn->current_state == FETCH_ROW_WAITING);

  /*
    The next stages in the state machine will detect that stopping == 1 and will stop the query.
    The after_cb will be called with `MSA_EQUERYSTOP` status.
  */

  return 0;
}

const char* msa_query_get(msa_query_t* query) {
    assert(query != NULL);
    return query->query;
}

void* msa_query_get_context(msa_query_t* query) {
    assert(query != NULL);
    return query->context;
}

void msa_query_set_context(msa_query_t* query, void* context) {
    assert(query != NULL);
    query->context = context;
}

/*
  Notice - `timeout_poll_handle` field is NOT initialized after this! we just set its data here.
*/
static int __msa_connection_init(msa_connection_t *conn, msa_pool_t *pool, int openning_reason) {
  uint timeout;

  assert(conn != NULL);
  assert(pool != NULL);

  timeout = pool->opts->timeout;

  mysql_init(&conn->mysql);
  mysql_options(&conn->mysql, MYSQL_OPT_NONBLOCK, NULL);
  mysql_options(&conn->mysql, MYSQL_READ_DEFAULT_GROUP, "async_queries");

  // set timeouts to 300 microseconds 
  mysql_options(&conn->mysql, MYSQL_OPT_READ_TIMEOUT, &timeout);
  mysql_options(&conn->mysql, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
  mysql_options(&conn->mysql, MYSQL_OPT_WRITE_TIMEOUT, &timeout);

  conn->pool = pool;
  conn->current_state = CONNECT_START;
  conn->timeout_poll_handle.data = conn;
  conn->openning_reason = openning_reason;
  conn->ret = NULL;
  conn->result = NULL;
  conn->row = NULL;
  conn->err = 0;
  conn->is_active = 0;
  conn->closing = 0;
  
  msa_list_add_tail(&conn->conns_list, &pool->nonactive_conns_list_head);
  pool->nr_nonactive_conns++;

  MSA_INIT_LIST_HEAD(&conn->free_conns_list);
  conn->current_query_entry = NULL;

  __msa_connection_state_machine_handler(&conn->timeout_poll_handle, -1, -1);

  return 0;
}

static int __msa_connection_del_current_query(msa_connection_t *conn) {
  assert(conn != NULL && conn->current_query_entry != NULL);
  conn->current_query_entry->conn = NULL;
  msa_list_del(&conn->current_query_entry->query_list);
  MSA_INIT_LIST_HEAD(&conn->current_query_entry->query_list);
  conn->current_query_entry = NULL;
  
  return 0;
}

static int __msa_pool_try_wake_free_conn(msa_pool_t *pool) {
    list_t* free_conn_node;
    msa_connection_t* conn;

    if (pool->nr_pending_queries < 1) {
        return -MSA_ENOPENDINGQUERIES;
    }

    if (msa_list_empty(&pool->free_conns_list_head)) {
        return -MSA_ENOFREECONNS;
    }

    free_conn_node = pool->free_conns_list_head.next;

    // a free conn is found, hence we assume there is exactly one pending query.
    assert(pool->nr_pending_queries == 1);

    conn = msa_list_entry(free_conn_node, msa_connection_t, free_conns_list);
    assert(conn->current_query_entry == NULL);
    assert(conn->current_state == QUERY_START);
    assert(__msa_conn_is_active(conn));

    __msa_conn_wake_up(conn);

    return 0;
}

static void __msa_conn_poll_handle_close_cb(uv_handle_t* handle) {
  uv_timeout_poll_t *timeout_poll_handle = (uv_timeout_poll_t*)handle;
  msa_connection_t *conn = (msa_connection_t*)timeout_poll_handle->data;
  __msa_pool_del_closed_connection_wo_poll_handle(conn);
}

/* without poll handle */
static inline int __msa_pool_del_closed_connection_wo_poll_handle(msa_connection_t *conn) {
  msa_pool_t *pool;

  // the connection is already closed.
  assert(!__msa_conn_is_active(conn));
  assert(conn->current_query_entry == NULL);
  assert(conn->result == NULL);
  assert(conn->row == NULL);
  assert(msa_list_empty(&conn->free_conns_list));

  assert(conn != NULL);
  pool = conn->pool;
  assert(pool != NULL);

  assert(!__msa_conn_is_active(conn));
  msa_list_del(&conn->conns_list);
  pool->nr_nonactive_conns--;

  free(conn);

  // last connection in a closing pool.
  if (pool->closing && __msa_pool_nr_conns(pool) == 0) {
      pool->close_cb(pool);
      // TODO: do we need to evict things from pool here?
  }
}

static inline int __msa_pool_del_closed_connection(msa_connection_t *conn) {
    assert(conn != NULL);
    assert(conn->pool != NULL);

    // the connection is already closed.
    assert(!__msa_conn_is_active(conn));
    assert(conn->current_query_entry == NULL);
    assert(conn->result == NULL);  // TODO: verify that in each call to this func it is the state..
    assert(conn->row == NULL);
    assert(msa_list_empty(&conn->free_conns_list));

    // remove from pool & free `conn` memory only AFTER timeout_poll_handle closes.
    // we assume the handle is already initialized in this stage.
    uv_timeout_poll_close(&conn->timeout_poll_handle, __msa_conn_poll_handle_close_cb);

    return 0;
}

/**
  It will be closed after the current query serving is done.
  The current query processing by this conn, if there any, won't be distrupted.
**/
static int __msa_conn_close(msa_connection_t* conn) {
    if (conn->closing) return 0;

    conn->closing = 1; // TODO: do we want to store a closing reason here? (to give another last chance before closing in some cases)

    // TODO: what to do if it is not connected yet? now we let it connect and only then close.

    // if the conn is free (hence sleeping), set the state to CLOSE_START and wake up the conn (start the state machine).
    if (__msa_conn_is_free(conn)) {
        assert(conn->current_state == QUERY_START);
        assert(conn->current_query_entry == NULL);
        conn->current_state = CLOSE_START;

        __msa_conn_wake_up(conn);
    }

    return 0;
}

/* caller should define the current_state before. */
static inline int __msa_conn_wake_up(msa_connection_t *conn) {
    assert(conn != NULL);
    assert(__msa_conn_is_free(conn));

    msa_list_del(&conn->free_conns_list);
    conn->pool->nr_free_conns--;
    MSA_INIT_LIST_HEAD(&conn->free_conns_list);

    // start the state machine handle of the conn.
    __msa_connection_state_machine_handler(&conn->timeout_poll_handle, 0, 0);

    return 0;
}

static inline int __msa_pending_query_close(msa_query_t *query) {
    assert(query != NULL);
    assert(query->conn == NULL);

    // the query is not assigned to a conn yet. it's in the pending list. we have to take it out of there.
    msa_list_del(&query->query_list);
    MSA_INIT_LIST_HEAD(&query->query_list);
    query->pool->nr_pending_queries--;

    //TODO: load balancing the connections of this pool, because a query was removed.

    // call the after_cb of the query.
    query->after_query_cb(query, MSA_EQUERYSTOP, 0);
    
    return 0;
}
