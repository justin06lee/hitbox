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

class HTTPClient;

// Streams one Messages API request on a worker thread, against the Anthropic
// API or a compatible server such as yagami.
//
// Every server-sent event is decoded from JSON and handed to the callback on
// the main thread (via call_deferred) as a Dictionary:
//   { "type": "sse",   "event": <the decoded event> }
//   { "type": "info",  "message": <text> }            progress worth showing
//   { "type": "error", "status": <http status or 0>, "message": <text> }
//   { "type": "done" }
// "done" or "error" is always the last event of a request; the owner must
// then call finish() to join the thread before starting another request.
class HitboxClient : public RefCounted {
	GDCLASS(HitboxClient, RefCounted);

public:
	struct Request {
		// "https://api.anthropic.com" or "http://127.0.0.1:8787".
		String base_url;
		String api_key;
		String body_json;
		Vector<String> extra_headers;
		// When the server cannot be reached, run this shell script once to
		// start it, then wait for it to listen.
		String autostart_script;
		String autostart_via;
		// Re-read the API key from this yagami config.json after starting.
		String yagami_config_path;
	};

private:
	Thread thread;
	SafeFlag cancel_flag;
	Request request;
	Callable event_callback;

	static void _thread_func(void *p_userdata);
	void _run();
	void _emit(const Dictionary &p_event);
	void _emit_info(const String &p_message);
	void _emit_error(int p_status, const String &p_message);
	void _emit_done();

	// Connects and waits; returns the final status.
	int _connect(const Ref<HTTPClient> &p_http, const String &p_host, int p_port, bool p_tls);
	bool _start_server(const String &p_host, int p_port, bool p_tls);

protected:
	static void _bind_methods() {}

public:
	Error start(const Request &p_request, const Callable &p_on_event);
	void cancel();
	void finish();
	bool is_running() const;

	~HitboxClient();
};
