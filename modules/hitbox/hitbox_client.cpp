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

#include "hitbox_backend.h"

#include "core/crypto/crypto.h"
#include "core/io/http_client.h"
#include "core/io/json.h"
#include "core/os/os.h"

static const char *HITBOX_API_PATH = "/v1/messages";
static const uint64_t HITBOX_SERVER_START_TIMEOUT_MSEC = 90000;

void HitboxClient::_thread_func(void *p_userdata) {
	static_cast<HitboxClient *>(p_userdata)->_run();
}

void HitboxClient::_emit(const Dictionary &p_event) {
	if (event_callback.is_valid()) {
		event_callback.call_deferred(p_event);
	}
}

void HitboxClient::_emit_info(const String &p_message) {
	Dictionary e;
	e["type"] = "info";
	e["message"] = p_message;
	_emit(e);
}

void HitboxClient::_emit_error(int p_status, const String &p_message) {
	Dictionary e;
	e["type"] = "error";
	e["status"] = p_status;
	e["message"] = p_message;
	_emit(e);
}

void HitboxClient::_emit_done() {
	Dictionary d;
	d["type"] = "done";
	_emit(d);
}

Error HitboxClient::start(const Request &p_request, const Callable &p_on_event) {
	ERR_FAIL_COND_V_MSG(thread.is_started(), ERR_BUSY, "HitboxClient: a request is already running; call finish() first.");
	request = p_request;
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

int HitboxClient::_connect(const Ref<HTTPClient> &p_http, const String &p_host, int p_port, bool p_tls) {
	const String scheme_host = (p_tls ? "https://" : "http://") + p_host;
	if (p_http->connect_to_host(scheme_host, p_port, p_tls ? TLSOptions::client() : Ref<TLSOptions>()) != OK) {
		return HTTPClient::STATUS_CANT_CONNECT;
	}
	while (p_http->get_status() == HTTPClient::STATUS_CONNECTING || p_http->get_status() == HTTPClient::STATUS_RESOLVING) {
		if (cancel_flag.is_set()) {
			p_http->close();
			return HTTPClient::STATUS_DISCONNECTED;
		}
		p_http->poll();
		OS::get_singleton()->delay_usec(2000);
	}
	return p_http->get_status();
}

bool HitboxClient::_start_server(const String &p_host, int p_port, bool p_tls) {
	_emit_info(vformat("yagami is not running; starting it with %s...", request.autostart_via));

	List<String> args;
	args.push_back("-c");
	args.push_back(request.autostart_script);
	String output;
	int exit_code = -1;
	Error err = OS::get_singleton()->execute("/bin/sh", args, &output, &exit_code, true);
	if (err != OK || exit_code != 0) {
		_emit_error(0, vformat("Could not start yagami (exit %d): %s", exit_code, output.strip_edges().right(600)));
		return false;
	}

	const uint64_t deadline = OS::get_singleton()->get_ticks_msec() + HITBOX_SERVER_START_TIMEOUT_MSEC;
	while (OS::get_singleton()->get_ticks_msec() < deadline) {
		if (cancel_flag.is_set()) {
			return false;
		}
		Ref<HTTPClient> probe = Ref<HTTPClient>(HTTPClient::create());
		probe->set_blocking_mode(true);
		const int status = _connect(probe, p_host, p_port, p_tls);
		probe->close();
		if (status == HTTPClient::STATUS_CONNECTED) {
			if (request.api_key.is_empty() && !request.yagami_config_path.is_empty()) {
				String url;
				String key;
				HitboxBackend::read_yagami_config(request.yagami_config_path, url, key);
				request.api_key = key;
			}
			_emit_info("yagami is up.");
			return true;
		}
		OS::get_singleton()->delay_usec(500000);
	}
	_emit_error(0, vformat("Started yagami, but nothing answered at %s within %d seconds. Check ~/.config/yagami/yagami.log.", request.base_url, (int)(HITBOX_SERVER_START_TIMEOUT_MSEC / 1000)));
	return false;
}

void HitboxClient::_run() {
	// Split the base URL into scheme, host and port.
	String url = request.base_url.strip_edges();
	bool tls = true;
	if (url.begins_with("http://")) {
		tls = false;
		url = url.substr(7);
	} else if (url.begins_with("https://")) {
		url = url.substr(8);
	}
	String path_prefix;
	const int slash = url.find("/");
	if (slash != -1) {
		path_prefix = url.substr(slash);
		url = url.left(slash);
	}
	while (path_prefix.ends_with("/")) {
		path_prefix = path_prefix.left(-1);
	}
	String host = url;
	int port = -1;
	const int bracket = url.rfind("]");
	const int colon = url.rfind(":");
	if (colon != -1 && colon > bracket) {
		host = url.left(colon);
		port = url.substr(colon + 1).to_int();
	}
	if (host.is_empty()) {
		_emit_error(0, vformat("Invalid server URL: %s", request.base_url));
		return;
	}

	Ref<HTTPClient> http = Ref<HTTPClient>(HTTPClient::create());
	http->set_blocking_mode(true);
	http->set_read_chunk_size(16384);

	int status = _connect(http, host, port, tls);
	if (cancel_flag.is_set()) {
		_emit_done();
		return;
	}
	if (status != HTTPClient::STATUS_CONNECTED && !request.autostart_script.is_empty()) {
		http->close();
		if (!_start_server(host, port, tls)) {
			if (cancel_flag.is_set()) {
				_emit_done();
			}
			return;
		}
		http = Ref<HTTPClient>(HTTPClient::create());
		http->set_blocking_mode(true);
		http->set_read_chunk_size(16384);
		status = _connect(http, host, port, tls);
	}
	if (cancel_flag.is_set()) {
		_emit_done();
		return;
	}
	if (status != HTTPClient::STATUS_CONNECTED) {
		_emit_error(0, vformat("Could not connect to %s (status %d).", request.base_url, status));
		return;
	}
	if (request.api_key.is_empty()) {
		_emit_error(0, "No API key for " + request.base_url + ".");
		return;
	}

	Vector<String> headers;
	headers.push_back("Content-Type: application/json");
	headers.push_back("Accept: text/event-stream");
	headers.push_back("x-api-key: " + request.api_key);
	headers.push_back("anthropic-version: 2023-06-01");
	for (const String &h : request.extra_headers) {
		headers.push_back(h);
	}

	const CharString body = request.body_json.utf8();
	Error err = http->request(HTTPClient::METHOD_POST, path_prefix + HITBOX_API_PATH, headers, (const uint8_t *)body.get_data(), body.length());
	if (err != OK) {
		_emit_error(0, vformat("Could not send request (error %d).", (int)err));
		return;
	}

	while (http->get_status() == HTTPClient::STATUS_REQUESTING) {
		if (cancel_flag.is_set()) {
			http->close();
			_emit_done();
			return;
		}
		http->poll();
		OS::get_singleton()->delay_usec(2000);
	}

	if (!http->has_response()) {
		_emit_error(0, vformat("No response from %s (status %d).", request.base_url, (int)http->get_status()));
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

	if (code != 200 && !cancel_flag.is_set()) {
		String message = vformat("Request failed with HTTP %d.", code);
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

	_emit_done();
}
