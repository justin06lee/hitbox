/**************************************************************************/
/*  hitbox_mcp_server.cpp                                                 */
/**************************************************************************/
/*                          This file is part of:                         */
/*                                HITBOX                                  */
/*                 https://github.com/justin06lee/hitbox                  */
/**************************************************************************/
/* Hitbox is a fork of Godot Engine. Godot Engine is Copyright (c)        */
/* 2014-present Godot Engine contributors, Copyright (c) 2007-2014 Juan   */
/* Linietsky, Ariel Manzur. Licensed under the MIT license.               */
/**************************************************************************/

#include "hitbox_mcp_server.h"

#include "hitbox_tools.h"

#include "core/io/json.h"
#include "core/os/os.h"
#include "core/templates/hash_map.h"
#include "core/version.h"

static const int HITBOX_MCP_MAX_HEADER = 64 * 1024;
static const int HITBOX_MCP_MAX_BODY = 16 * 1024 * 1024;
static const uint64_t HITBOX_MCP_IDLE_MSEC = 10 * 60 * 1000;
static const int HITBOX_MCP_MAX_CONNS = 32;

HitboxMcpServer::HitboxMcpServer() {
	uint8_t bytes[24];
	if (OS::get_singleton()->get_entropy(bytes, sizeof(bytes)) == OK) {
		token = String::hex_encode_buffer(bytes, sizeof(bytes));
	} else {
		token = String::num_uint64(OS::get_singleton()->get_ticks_usec(), 16) + String::num_uint64(OS::get_singleton()->get_unix_time(), 16);
	}
}

HitboxMcpServer::~HitboxMcpServer() {
	stop();
}

Error HitboxMcpServer::start() {
	if (is_running()) {
		return OK;
	}
	server.instantiate();
	Error err = server->listen(0, IPAddress("127.0.0.1"));
	if (err != OK) {
		server.unref();
		return err;
	}
	port = server->get_local_port();
	set_process(true);
	return OK;
}

void HitboxMcpServer::stop() {
	for (Conn &c : conns) {
		c.peer->disconnect_from_host();
	}
	conns.clear();
	if (server.is_valid()) {
		server->stop();
		server.unref();
	}
	port = 0;
	if (is_inside_tree()) {
		set_process(false);
	}
}

bool HitboxMcpServer::is_running() const {
	return server.is_valid() && server->is_listening();
}

String HitboxMcpServer::get_url() const {
	return vformat("http://127.0.0.1:%d/mcp", port);
}

void HitboxMcpServer::_notification(int p_what) {
	if (p_what == NOTIFICATION_EXIT_TREE) {
		stop();
		return;
	}
	if (p_what != NOTIFICATION_PROCESS || server.is_null()) {
		return;
	}

	const uint64_t now = OS::get_singleton()->get_ticks_msec();
	while (server->is_connection_available()) {
		Ref<StreamPeerTCP> peer = server->take_connection();
		if (peer.is_null()) {
			break;
		}
		if (conns.size() >= HITBOX_MCP_MAX_CONNS) {
			peer->disconnect_from_host();
			continue;
		}
		Conn c;
		c.peer = peer;
		c.last_msec = now;
		conns.push_back(c);
	}

	List<Conn>::Element *E = conns.front();
	while (E) {
		List<Conn>::Element *next = E->next();
		Conn &c = E->get();
		c.peer->poll();
		bool drop = c.peer->get_status() != StreamPeerTCP::STATUS_CONNECTED;
		if (!drop) {
			const int available = c.peer->get_available_bytes();
			if (available > 0) {
				const int old = c.in.size();
				c.in.resize(old + available);
				int received = 0;
				Error err = c.peer->get_partial_data(c.in.ptrw() + old, available, received);
				c.in.resize(old + MAX(received, 0));
				if (err != OK) {
					drop = true;
				} else {
					c.last_msec = now;
				}
			}
			while (!drop && !c.in.is_empty()) {
				const int r = _try_handle(c);
				if (r < 0) {
					drop = true;
				} else if (r == 0) {
					break;
				}
			}
			if (now - c.last_msec > HITBOX_MCP_IDLE_MSEC) {
				drop = true;
			}
		}
		if (drop) {
			c.peer->disconnect_from_host();
			conns.erase(E);
		}
		E = next;
	}
}

static int _find_crlf(const uint8_t *p_data, int p_from, int p_size) {
	for (int i = p_from; i + 1 < p_size; i++) {
		if (p_data[i] == '\r' && p_data[i + 1] == '\n') {
			return i;
		}
	}
	return -1;
}

