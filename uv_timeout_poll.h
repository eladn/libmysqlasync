#ifndef UV_TIMEOUT_POLL_H
#define UV_TIMEOUT_POLL_H

#include <uv.h>

enum uv_timeout_poll_init_flags {
	UV_USE_POLL = 1,
	UV_USE_TIMEOUT = 2,
	UV_STOP_HANDLES_EACH_EVENT = 4,
};

struct uv_timeout_poll_s;
typedef struct uv_timeout_poll_s uv_timeout_poll_t;
typedef void (*uv_timeout_poll_cb)(uv_timeout_poll_t* handle, int status, int events);

struct uv_timeout_poll_s {
	uv_poll_t poll;
	uv_timer_t timeout;
	int init_flags;
	uv_timeout_poll_cb cb;
	void* data;
	uv_close_cb close_cb;
};


int uv_timeout_poll_init(uv_loop_t* loop, uv_timeout_poll_t* handle, int fd, int flags);

int uv_timeout_poll_start(uv_timeout_poll_t* handle, int events, uv_timeout_poll_cb cb, uint64_t timeout);

int uv_timeout_poll_stop(uv_timeout_poll_t* handle);

int uv_timeout_poll_close(uv_timeout_poll_t* handle, uv_close_cb close_cb);

#endif // UV_TIMEOUT_POLL_H
