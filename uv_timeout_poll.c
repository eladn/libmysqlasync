#include "uv_timeout_poll.h"
#include <stdint.h>
#include <assert.h>
#include <uv.h>

#define get_timeout_poll_from_poll(poll) ((uv_timeout_poll_t*)((char*)poll - (uintptr_t)(&(((uv_timeout_poll_t*)0)->poll))))
#define get_timeout_poll_from_timer(timer) ((uv_timeout_poll_t*)((char*)timer - (uintptr_t)(&(((uv_timeout_poll_t*)0)->timeout))))
#define get_poll_from_timer(timer) (&(get_timeout_poll_from_timer(timer)->poll))
#define get_timer_from_poll(poll) (&(get_timeout_poll_from_poll(poll)->timeout))

static void __uv_timeout_poll_close_poll_cb(uv_handle_t* poll) {
	uv_timeout_poll_t* handle = get_timeout_poll_from_poll(poll);
	assert(handle->init_flags & UV_USE_POLL);

	handle->init_flags ^= ~UV_USE_POLL; // turn off this flag.
	
	if (handle->init_flags & UV_USE_TIMEOUT) {
		// __uv_timeout_poll_close_timer_cb() will be called.
		assert(uv_is_closing((uv_handle_t*)(&handle->timeout)));
		return;
	}

	handle->close_cb((uv_handle_t*)handle);
}
static void __uv_timeout_poll_close_timer_cb(uv_handle_t* timeout) {
	uv_timeout_poll_t* handle = get_timeout_poll_from_timer(timeout);
	assert(handle->init_flags & UV_USE_TIMEOUT);
	
	handle->init_flags ^= ~UV_USE_TIMEOUT; // turn off this flag.
	
	if (handle->init_flags & UV_USE_POLL) {
		// __uv_timeout_poll_close_poll_cb() will be called.
		assert(uv_is_closing((uv_handle_t*)(&handle->poll)));
		return;
	}

	handle->close_cb((uv_handle_t*)handle);
}

static void __uv_timeout_poll_timer_cb(uv_timer_t* timeout) {
	uv_timeout_poll_t* handle = get_timeout_poll_from_timer(timeout);

	if (handle->init_flags & UV_USE_POLL) {
		uv_poll_stop(&handle->poll);
	}
	// timer has no repeat anyway so it wont triggered again.
	/*if (handle->init_flags & UV_STOP_HANDLES_EACH_EVENT)
		uv_timer_stop(timeout);*/
	(handle->cb)(handle, UV_ETIMEDOUT, 0);
}

static void __uv_timeout_poll_poll_cb(uv_poll_t* poll, int status, int event) {
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

	if (cb == NULL) {
		return -EINVAL;
	}

	handle->cb = cb;

	if (handle->init_flags & UV_USE_POLL) {
		ret = uv_poll_start(&handle->poll, events, __uv_timeout_poll_poll_cb);
		if (ret < 0) {
			// At first, the timer was started before the poll so we had to close the timer here.
			// actually this was not a good practice, because the user might free the whole uv_timeout_poll_t
			// before the close_cb will be called to the timer.
			/*if (handle->init_flags & UV_USE_TIMEOUT) {
				uv_close((uv_handle_t*)(&handle->timeout), __uv_timeout_poll_close_cb);
			}*/
			return ret;
		}
	}

	if (timeout != 0) {
		ret = uv_timer_start(&handle->timeout, __uv_timeout_poll_timer_cb, timeout, 0);
		assert(ret == 0);  	// We cannot close the other handle now because the user
						   	// 	might free this memory before the actual closing will take place.
							// Last time checked, `uv_timer_start()` will fail if-and-only-if given cb param is NULL.
							//  so we should be fine.
		handle->init_flags |= UV_USE_TIMEOUT;
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

int uv_timeout_poll_close(uv_timeout_poll_t* handle, uv_close_cb close_cb) {
	assert(handle != NULL);
	assert(close_cb != NULL);

	handle->close_cb = close_cb;

	if (handle->init_flags & UV_USE_POLL) {
		uv_close((uv_handle_t*)(&handle->poll), __uv_timeout_poll_close_poll_cb);
	}
	if (handle->init_flags & UV_USE_TIMEOUT) {
		uv_close((uv_handle_t*)(&handle->timeout), __uv_timeout_poll_close_timer_cb);
	}
	if (handle->init_flags == 0) {
		close_cb((uv_handle_t*)handle);
	}

	return 0;
}
