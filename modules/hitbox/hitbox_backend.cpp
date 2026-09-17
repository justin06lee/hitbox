/**************************************************************************/
/*  hitbox_backend.cpp                                                    */
/**************************************************************************/
/*                          This file is part of:                         */
/*                                HITBOX                                  */
/*                 https://github.com/justin06lee/hitbox                  */
/**************************************************************************/
/* Hitbox is a fork of Godot Engine. Godot Engine is Copyright (c)        */
/* 2014-present Godot Engine contributors, Copyright (c) 2007-2014 Juan   */
/* Linietsky, Ariel Manzur. Licensed under the MIT license.               */
/**************************************************************************/

#include "hitbox_backend.h"

#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/os/os.h"
#include "editor/settings/editor_settings.h"

static const char *HITBOX_ANTHROPIC_URL = "https://api.anthropic.com";
static const char *HITBOX_YAGAMI_DEFAULT_URL = "http://127.0.0.1:8787";
static const char *HITBOX_YAGAMI_PACKAGE = "@justin06lee/yagami@latest";

static String _home() {
	return OS::get_singleton()->get_environment("HOME");
}

static String _env(const String &p_name) {
	return OS::get_singleton()->get_environment(p_name).strip_edges();
}

String HitboxBackend::yagami_config_dir() {
	const String dir = _env("YAGAMI_CONFIG_DIR");
	if (!dir.is_empty()) {
		return dir;
	}
	return _home().path_join(".config").path_join("yagami");
}

bool HitboxBackend::read_yagami_config(const String &p_path, String &r_base_url, String &r_api_key) {
	String host = "127.0.0.1";
	int64_t port = 8787;
	String key;
	bool exists = false;

	if (FileAccess::exists(p_path)) {
		exists = true;
		const String text = FileAccess::get_file_as_string(p_path);
		JSON json;
		if (json.parse(text) == OK && json.get_data().get_type() == Variant::DICTIONARY) {
			const Dictionary config = json.get_data();
			if (config.get("host", Variant()).get_type() == Variant::STRING) {
				host = config["host"];
			}
			const Variant p = config.get("port", Variant());
			if (p.get_type() == Variant::FLOAT || p.get_type() == Variant::INT) {
				port = (int64_t)p;
			}
			const Variant keys = config.get("apiKeys", Variant());
			if (keys.get_type() == Variant::ARRAY && !Array(keys).is_empty()) {
				key = Array(keys)[0];
			}
		}
	}

	if (!_env("YAGAMI_HOST").is_empty()) {
		host = _env("YAGAMI_HOST");
	}
	if (_env("YAGAMI_PORT").is_valid_int()) {
		port = _env("YAGAMI_PORT").to_int();
	}
	if (!_env("YAGAMI_API_KEY").is_empty()) {
		key = _env("YAGAMI_API_KEY");
	}

	// A wildcard bind is reachable on loopback.
	if (host == "0.0.0.0" || host == "::" || host.is_empty()) {
		host = "127.0.0.1";
	}
	if (host.contains(":") && !host.begins_with("[")) {
		host = "[" + host + "]";
	}
	r_base_url = vformat("http://%s:%d", host, port);
	r_api_key = key;
	return exists;
}

String HitboxBackend::find_executable(const String &p_name) {
	Vector<String> dirs;
	const String home = _home();
	dirs.push_back(home.path_join(".local/bin"));
	dirs.push_back(home.path_join(".bun/bin"));
	dirs.push_back("/opt/homebrew/bin");
	dirs.push_back("/usr/local/bin");
	const Vector<String> path = _env("PATH").split(":", false);
	for (const String &d : path) {
		if (!dirs.has(d)) {
			dirs.push_back(d);
		}
	}
	dirs.push_back("/usr/bin");
	dirs.push_back("/bin");
	for (const String &d : dirs) {
		const String candidate = d.path_join(p_name);
		if (FileAccess::exists(candidate)) {
			return candidate;
		}
	}
	return String();
}

// Starts yagami in the background. Tries an installed binary first, then the
// npm package through bunx or npx. PATH is widened because an app launched
// from the Dock does not inherit the login shell's PATH, and yagami's
// launcher needs `node`.
static String _autostart_script() {
	return String() +
			"export PATH=\"$HOME/.local/bin:$HOME/.bun/bin:/opt/homebrew/bin:/usr/local/bin:$PATH\"\n" +
			"if command -v yagami >/dev/null 2>&1; then exec yagami start --daemon; fi\n" +
			"if command -v bunx >/dev/null 2>&1; then exec bunx " + HITBOX_YAGAMI_PACKAGE + " start --daemon; fi\n" +
			"if command -v npx >/dev/null 2>&1; then exec npx -y " + HITBOX_YAGAMI_PACKAGE + " start --daemon; fi\n" +
			"echo 'yagami is not installed, and neither bunx nor npx was found to run it from npm.' >&2\n" +
			"exit 127\n";
}

