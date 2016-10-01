struct msa_query_s {
	// public fields:
	const char *query;
	void* context;
	msa_query_res_ready_cb res_ready_cb;
	msa_after_query_cb after_query_cb;

	// private fields:
	list_t query_list;
	msa_connection_t *conn; // TODO: maybe we won't need it.

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

	list_t active_conns_list;
	list_t free_conns_list;

  int openning_reason; // should be used for handling failures.

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

	list_t active_conns_list_head;
	list_t free_conns_list_head;
	size_t nr_active_conns;
	size_t nr_free_conns;

	unsigned long last_freed_conn_time; // todo: use right time type
	size_t new_pending_queries;  // being reset when a conn is freed. used to control num of opened conns.

  int nr_successive_connection_fails;

#ifdef MSA_USE_STATISTICS
	// todo: use right time type
	unsigned long avg_query_time;
	unsigned long avg_query_time_by_type[NR_QUERY_CATEGORIES_FOR_STATS];
#endif // MSA_USE_STATISTICS
};
