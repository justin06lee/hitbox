/**************************************************************************/
/*  hitbox_backend.h                                                      */
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

#include "core/string/ustring.h"

// Where the agent's requests go.
//
// - Anthropic API: api.anthropic.com with an API key. Hitbox runs the tool
//   loop itself.
// - yagami: a local server (github.com/justin06lee/yagami, npm
//   @justin06lee/yagami) that serves the Messages API from the signed-in
//   Claude Code CLI. Hitbox exposes its tools as an MCP server and the model
//   calls them inside the turn. No API key needed.
struct HitboxBackendConfig {
	enum Kind {
		KIND_ANTHROPIC,
		KIND_YAGAMI,
	};

	Kind kind = KIND_ANTHROPIC;
	// Scheme + host + port, no trailing slash: "https://api.anthropic.com".
	String base_url;
	// May be empty for yagami until it has been started once.
	String api_key;
	// yagami: config.json to re-read the key from after starting the server.
	String yagami_config_path;
	// yagami: shell script that starts the server; empty when auto-start is off.
	String autostart_script;
	// yagami: how it would be started ("yagami", "bunx @justin06lee/yagami", ...).
	String autostart_via;
	// Human-readable, for the dock: "yagami at http://127.0.0.1:8787".
	String label;
	// Set when the backend cannot be used as configured.
	String problem;
};

class HitboxBackend {
public:
	static HitboxBackendConfig resolve();

	// $YAGAMI_CONFIG_DIR, else ~/.config/yagami.
	static String yagami_config_dir();
	// Reads host, port and the first API key from yagami's config.json,
	// with the same YAGAMI_HOST / YAGAMI_PORT / YAGAMI_API_KEY overrides the
	// server applies. Returns false when the file does not exist.
	static bool read_yagami_config(const String &p_path, String &r_base_url, String &r_api_key);
	// Absolute path of an executable, searching PATH and the usual install
	// directories a GUI app's PATH lacks. Empty when not found.
	static String find_executable(const String &p_name);
};