HitboxBackendConfig HitboxBackend::resolve() {
	const String mode = String(EDITOR_GET("hitbox/backend/mode")).strip_edges().to_lower();

	String settings_key = String(EDITOR_GET("hitbox/anthropic/api_key")).strip_edges();
	const String env_key = _env("ANTHROPIC_API_KEY");

	const String config_path = yagami_config_dir().path_join("config.json");
	String yagami_url;
	String yagami_key;
	const bool yagami_config = read_yagami_config(config_path, yagami_url, yagami_key);
	const String url_override = String(EDITOR_GET("hitbox/yagami/url")).strip_edges();
	const String key_override = String(EDITOR_GET("hitbox/yagami/api_key")).strip_edges();
	if (!url_override.is_empty()) {
		yagami_url = url_override;
	}
	if (!key_override.is_empty()) {
		yagami_key = key_override;
	}
	while (yagami_url.ends_with("/")) {
		yagami_url = yagami_url.left(-1);
	}
	if (yagami_url.ends_with("/v1")) {
		yagami_url = yagami_url.left(-3);
	}
	if (yagami_url.is_empty()) {
		yagami_url = HITBOX_YAGAMI_DEFAULT_URL;
	}

	const bool is_windows = OS::get_singleton()->get_name() == "Windows";
	const String yagami_bin = is_windows ? String() : find_executable("yagami");
	String npm_runner;
	if (!is_windows) {
		if (!find_executable("bunx").is_empty()) {
			npm_runner = "bunx";
		} else if (!find_executable("npx").is_empty()) {
			npm_runner = "npx";
		}
	}
	const bool claude_cli = !find_executable("claude").is_empty();

	bool use_yagami = false;
	if (mode == "yagami") {
		use_yagami = true;
	} else if (mode == "anthropic_api") {
		use_yagami = false;
	} else {
		// Auto: a key typed into Hitbox is a deliberate choice; otherwise a
		// yagami install (or the means to run it from npm next to a Claude
		// Code CLI) wins over an API key that merely sits in the environment.
		if (!settings_key.is_empty()) {
			use_yagami = false;
		} else if (yagami_config || !yagami_bin.is_empty() || !url_override.is_empty()) {
			use_yagami = true;
		} else if (!env_key.is_empty()) {
			use_yagami = false;
		} else if (claude_cli && !npm_runner.is_empty()) {
			use_yagami = true;
		}
	}

	HitboxBackendConfig cfg;
	if (!use_yagami) {
		cfg.kind = HitboxBackendConfig::KIND_ANTHROPIC;
		cfg.base_url = HITBOX_ANTHROPIC_URL;
		cfg.api_key = settings_key.is_empty() ? env_key : settings_key;
		cfg.label = "the Anthropic API";
		if (cfg.api_key.is_empty()) {
			cfg.problem = "No Anthropic API key. Paste one below, set ANTHROPIC_API_KEY, or install yagami to use your Claude Code sign-in instead.";
		}
		return cfg;
	}

	cfg.kind = HitboxBackendConfig::KIND_YAGAMI;
	cfg.base_url = yagami_url;
	cfg.api_key = yagami_key;
	cfg.yagami_config_path = config_path;
	cfg.label = "yagami at " + yagami_url;

	const bool loopback = yagami_url.contains("://127.0.0.1") || yagami_url.contains("://localhost") || yagami_url.contains("://[::1]");
	if (bool(EDITOR_GET("hitbox/yagami/auto_start")) && loopback && !is_windows) {
		if (!yagami_bin.is_empty()) {
			cfg.autostart_via = "yagami";
		} else if (!npm_runner.is_empty()) {
			cfg.autostart_via = npm_runner + " " + HITBOX_YAGAMI_PACKAGE;
		}
		if (!cfg.autostart_via.is_empty()) {
			cfg.autostart_script = _autostart_script();
		}
	}
	if (cfg.api_key.is_empty() && cfg.autostart_script.is_empty()) {
		cfg.problem = "yagami has no API key yet. Run `yagami start` once (or `bunx @justin06lee/yagami start`), or set Editor Settings > Hitbox > Yagami > Api Key.";
	}
	return cfg;
}
