#include "msa.h"
#include <assert.h>

typedef struct msa_dal_request_s {
	msa_after_query_cb after_cb;

	char* query_str;
	msa_query_t query;
	msa_dal_t* dal;
} msa_dal_request_t;

#define get_req_from_query(query) ((msa_dal_request_t*)((char*)query - (uintptr_t)(&(((msa_dal_request_t*)0)->query))))
#define get_dal_from_pool(pool) ((msa_dal_t*)((char*)pool - (uintptr_t)(&(((msa_dal_t*)0)->pool))))

static inline msa_dal_request_t* __msa_dal_allocate_request(msa_dal_t *dal, const char* query_str);
static inline void __msa_dal_free_request(msa_dal_t *dal, msa_dal_request_t* req);
static void __msa_dal_after_cb(msa_query_t *query, int status, int mysql_status);
static inline void* __msa_strcpy(const char* str);
static void __msa_dal_pool_close_cb(msa_pool_t *pool);

int msa_dal_init(msa_dal_t* dal, uv_loop_t *loop, const char* host, const char* user, const char* password, const char* db) {
	int res;

	assert(dal != NULL && loop != NULL);
	memset(&dal->opts, 0, sizeof(dal->opts));
	dal->opts.db = __msa_strcpy(db);
	dal->opts.user = __msa_strcpy(user);
	dal->opts.password = __msa_strcpy(password);
	dal->opts.host = __msa_strcpy(host);
	if (!dal->opts.db || !dal->opts.user || !dal->opts.password || !dal->opts.host) {
		return -MSA_EOUTOFMEM;
	}

	//dal->opts.error_cb = pool_error_cb; // TODO: ??

	/* The following will be zero because of memset(..) */
	/*dal->opts.initial_nr_conns = 0;
	dal->opts.min_nr_conns = 0;
	dal->opts.max_nr_conns = 0;
	dal->opts.timeout = 0;
	dal->opts.max_nr_successive_connection_fails = 0;*/

	res = msa_pool_init(&dal->pool, &dal->opts, loop);
	if (res != 0) {
		free(dal->opts.db);
		free(dal->opts.user);
		free(dal->opts.password);
		free(dal->opts.host);
		return res;
	}

	return 0;
}


int msa_dal_execute(msa_dal_t *dal, const char* query_str, msa_query_res_ready_cb res_ready_cb, msa_after_query_cb after_cb, void* context) {
	int res;
	msa_dal_request_t* req = __msa_dal_allocate_request(dal, query_str);
	if (!req) {
		return -MSA_EOUTOFMEM;
	}
	req->dal = dal;
	req->after_cb = after_cb;

	res = msa_query_init(&req->query, req->query_str, res_ready_cb, __msa_dal_after_cb);
	if (!res) {
		__msa_dal_free_request(dal, req);
		return res;
	}

	req->query.context = context;
	res = msa_query_start(&dal->pool, &req->query);
	if (!res) {
		__msa_dal_free_request(dal, req);
		return res;
	}

	return 0;
}

int msa_dal_close(msa_dal_t *dal, msa_dal_close_cb close_cb) {
	if (dal->pool.closing) {
		return -MSA_ECLOSING;
	}

	dal->close_cb = close_cb;
	return msa_pool_close(&dal->pool, __msa_dal_pool_close_cb);
}

/* the 2 following alloc&free functions are actualy the core logics of the dal */
/* in future we might have our own block already allocated (dynamiclly growing) */
static inline msa_dal_request_t* __msa_dal_allocate_request(msa_dal_t *dal, const char* query_str) {
	/* todo: implement this core function smartly */

	msa_dal_request_t *req;
	char* str = malloc(strlen(query_str)+1); /* TODO: maybe use an already allocated buffer */
	if (!str) {
		return NULL;
	}
	strcpy(str, query_str);
	req = malloc(sizeof(msa_dal_request_t)); /* TODO: maybe use an already allocated buffer */
	if (!req) {
		free(str);
		return NULL;
	}
	req->query_str = str;
	return req;
}

static inline void __msa_dal_free_request(msa_dal_t *dal, msa_dal_request_t* req) {
	assert(req != NULL);
	/* todo: implement this core function smartly */
	free(req->query_str);
	free(req); /* assume the given request in not used anymore */
}


static void __msa_dal_after_cb(msa_query_t *query, int status, int mysql_status) {
	msa_dal_request_t *req = get_req_from_query(query);
	assert(query != NULL);
	
	req->after_cb(query, status, mysql_status);

	__msa_dal_free_request(req->dal, req);
}

static inline void* __msa_strcpy(const char* str) {
	char* cpy;
	if (str == NULL) return NULL;
	cpy = malloc(strlen(str)+1);
	strcpy(cpy, str);
	return cpy;
}

static void __msa_dal_pool_close_cb(msa_pool_t *pool) {
	msa_dal_t *dal = get_dal_from_pool(pool);

	dal->close_cb(dal);

	free(dal->opts.db);
	free(dal->opts.user);
	free(dal->opts.password);
	free(dal->opts.host);

	/**
	TODO: free reserved request allocations.
	**/
}
