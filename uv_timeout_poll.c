#include "uv_timeout_poll.h"
#include <stdint.h>

#define get_timeout_poll_from_poll(poll) ((uv_timeout_poll_t*)((char*)poll - (uintptr_t)(&(((uv_timeout_poll_t*)0)->poll))))
#define get_timeout_poll_from_timer(timer) ((uv_timeout_poll_t*)((char*)timer - (uintptr_t)(&(((uv_timeout_poll_t*)0)->timeout))))
#define get_poll_from_timer(timer) (&(get_timeout_poll_from_timer(timer)->poll))
#define get_timer_from_poll(poll) (&(get_timeout_poll_from_poll(poll)->timeout))

static void __uv_timeout_poll_close_cb(uv_handle_t* handle) {

}

static void uv_timeout_poll_timer_cb(uv_timer_t* timeout) {
	uv_timeout_poll_t* handle = get_timeout_poll_from_timer(timeout);

	if (handle->init_flags & UV_USE_POLL) {
		uv_poll_stop(&handle->poll);
	}
	// timer has no repeat anyway so it wont triggered again.
	/*if (handle->init_flags & UV_STOP_HANDLES_EACH_EVENT)
		uv_timer_stop(timeout);*/
	(handle->cb)(handle, UV_ETIMEDOUT, 0);
}

static void uv_timeout_poll_poll_cb(uv_poll_t* poll, int status, int event) {
	uv_timeout_poll_t* handle = get_timeout_poll_from_poll(poll);

	if (handle->init_flags & UV_USE_TIMEOUT) {
		uv_timer_stop(&handle->timeout);
	}
	if (handle->init_flags & UV_STOP_HANDLES_EACH_EVENT)
		uv_poll_stop(poll);
	(handle->cb)(handle, status, event);
}

int uv_timeout_poll_init(uv_loop_t* loop, uv_timeout_poll_t* handle, int fd, int flags) {
	int ret;
	handle->init_flags = 0;
	if (fd < 0) {
		/* NOT MATTERS ANYMORE - NOW WE USE DEDICATED FLAGS FIELD */
		// if -1 passed as fd the `uv_poll_init` won't succeed, we need to manually reset handle flags
		// so we can use uv_is_active(poll) later on.
		//memset(&handle->poll, 0);
	} else {
		ret = uv_poll_init(loop, &handle->poll, fd);
		if (ret < 0) {
			return ret;
		}
		handle->init_flags |= UV_USE_POLL;
	}
	
	ret = uv_timer_init(loop, &handle->timeout);

	handle->init_flags |= (flags & UV_STOP_HANDLES_EACH_EVENT);

	return ret;
}

// TODO: fix return value & handle errors
int uv_timeout_poll_start(uv_timeout_poll_t* handle, int events, uv_timeout_poll_cb cb, uint64_t timeout) {
	int ret = 0;
	handle->cb = cb;
	if (timeout != 0) {
		ret = uv_timer_start(&handle->timeout, uv_timeout_poll_timer_cb, timeout, 0);
		if (ret < 0) {
			return ret;
		}
		handle->init_flags |= UV_USE_TIMEOUT;
	}
	if (handle->init_flags & UV_USE_POLL) {
		ret = uv_poll_start(&handle->poll, events, uv_timeout_poll_poll_cb);
		if (ret < 0) {
			if (handle->init_flags & UV_USE_TIMEOUT) {
				uv_close((uv_handle_t*)(&handle->timeout), __uv_timeout_poll_close_cb);
			}
			return ret;
		}
	}
	
	return ret;
}

int uv_timeout_poll_stop(uv_timeout_poll_t* handle) {
	int ret = 0;

	if (handle->init_flags & UV_USE_POLL) {
		ret = uv_poll_stop(&handle->poll);
	}
	if (handle->init_flags & UV_USE_TIMEOUT) {
		uv_timer_stop(&handle->timeout);
	}

	return ret;
}

void uv_timeout_poll_close(uv_timeout_poll_t* handle) {
	if (handle->init_flags & UV_USE_POLL) {
		uv_close((uv_handle_t*)(&handle->poll), __uv_timeout_poll_close_cb);
	}
	if (handle->init_flags & UV_USE_TIMEOUT) {
		uv_close((uv_handle_t*)(&handle->timeout), __uv_timeout_poll_close_cb);
	}
}
