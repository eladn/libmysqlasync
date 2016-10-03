struct msa_query_s {
	// public fields:
	const char *query;
	void* context;
	msa_query_res_ready_cb res_ready_cb;
	msa_after_query_cb after_query_cb;

	// private fields:
	list_t query_list;
	msa_connection_t *conn;
	msa_pool_t *pool; // used for the period that the query is pending, and has no conn yet.

#ifdef MSA_USE_STATISTICS
	query_stats_category_t stat_category;
#endif // MSA_USE_STATISTICS
};

struct msa_connection_s {
	msa_pool_t* pool;
	int current_state;                   // State machine current state

	MYSQL mysql;                        
	MYSQL *ret;
	MYSQL_RES *result;
	MYSQL_ROW row;
	
	uv_timeout_poll_t timeout_poll_handle;

	msa_query_t *current_query_entry;
	int err;
	//int index;  // TODO: do we need this?

	list_t conns_list; 	// member of active_conn_list or nonactive_conn_list
	list_t free_conns_list;

	int is_active;

  	int openning_reason; // should be used for handling failures.

  	int closing;  // 0 - no closing; non-zero - the closing reason. it will be closed after the current query serving is done.

#ifdef MSA_USE_STATISTICS
	// todo: use right time type
	unsigned long current_query_start_time;
#endif // MSA_USE_STATISTICS
};

struct msa_pool_s {
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
