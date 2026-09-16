/**************************************************************************/
/*  hitbox_client.h                                                       */
/**************************************************************************/
/*                          This file is part of:                         */
/*                                HITBOX                                  */
/*                 https://github.com/justin06lee/hitbox                  */
/**************************************************************************/
/* Hitbox is a fork of Godot Engine. Godot Engine is Copyright (c)        */
/* 2014-present Godot Engine contributors, Copyright (c) 2007-2014 Juan   */
/* Linietsky, Ariel Manzur. Licensed under the MIT license.               */
/**************************************************************************/

#pragma once

#include "core/object/ref_counted.h"
#include "core/os/thread.h"
#include "core/templates/safe_refcount.h"
#include "core/variant/callable.h"

// Streams one Anthropic Messages API request on a worker thread.
//
// Every server-sent event is decoded from JSON and handed to the callback on
// the main thread (via call_deferred) as a Dictionary:
//   { "type": "sse",   "event": <the decoded event> }
//   { "type": "error", "status": <http status or 0>, "message": <text> }
//   { "type": "done" }
// "done" or "error" is always the last event of a request; the owner must
// then call finish() to join the thread before starting another request.
class HitboxClient : public RefCounted {
	GDCLASS(HitboxClient, RefCounted);

	Thread thread;
	SafeFlag cancel_flag;

	String api_key;
	String body_json;
	Vector<String> extra_headers;
	Callable event_callback;

	static void _thread_func(void *p_userdata);
	void _run();
	void _emit(const Dictionary &p_event);
	void _emit_error(int p_status, const String &p_message);

protected:
	static void _bind_methods() {}

public:
	Error start(const String &p_api_key, const String &p_body_json, const Vector<String> &p_extra_headers, const Callable &p_on_event);
	void cancel();
	void finish();
	bool is_running() const;

	~HitboxClient();
};