int HitboxMcpServer::_try_handle(Conn &p_conn) {
	const int size = p_conn.in.size();
	const uint8_t *d = p_conn.in.ptr();

	int header_end = -1;
	for (int i = 3; i < size; i++) {
		if (d[i - 3] == '\r' && d[i - 2] == '\n' && d[i - 1] == '\r' && d[i] == '\n') {
			header_end = i + 1;
			break;
		}
	}
	if (header_end < 0) {
		return size > HITBOX_MCP_MAX_HEADER ? -1 : 0;
	}

	const Vector<String> lines = String::utf8((const char *)d, header_end).split("\r\n", false);
	if (lines.is_empty()) {
		return -1;
	}
	const Vector<String> request_line = lines[0].split(" ", false);
	if (request_line.size() < 2) {
		return -1;
	}
	const String method = request_line[0].to_upper();
	const String path = request_line[1].get_slice("?", 0);
	HashMap<String, String> headers;
	for (int i = 1; i < lines.size(); i++) {
		const int colon = lines[i].find(":");
		if (colon > 0) {
			headers[lines[i].substr(0, colon).strip_edges().to_lower()] = lines[i].substr(colon + 1).strip_edges();
		}
	}

	// Read the body: Content-Length or chunked.
	Vector<uint8_t> body;
	int consumed = header_end;
	if (headers.has("transfer-encoding") && headers["transfer-encoding"].to_lower().contains("chunked")) {
		int pos = header_end;
		while (true) {
			const int line_end = _find_crlf(d, pos, size);
			if (line_end < 0) {
				return 0;
			}
			const int64_t chunk = String::utf8((const char *)d + pos, line_end - pos).get_slice(";", 0).strip_edges().hex_to_int();
			if (chunk < 0 || body.size() + chunk > HITBOX_MCP_MAX_BODY) {
				return -1;
			}
			pos = line_end + 2;
			if (chunk == 0) {
				// Optional trailers, then an empty line.
				while (true) {
					const int trailer_end = _find_crlf(d, pos, size);
					if (trailer_end < 0) {
						return 0;
					}
					const bool empty = trailer_end == pos;
					pos = trailer_end + 2;
					if (empty) {
						break;
					}
				}
				consumed = pos;
				break;
			}
			if (pos + chunk + 2 > size) {
				return 0;
			}
			const int old = body.size();
			body.resize(old + chunk);
			memcpy(body.ptrw() + old, d + pos, chunk);
			pos += chunk + 2;
		}
	} else {
		const int64_t length = headers.has("content-length") ? headers["content-length"].to_int() : 0;
		if (length < 0 || length > HITBOX_MCP_MAX_BODY) {
			return -1;
		}
		if (header_end + length > size) {
			return 0;
		}
		body.resize(length);
		if (length > 0) {
			memcpy(body.ptrw(), d + header_end, length);
		}
		consumed = header_end + length;
	}

	// Drop the request from the buffer; `d` is invalid after this.
	if (consumed >= size) {
		p_conn.in.clear();
	} else {
		Vector<uint8_t> rest;
		rest.resize(size - consumed);
		memcpy(rest.ptrw(), d + consumed, size - consumed);
		p_conn.in = rest;
	}

	const bool keep_alive = !(headers.has("connection") && headers["connection"].to_lower() == "close");
	const Ref<StreamPeerTCP> peer = p_conn.peer;

	if (path != "/mcp") {
		_respond(peer, 404, "", keep_alive);
		return keep_alive ? 1 : -1;
	}
	if (!headers.has("authorization") || headers["authorization"] != "Bearer " + token) {
		_respond(peer, 401, "", keep_alive, "WWW-Authenticate: Bearer\r\n");
		return keep_alive ? 1 : -1;
	}
	if (method == "DELETE") {
		_respond(peer, 200, "", keep_alive);
		return keep_alive ? 1 : -1;
	}
	if (method != "POST") {
		// No server-initiated stream: clients fall back to POST-only.
		_respond(peer, 405, "", keep_alive, "Allow: POST, DELETE\r\n");
		return keep_alive ? 1 : -1;
	}

	JSON json;
	if (json.parse(String::utf8((const char *)body.ptr(), body.size())) != OK) {
		Dictionary error;
		error["code"] = -32700;
		error["message"] = "Parse error: " + json.get_error_message();
		Dictionary response;
		response["jsonrpc"] = "2.0";
		response["id"] = Variant();
		response["error"] = error;
		_respond(peer, 400, JSON::stringify(response), keep_alive);
		return keep_alive ? 1 : -1;
	}

	const Variant message = json.get_data();
	Variant reply;
	if (message.get_type() == Variant::ARRAY) {
		const Array batch = message;
		Array replies;
		for (int i = 0; i < batch.size(); i++) {
			const Variant r = _handle_rpc(batch[i]);
			if (r.get_type() != Variant::NIL) {
				replies.push_back(r);
			}
		}
		if (!replies.is_empty()) {
			reply = replies;
		}
	} else {
		reply = _handle_rpc(message);
	}

	if (reply.get_type() == Variant::NIL) {
		// Notifications and client responses get no body.
		_respond(peer, 202, "", keep_alive);
	} else {
		_respond(peer, 200, JSON::stringify(reply), keep_alive, "Content-Type: application/json\r\n");
	}
	return keep_alive ? 1 : -1;
}

