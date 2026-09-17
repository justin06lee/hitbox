/**************************************************************************/
/*  hitbox_mcp_server.h                                                   */
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

#include "core/io/stream_peer_tcp.h"
#include "core/io/tcp_server.h"
#include "core/templates/list.h"
#include "scene/main/node.h"

// A minimal MCP server (JSON-RPC 2.0 over the streamable HTTP transport)
// exposing HitboxTools. A backend such as yagami connects Claude Code to it,
// so the model calls the editor's tools directly during a turn.
//
// It binds 127.0.0.1 on a random port, requires a random bearer token, and
// is polled from the main thread, so every tool call runs where editor state
// may be touched.
class HitboxMcpServer : public Node {
	GDCLASS(HitboxMcpServer, Node);

	struct Conn {
		Ref<StreamPeerTCP> peer;
		Vector<uint8_t> in;
		uint64_t last_msec = 0;
	};

	Ref<TCPServer> server;
	List<Conn> conns;
	String token;
	int port = 0;
	int calls = 0;

	// 1: handled one request, 0: need more bytes, -1: close the connection.
	int _try_handle(Conn &p_conn);
	void _respond(const Ref<StreamPeerTCP> &p_peer, int p_status, const String &p_body, bool p_keep_alive, const String &p_extra_headers = String());
	Variant _handle_rpc(const Variant &p_message);

protected:
	void _notification(int p_what);
	static void _bind_methods() {}

public:
	Error start();
	void stop();
	bool is_running() const;
	// e.g. "http://127.0.0.1:52011/mcp"
	String get_url() const;
	String get_token() const { return token; }
	int get_call_count() const { return calls; }

	HitboxMcpServer();
	~HitboxMcpServer();
};
