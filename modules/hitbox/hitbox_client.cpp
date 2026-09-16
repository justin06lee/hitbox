/**************************************************************************/
/*  hitbox_client.cpp                                                     */
/**************************************************************************/
/*                          This file is part of:                         */
/*                                HITBOX                                  */
/*                 https://github.com/justin06lee/hitbox                  */
/**************************************************************************/
/* Hitbox is a fork of Godot Engine. Godot Engine is Copyright (c)        */
/* 2014-present Godot Engine contributors, Copyright (c) 2007-2014 Juan   */
/* Linietsky, Ariel Manzur. Licensed under the MIT license.               */
/**************************************************************************/

#include "hitbox_client.h"

#include "core/crypto/crypto.h"
#include "core/io/http_client.h"
#include "core/io/json.h"
#include "core/os/os.h"

static const char *HITBOX_API_HOST = "https://api.anthropic.com";
static const char *HITBOX_API_PATH = "/v1/messages";

void HitboxClient::_thread_func(void *p_userdata) {
	static_cast<HitboxClient *>(p_userdata)->_run();
}

void HitboxClient::_emit(const Dictionary &p_event) {
	if (event_callback.is_valid()) {
		event_callback.call_deferred(p_event);
	}
}

void HitboxClient::_emit_error(int p_status, const String &p_message) {
	Dictionary e;
	e["type"] = "error";
	e["status"] = p_status;
	e["message"] = p_message;
	_emit(e);
}

Error HitboxClient::start(const String &p_api_key, const String &p_body_json, const Vector<String> &p_extra_headers, const Callable &p_on_event) {
	ERR_FAIL_COND_V_MSG(thread.is_started(), ERR_BUSY, "HitboxClient: a request is already running; call finish() first.");
	api_key = p_api_key;
	body_json = p_body_json;
	extra_headers = p_extra_headers;
	event_callback = p_on_event;
	cancel_flag.clear();
	thread.start(_thread_func, this);
	return OK;
}

void HitboxClient::cancel() {
	cancel_flag.set();
}

void HitboxClient::finish() {
	if (thread.is_started()) {
		thread.wait_to_finish();
	}
}

bool HitboxClient::is_running() const {
	return thread.is_started();
}

HitboxClient::~HitboxClient() {
	cancel();
	finish();
}

void HitboxClient::_run() {
	Ref<HTTPClient> http = Ref<HTTPClient>(HTTPClient::create());
	http->set_blocking_mode(true);
	http->set_read_chunk_size(16384);

	Error err = http->connect_to_host(HITBOX_API_HOST, -1, TLSOptions::client());
	if (err != OK) {
		_emit_error(0, vformat("Could not start connection to %s (error %d).", HITBOX_API_HOST, (int)err));
		return;
	}

	while (http->get_status() == HTTPClient::STATUS_CONNECTING || http->get_status() == HTTPClient::STATUS_RESOLVING) {
		if (cancel_flag.is_set()) {
			http->close();
			Dictionary d;
			d["type"] = "done";
			_emit(d);
			return;
		}
		http->poll();
		OS::get_singleton()->delay_usec(2000);
	}

	if (http->get_status() != HTTPClient::STATUS_CONNECTED) {
		_emit_error(0, vformat("Could not connect to %s (status %d). Check your network connection.", HITBOX_API_HOST, (int)http->get_status()));
		return;
	}

	Vector<String> headers;
	headers.push_back("Content-Type: application/json");
	headers.push_back("Accept: text/event-stream");
	headers.push_back("x-api-key: " + api_key);
	headers.push_back("anthropic-version: 2023-06-01");
	for (const String &h : extra_headers) {
		headers.push_back(h);
	}

	CharString body = body_json.utf8();
	err = http->request(HTTPClient::METHOD_POST, HITBOX_API_PATH, headers, (const uint8_t *)body.get_data(), body.length());
	if (err != OK) {
		_emit_error(0, vformat("Could not send request (error %d).", (int)err));
		return;
	}

	while (http->get_status() == HTTPClient::STATUS_REQUESTING) {
		if (cancel_flag.is_set()) {
			http->close();
			Dictionary d;
			d["type"] = "done";
			_emit(d);
			return;
		}
		http->poll();
		OS::get_singleton()->delay_usec(2000);
	}

	if (!http->has_response()) {
		_emit_error(0, vformat("No response from the API (status %d).", (int)http->get_status()));
		return;
	}

	const int code = http->get_response_code();
	Vector<uint8_t> pending; // Bytes of the current, not yet newline-terminated SSE line.
	Vector<uint8_t> raw_body; // Whole body, only kept for non-200 responses.

	while (http->get_status() == HTTPClient::STATUS_BODY) {
		if (cancel_flag.is_set()) {
			http->close();
			break;
		}
		http->poll();
		PackedByteArray chunk = http->read_response_body_chunk();
		if (chunk.is_empty()) {
			OS::get_singleton()->delay_usec(1000);
			continue;
		}

		if (code != 200) {
			raw_body.append_array(chunk);
			continue;
		}

		const uint8_t *src = chunk.ptr();
		const int n = chunk.size();
		for (int i = 0; i < n; i++) {
			const uint8_t c = src[i];
			if (c != '\n') {
				pending.push_back(c);
				continue;
			}
			// Complete line.
			int len = pending.size();
			if (len > 0 && pending[len - 1] == '\r') {
				len--;
			}
			if (len > 0) {
				String line = String::utf8((const char *)pending.ptr(), len);
				if (line.begins_with("data:")) {
					String data = line.substr(5).strip_edges();
					if (!data.is_empty()) {
						JSON json;
						if (json.parse(data) == OK) {
							Dictionary e;
							e["type"] = "sse";
							e["event"] = json.get_data();
							_emit(e);
						}
					}
				}
			}
			pending.clear();
		}
	}

	if (code != 200) {
		String message = vformat("API request failed with HTTP %d.", code);
		if (!raw_body.is_empty()) {
			String text = String::utf8((const char *)raw_body.ptr(), raw_body.size());
			JSON json;
			if (json.parse(text) == OK && json.get_data().get_type() == Variant::DICTIONARY) {
				Dictionary d = json.get_data();
				Dictionary error = d.get("error", Dictionary());
				String m = error.get("message", "");
				if (!m.is_empty()) {
					message = vformat("HTTP %d: %s", code, m);
				}
			} else {
				message += " " + text.left(400);
			}
		}
		_emit_error(code, message);
		return;
	}

	Dictionary d;
	d["type"] = "done";
	_emit(d);
}