void HitboxMcpServer::_respond(const Ref<StreamPeerTCP> &p_peer, int p_status, const String &p_body, bool p_keep_alive, const String &p_extra_headers) {
	String reason;
	switch (p_status) {
		case 200:
			reason = "OK";
			break;
		case 202:
			reason = "Accepted";
			break;
		case 400:
			reason = "Bad Request";
			break;
		case 401:
			reason = "Unauthorized";
			break;
		case 404:
			reason = "Not Found";
			break;
		case 405:
			reason = "Method Not Allowed";
			break;
		default:
			reason = "Error";
			break;
	}
	const CharString body = p_body.utf8();
	String head = vformat("HTTP/1.1 %d %s\r\n", p_status, reason);
	head += vformat("Content-Length: %d\r\n", body.length());
	head += p_keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
	head += p_extra_headers;
	head += "\r\n";
	const CharString h = head.utf8();
	p_peer->put_data((const uint8_t *)h.get_data(), h.length());
	if (body.length() > 0) {
		p_peer->put_data((const uint8_t *)body.get_data(), body.length());
	}
}

static Dictionary _rpc_error(const Variant &p_id, int p_code, const String &p_message) {
	Dictionary error;
	error["code"] = p_code;
	error["message"] = p_message;
	Dictionary response;
	response["jsonrpc"] = "2.0";
	response["id"] = p_id;
	response["error"] = error;
	return response;
}

Variant HitboxMcpServer::_handle_rpc(const Variant &p_message) {
	if (p_message.get_type() != Variant::DICTIONARY) {
		return _rpc_error(Variant(), -32600, "Invalid Request");
	}
	const Dictionary request = p_message;
	const bool is_notification = !request.has("id");
	if (!request.has("method")) {
		// A response to a server request; this server never sends any.
		return Variant();
	}
	const String method = request["method"];
	const Variant id = request.get("id", Variant());
	const Dictionary params = request.get("params", Variant()).get_type() == Variant::DICTIONARY ? Dictionary(request["params"]) : Dictionary();

	if (is_notification) {
		// notifications/initialized, notifications/cancelled, ...: nothing to do.
		return Variant();
	}

	Dictionary result;
	if (method == "initialize") {
		// Echo the client's protocol version: every client supports its own.
		result["protocolVersion"] = params.get("protocolVersion", "2025-06-18");
		Dictionary tools;
		tools["listChanged"] = false;
		Dictionary capabilities;
		capabilities["tools"] = tools;
		result["capabilities"] = capabilities;
		Dictionary info;
		info["name"] = "hitbox";
		info["title"] = "Hitbox editor";
		info["version"] = GODOT_VERSION_FULL_CONFIG;
		result["serverInfo"] = info;
		result["instructions"] = "Tools that read and change the Godot project open in the Hitbox editor, run the game, and read the editor's output.";
	} else if (method == "ping") {
		// Empty result.
	} else if (method == "tools/list") {
		Array tools;
		const Array definitions = HitboxTools::get_tool_definitions();
		for (int i = 0; i < definitions.size(); i++) {
			const Dictionary def = definitions[i];
			Dictionary tool;
			tool["name"] = def["name"];
			tool["description"] = def["description"];
			tool["inputSchema"] = def["input_schema"];
			tools.push_back(tool);
		}
		result["tools"] = tools;
	} else if (method == "tools/call") {
		const String name = params.get("name", "");
		const Dictionary arguments = params.get("arguments", Variant()).get_type() == Variant::DICTIONARY ? Dictionary(params["arguments"]) : Dictionary();
		if (name.is_empty()) {
			return _rpc_error(id, -32602, "tools/call needs a tool name");
		}
		bool is_error = false;
		const String out = HitboxTools::execute(name, arguments, is_error);
		calls++;
		Dictionary text;
		text["type"] = "text";
		text["text"] = out;
		Array content;
		content.push_back(text);
		result["content"] = content;
		result["isError"] = is_error;
	} else if (method == "resources/list") {
		result["resources"] = Array();
	} else if (method == "prompts/list") {
		result["prompts"] = Array();
	} else {
		return _rpc_error(id, -32601, "Method not found: " + method);
	}

	Dictionary response;
	response["jsonrpc"] = "2.0";
	response["id"] = id;
	response["result"] = result;
	return response;
}
