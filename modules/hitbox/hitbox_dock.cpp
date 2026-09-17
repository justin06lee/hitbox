/**************************************************************************/
/*  hitbox_dock.cpp                                                       */
/**************************************************************************/
/*                          This file is part of:                         */
/*                                HITBOX                                  */
/*                 https://github.com/justin06lee/hitbox                  */
/**************************************************************************/
/* Hitbox is a fork of Godot Engine. Godot Engine is Copyright (c)        */
/* 2014-present Godot Engine contributors, Copyright (c) 2007-2014 Juan   */
/* Linietsky, Ariel Manzur. Licensed under the MIT license.               */
/**************************************************************************/

#include "hitbox_dock.h"

#include "hitbox_client.h"
#include "hitbox_mcp_server.h"
#include "hitbox_tools.h"

#include "core/input/input_event.h"
#include "core/io/json.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/keyboard.h"
#include "core/os/os.h"
#include "editor/editor_string_names.h"
#include "editor/settings/editor_settings.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/option_button.h"
#include "scene/gui/rich_text_label.h"
#include "scene/gui/text_edit.h"

static const int HITBOX_MAX_TOOL_RESULT_CHARS = 120000;

struct HitboxModel {
	const char *id;
	const char *label;
	bool thinking;
	bool fallbacks;
	int max_tokens;
};

static const HitboxModel HITBOX_MODELS[] = {
	{ "claude-opus-5", "Claude Opus 5", true, true, 64000 },
	{ "claude-sonnet-5", "Claude Sonnet 5", true, false, 64000 },
	{ "claude-fable-5-1", "Claude Fable 5.1", true, true, 64000 },
	{ "claude-haiku-4-5", "Claude Haiku 4.5", false, false, 32000 },
};
static const int HITBOX_MODEL_COUNT = sizeof(HITBOX_MODELS) / sizeof(HITBOX_MODELS[0]);

static const char *HITBOX_PROMPT_BASE =
		"You are Hitbox, an AI game-development agent built directly into Hitbox, a fork of the Godot 4.7 editor. "
		"You are talking with the developer inside their open project.\n"
		"\n"
		"Every user message starts with an <editor_context> block describing what the developer is looking at: the open scene, "
		"the selected nodes, and the active script with any selected text. Use it instead of asking for that information.\n"
		"\n"
		"Rules:\n"
		"- This is Godot 4. Never write Godot 3 syntax. Use `@export`, `@onready`, `await`, `signal_name.connect(callable)`, "
		"`create_tween()`, `CharacterBody2D.velocity` with `move_and_slide()`, `Input.is_action_pressed()`, and typed GDScript "
		"where it reads naturally. `yield`, `export var`, `onready var`, `KinematicBody2D`, `instance()` and `connect(\"sig\", obj, \"method\")` do not exist.\n"
		"- Paths are res:// project paths. Node paths are relative to the scene root, where \".\" is the root.\n"
		"- Keep replies short and concrete.\n";

static const char *HITBOX_PROMPT_TOOLS =
		"\n"
		"You have tools that read and modify the project, inspect and edit the scene open in the editor, run the game, and read "
		"the editor's output log.\n"
		"\n"
		"Tool rules:\n"
		"- When you are not certain about an engine API (method names, property names, signals, argument order, enum values), "
		"call get_class_docs first. Those docs come from this exact engine build and are authoritative.\n"
		"- Look before you edit: read a file or inspect the scene tree before changing it. For changes to existing files prefer "
		"edit_file with a unique old_string over rewriting the whole file.\n"
		"- Scenes: prefer the structured node tools (add_node, set_node_property, attach_script, remove_node); they are undoable "
		"and keep the editor in sync. Writing .tscn text with write_file is acceptable for bulk changes; the editor reloads the "
		"scene from disk when you do, discarding unsaved editor changes to it. Call save_all after scene edits you want on disk.\n"
		"- Files you write are picked up by the editor automatically; no restart is needed.\n"
		"- Verify when it matters: after meaningful gameplay changes, save_all, run_project, then get_output_log to check for "
		"parse errors and runtime errors, then stop_project. Fix what you find.\n"
		"- Say what you changed and where. Do not paste whole files back into the chat.\n";

static const char *HITBOX_PROMPT_MCP_NAMES =
		"- The tools are served by the editor's MCP server, so their names may carry a prefix, such as mcp__hitbox__read_file "
		"for read_file. They are the same tools.\n";

static const char *HITBOX_PROMPT_NO_TOOLS =
		"\n"
		"In this session you have no tools: the connected server cannot hand you the editor's tools. Work from the "
		"<editor_context>, and give the developer exact code and the steps to apply it: which file, which node, which property.\n";

/* ---------------------------------------------------------------------- */
/* Construction                                                            */
/* ---------------------------------------------------------------------- */

HitboxDock::HitboxDock() {
	set_name(TTRC("Hitbox"));
	set_icon_name("Hitbox");
	// Own full-height column at the far right, next to the Inspector column.
	set_default_slot(EditorDock::DOCK_SLOT_RIGHT_UR);
	set_custom_minimum_size(Size2(280, 0) * EDSCALE);

	VBoxContainer *vb = memnew(VBoxContainer);
	vb->set_v_size_flags(SIZE_EXPAND_FILL);
	vb->set_h_size_flags(SIZE_EXPAND_FILL);
	add_child(vb);

	// Top row: model picker and new chat.
	HBoxContainer *top = memnew(HBoxContainer);
	vb->add_child(top);

	model_button = memnew(OptionButton);
	model_button->set_h_size_flags(SIZE_EXPAND_FILL);
	model_button->set_flat(true);
	model_button->set_tooltip_text(TTR("Model used for this chat. Change the default in Editor Settings > Hitbox."));
	for (int i = 0; i < HITBOX_MODEL_COUNT; i++) {
		model_button->add_item(HITBOX_MODELS[i].label, i);
	}
	model_button->select(_get_model_index());
	model_button->connect("item_selected", callable_mp(this, &HitboxDock::_on_model_selected));
	top->add_child(model_button);

	new_chat_button = memnew(Button);
	new_chat_button->set_flat(true);
	new_chat_button->set_tooltip_text(TTR("New chat"));
	new_chat_button->connect("pressed", callable_mp(this, &HitboxDock::_new_chat));
	top->add_child(new_chat_button);

	// Transcript.
	transcript = memnew(RichTextLabel);
	transcript->set_v_size_flags(SIZE_EXPAND_FILL);
	transcript->set_h_size_flags(SIZE_EXPAND_FILL);
	transcript->set_selection_enabled(true);
	transcript->set_context_menu_enabled(true);
	transcript->set_scroll_follow(true);
	transcript->set_focus_mode(FOCUS_CLICK);
	vb->add_child(transcript);

	// Status line.
	status_label = memnew(Label);
	status_label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	status_label->hide();
	vb->add_child(status_label);

	// API key row, only shown until a key exists.
	key_row = memnew(HBoxContainer);
	vb->add_child(key_row);

	key_edit = memnew(LineEdit);
	key_edit->set_h_size_flags(SIZE_EXPAND_FILL);
	key_edit->set_secret(true);
	key_edit->set_placeholder(TTR("Anthropic API key (sk-ant-...)"));
	key_edit->connect("text_submitted", callable_mp(this, &HitboxDock::_save_key).unbind(1));
	key_row->add_child(key_edit);

	key_save_button = memnew(Button);
	key_save_button->set_text(TTR("Save"));
	key_save_button->connect("pressed", callable_mp(this, &HitboxDock::_save_key));
	key_row->add_child(key_save_button);

	// Input row.
	HBoxContainer *bottom = memnew(HBoxContainer);
	vb->add_child(bottom);

	input = memnew(TextEdit);
	input->set_h_size_flags(SIZE_EXPAND_FILL);
	input->set_custom_minimum_size(Size2(0, 72) * EDSCALE);
	input->set_line_wrapping_mode(TextEdit::LINE_WRAPPING_BOUNDARY);
	input->set_placeholder(TTR("Ask Hitbox to build, fix or explain something.\nEnter to send, Shift+Enter for a new line."));
	input->connect("gui_input", callable_mp(this, &HitboxDock::_on_input_gui_input));
	bottom->add_child(input);

	VBoxContainer *buttons = memnew(VBoxContainer);
	bottom->add_child(buttons);

	send_button = memnew(Button);
	send_button->set_text(TTR("Send"));
	send_button->connect("pressed", callable_mp(this, &HitboxDock::_send));
	buttons->add_child(send_button);

	stop_button = memnew(Button);
	stop_button->set_tooltip_text(TTR("Stop the current response"));
	stop_button->connect("pressed", callable_mp(this, &HitboxDock::_stop));
	stop_button->hide();
	buttons->add_child(stop_button);

	// Serves the editor tools to yagami; started on the first yagami request.
	mcp_server = memnew(HitboxMcpServer);
	mcp_server->set_name("HitboxMcpServer");
	add_child(mcp_server);

	backend = HitboxBackend::resolve();
	EditorSettings::get_singleton()->connect("settings_changed", callable_mp(this, &HitboxDock::_on_settings_changed));
	_update_ui();
}

void HitboxDock::_bind_methods() {
	ClassDB::bind_method(D_METHOD("send_prompt", "text"), &HitboxDock::send_prompt);
	ClassDB::bind_method(D_METHOD("is_busy"), &HitboxDock::is_busy);
	ClassDB::bind_method(D_METHOD("get_transcript_text"), &HitboxDock::get_transcript_text);
	ClassDB::bind_method(D_METHOD("get_backend_label"), &HitboxDock::get_backend_label);
	ClassDB::bind_method(D_METHOD("get_mcp_call_count"), &HitboxDock::get_mcp_call_count);
}

void HitboxDock::send_prompt(const String &p_text) {
	input->set_text(p_text);
	_send();
}

String HitboxDock::get_transcript_text() const {
	return transcript->get_parsed_text();
}

String HitboxDock::get_backend_label() const {
	return backend.label;
}

int HitboxDock::get_mcp_call_count() const {
	return mcp_server ? mcp_server->get_call_count() : 0;
}

HitboxDock::~HitboxDock() {
	if (client.is_valid()) {
		client->cancel();
		client->finish();
	}
}

void HitboxDock::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_THEME_CHANGED: {
			new_chat_button->set_button_icon(get_editor_theme_icon(SNAME("New")));
			stop_button->set_button_icon(get_editor_theme_icon(SNAME("Stop")));
			transcript->add_theme_font_override("mono_font", get_theme_font(SNAME("output_source"), EditorStringName(EditorFonts)));
			_show_intro();
		} break;
	}
}

/* ---------------------------------------------------------------------- */
/* Settings                                                                */
/* ---------------------------------------------------------------------- */

void HitboxDock::_on_settings_changed() {
	if (busy) {
		return; // Picked up by the next send.
	}
	backend = HitboxBackend::resolve();
	_show_intro();
	_update_ui();
}

void HitboxDock::_show_intro() {
	if (!transcript_empty) {
		return;
	}
	String intro = TTR("Ask Hitbox to build, fix or explain anything in this project. It can read and edit files, change the open scene, run the game and read the output log.");
	if (backend.kind == HitboxBackendConfig::KIND_YAGAMI) {
		intro += "\n\n" + vformat(TTR("Using %s, which runs on your Claude Code sign-in."), backend.label);
		if (!backend.autostart_via.is_empty()) {
			intro += " " + vformat(TTR("If it is not running, Hitbox starts it with %s."), backend.autostart_via);
		}
	} else {
		intro += "\n\n" + TTR("Using the Anthropic API.");
	}
	if (!backend.problem.is_empty()) {
		intro += "\n\n" + backend.problem;
	}
	transcript->clear();
	transcript->push_color(_color(SNAME("font_disabled_color")));
	transcript->add_text(intro);
	transcript->pop();
}

String HitboxDock::_system_prompt() const {
	String prompt = String(HITBOX_PROMPT_BASE);
	if (backend.kind == HitboxBackendConfig::KIND_ANTHROPIC) {
		prompt += HITBOX_PROMPT_TOOLS;
	} else if (yagami_tools) {
		prompt += HITBOX_PROMPT_TOOLS;
		prompt += HITBOX_PROMPT_MCP_NAMES;
	} else {
		prompt += HITBOX_PROMPT_NO_TOOLS;
	}
	return prompt;
}

int HitboxDock::_get_model_index() const {
	const String id = EDITOR_GET("hitbox/anthropic/model");
	for (int i = 0; i < HITBOX_MODEL_COUNT; i++) {
		if (id == HITBOX_MODELS[i].id) {
			return i;
		}
	}
	return 0;
}

void HitboxDock::_on_model_selected(int p_index) {
	if (p_index < 0 || p_index >= HITBOX_MODEL_COUNT) {
		return;
	}
	EditorSettings::get_singleton()->set("hitbox/anthropic/model", HITBOX_MODELS[p_index].id);
}

void HitboxDock::_save_key() {
	const String key = key_edit->get_text().strip_edges();
	if (key.is_empty()) {
		return;
	}
	EditorSettings::get_singleton()->set("hitbox/anthropic/api_key", key);
	EditorSettings::save();
	key_edit->set_text("");
	_set_status("");
	backend = HitboxBackend::resolve();
	_show_intro();
	_update_ui();
}

/* ---------------------------------------------------------------------- */
/* UI helpers                                                              */
/* ---------------------------------------------------------------------- */

Color HitboxDock::_color(const StringName &p_name) const {
	return get_theme_color(p_name, EditorStringName(Editor));
}

void HitboxDock::_update_ui() {
	send_button->set_visible(!busy);
	stop_button->set_visible(busy);
	key_row->set_visible(backend.kind == HitboxBackendConfig::KIND_ANTHROPIC && backend.api_key.is_empty());
}

void HitboxDock::_set_status(const String &p_text) {
	status_label->set_text(p_text);
	status_label->set_visible(!p_text.is_empty());
}

void HitboxDock::_append_header(const String &p_who) {
	if (transcript_empty) {
		transcript->clear();
		transcript_empty = false;
	} else {
		transcript->add_newline();
	}
	transcript->push_bold();
	transcript->push_color(_color(SNAME("accent_color")));
	transcript->add_text(p_who);
	transcript->pop();
	transcript->pop();
	transcript->add_newline();
}

void HitboxDock::_append_line(const String &p_text, const Color &p_color) {
	if (transcript_empty) {
		transcript->clear();
		transcript_empty = false;
	}
	transcript->push_color(p_color);
	transcript->add_text(p_text);
	transcript->pop();
	transcript->add_newline();
}

void HitboxDock::_toggle_fence() {
	if (in_code_fence) {
		transcript->pop();
		transcript->pop();
		in_code_fence = false;
	} else {
		transcript->push_mono();
		transcript->push_color(_color(SNAME("font_readonly_color")));
		in_code_fence = true;
		skipping_fence_tag = true;
	}
}

void HitboxDock::_append_stream_text(const String &p_text) {
	String run;
	for (int i = 0; i < p_text.length(); i++) {
		const char32_t c = p_text[i];
		if (c == '`') {
			if (!run.is_empty()) {
				transcript->add_text(run);
				run = String();
			}
			backtick_run++;
			if (backtick_run == 3) {
				backtick_run = 0;
				_toggle_fence();
			}
			continue;
		}
		if (backtick_run > 0) {
			run += String("`").repeat(backtick_run);
			backtick_run = 0;
		}
		if (skipping_fence_tag) {
			if (c == '\n') {
				skipping_fence_tag = false;
			}
			continue;
		}
		run += c;
	}
	if (!run.is_empty()) {
		transcript->add_text(run);
	}
}

void HitboxDock::_close_block_rendering() {
	switch (render_state) {
		case RENDER_TEXT: {
			if (backtick_run > 0) {
				transcript->add_text(String("`").repeat(backtick_run));
				backtick_run = 0;
			}
			if (in_code_fence) {
				_toggle_fence();
			}
			transcript->add_newline();
		} break;
		case RENDER_THINKING: {
			transcript->pop();
			transcript->pop();
			transcript->add_newline();
		} break;
		case RENDER_NONE:
			break;
	}
	render_state = RENDER_NONE;
}

/* ---------------------------------------------------------------------- */
/* User actions                                                            */
/* ---------------------------------------------------------------------- */

void HitboxDock::_on_input_gui_input(const Ref<InputEvent> &p_event) {
	Ref<InputEventKey> k = p_event;
	if (k.is_null() || !k->is_pressed() || k->is_echo()) {
		return;
	}
	if ((k->get_keycode() == Key::ENTER || k->get_keycode() == Key::KP_ENTER) && !k->is_shift_pressed() && !k->is_alt_pressed()) {
		input->accept_event();
		_send();
	}
}

void HitboxDock::_send() {
	if (busy) {
		return;
	}
	const String text = input->get_text().strip_edges();
	if (text.is_empty()) {
		return;
	}
	backend = HitboxBackend::resolve();
	_update_ui();
	if (!backend.problem.is_empty()) {
		_set_status(backend.problem);
		if (key_row->is_visible()) {
			key_edit->grab_focus();
		}
		return;
	}
	if (conversation_backend != -1 && conversation_backend != (int)backend.kind && !messages.is_empty()) {
		// Histories are not portable between backends (tool blocks differ).
		messages = Array();
		yagami_tools = true;
		_append_line(vformat(TTR("Switched to %s; starting a fresh conversation."), backend.label), _color(SNAME("warning_color")));
	}
	conversation_backend = (int)backend.kind;

	Dictionary context_block;
	context_block["type"] = "text";
	context_block["text"] = HitboxTools::get_editor_context();
	Dictionary text_block;
	text_block["type"] = "text";
	text_block["text"] = text;
	Array content;
	content.push_back(context_block);
	content.push_back(text_block);
	Dictionary msg;
	msg["role"] = "user";
	msg["content"] = content;
	messages.push_back(msg);

	_append_header(TTR("You"));
	_append_line(text, _color(SNAME("font_color")));

	input->set_text("");
	tool_rounds = 0;
	_start_request();
}

void HitboxDock::_stop() {
	if (!busy || client.is_null()) {
		return;
	}
	cancel_requested = true;
	client->cancel();
	_set_status(TTR("Stopping..."));
}

void HitboxDock::_new_chat() {
	if (busy) {
		_stop();
	}
	messages = Array();
	current_blocks = Array();
	partial_json.clear();
	tool_rounds = 0;
	conversation_backend = -1;
	yagami_tools = true;
	backend = HitboxBackend::resolve();
	_update_ui();
	transcript->clear();
	transcript_empty = true;
	render_state = RENDER_NONE;
	in_code_fence = false;
	backtick_run = 0;
	_set_status("");
	notification(NOTIFICATION_THEME_CHANGED);
	input->grab_focus();
}

/* ---------------------------------------------------------------------- */
/* Request lifecycle                                                       */
/* ---------------------------------------------------------------------- */

Dictionary HitboxDock::_build_request(Vector<String> &r_headers) {
	const HitboxModel &model = HITBOX_MODELS[CLAMP(model_button->get_selected(), 0, HITBOX_MODEL_COUNT - 1)];
	const bool yagami = backend.kind == HitboxBackendConfig::KIND_YAGAMI;

	Dictionary req;
	req["model"] = model.id;
	req["max_tokens"] = (int64_t)model.max_tokens;
	req["stream"] = true;

	Dictionary cache;
	cache["type"] = "ephemeral";
	Dictionary system_block;
	system_block["type"] = "text";
	system_block["text"] = _system_prompt();
	if (!yagami) {
		system_block["cache_control"] = cache;
	}
	Array system;
	system.push_back(system_block);
	req["system"] = system;

	if (!yagami) {
		req["tools"] = HitboxTools::get_tool_definitions();
	} else if (yagami_tools) {
		// Anthropic MCP connector shape; yagami connects Claude Code to it.
		Dictionary server;
		server["type"] = "url";
		server["name"] = "hitbox";
		server["url"] = mcp_server->get_url();
		server["authorization_token"] = mcp_server->get_token();
		Array servers;
		servers.push_back(server);
		req["mcp_servers"] = servers;
		Dictionary toolset;
		toolset["type"] = "mcp_toolset";
		toolset["mcp_server_name"] = "hitbox";
		Array tools;
		tools.push_back(toolset);
		req["tools"] = tools;
	}

	if (model.thinking) {
		Dictionary thinking;
		thinking["type"] = "adaptive";
		thinking["display"] = "summarized";
		req["thinking"] = thinking;
		const String effort = EDITOR_GET("hitbox/anthropic/effort");
		if (yagami) {
			req["effort"] = effort; // yagami's extension for Claude Code's effort.
		} else {
			Dictionary output_config;
			output_config["effort"] = effort;
			req["output_config"] = output_config;
		}
	}
	if (model.fallbacks && !yagami) {
		req["fallbacks"] = "default";
		r_headers.push_back("anthropic-beta: server-side-fallback-2026-07-01");
	}

	// Anthropic API: exactly one conversation cache breakpoint, on the newest
	// message, so long tool loops reuse the cached prefix.
	for (int i = 0; i < messages.size(); i++) {
		Dictionary m = messages[i];
		Array content = m["content"];
		if (content.is_empty()) {
			continue;
		}
		Dictionary last = content[content.size() - 1];
		if (!yagami && i == messages.size() - 1) {
			last["cache_control"] = cache;
		} else {
			last.erase("cache_control");
		}
	}
	req["messages"] = messages;
	return req;
}

void HitboxDock::_start_request() {
	busy = true;
	cancel_requested = false;
	request_failed = false;
	current_blocks = Array();
	partial_json.clear();
	current_stop_reason = String();
	render_state = RENDER_NONE;
	assistant_header_shown = false;
	in_code_fence = false;
	skipping_fence_tag = false;
	backtick_run = 0;

	if (backend.kind == HitboxBackendConfig::KIND_YAGAMI && yagami_tools && !mcp_server->is_running()) {
		Error mcp_err = mcp_server->start();
		if (mcp_err != OK) {
			busy = false;
			_append_line(vformat(TTR("Could not start the editor's MCP server (error %d)."), (int)mcp_err), _color(SNAME("error_color")));
			_rollback_to_last_prompt();
			_update_ui();
			return;
		}
	}

	Vector<String> headers;
	Dictionary req = _build_request(headers);

	HitboxClient::Request request;
	request.base_url = backend.base_url;
	request.api_key = backend.api_key;
	request.body_json = JSON::stringify(req);
	request.extra_headers = headers;
	if (backend.kind == HitboxBackendConfig::KIND_YAGAMI) {
		request.autostart_script = backend.autostart_script;
		request.autostart_via = backend.autostart_via;
		request.yagami_config_path = backend.yagami_config_path;
	}

	if (client.is_null()) {
		client.instantiate();
	}
	client->finish();
	Error err = client->start(request, callable_mp(this, &HitboxDock::_on_client_event));
	if (err != OK) {
		busy = false;
		_append_line(TTR("Could not start the request."), _color(SNAME("error_color")));
		_rollback_to_last_prompt();
		_update_ui();
		return;
	}
	_set_status(TTR("Thinking..."));
	_update_ui();
}

void HitboxDock::_on_client_event(const Dictionary &p_event) {
	const String type = p_event.get("type", "");
	if (type == "sse") {
		_handle_sse(p_event.get("event", Dictionary()));
	} else if (type == "info") {
		_append_line(String(p_event.get("message", "")), _color(SNAME("font_disabled_color")));
	} else if (type == "error") {
		// Transport or HTTP-level failure: the worker thread is exiting and no
		// "done" follows, so finish the request here.
		const int status = (int)(int64_t)p_event.get("status", 0);
		const String message = p_event.get("message", "Request failed.");
		if (backend.kind == HitboxBackendConfig::KIND_YAGAMI && yagami_tools && status == 400 && message.contains("mcp_toolset")) {
			// yagami before 0.10 has no MCP connector: keep chatting without tools.
			yagami_tools = false;
			client->finish();
			_close_block_rendering();
			_append_line(TTR("This yagami is older than 0.10 and cannot hand Hitbox the editor's tools, so this chat continues without them. Update yagami for the full agent."), _color(SNAME("warning_color")));
			_start_request();
			return;
		}
		request_failed = true;
		_close_block_rendering();
		_append_line(message, _color(SNAME("error_color")));
		_on_request_finished();
	} else if (type == "done") {
		_on_request_finished();
	}
}

void HitboxDock::_handle_sse(const Dictionary &p_event) {
	const String type = p_event.get("type", "");

	if (type == "content_block_start") {
		const int idx = (int)(int64_t)p_event.get("index", 0);
		Dictionary cb = p_event.get("content_block", Dictionary());
		const String block_type = cb.get("type", "");
		Dictionary block;
		if (block_type == "text") {
			block["type"] = "text";
			block["text"] = "";
			if (!assistant_header_shown) {
				_append_header("Hitbox");
				assistant_header_shown = true;
			}
			render_state = RENDER_TEXT;
			_set_status("");
		} else if (block_type == "tool_use") {
			block["type"] = "tool_use";
			block["id"] = cb.get("id", "");
			block["name"] = cb.get("name", "");
			block["input"] = Dictionary();
			partial_json[idx] = String();
			_set_status(vformat(TTR("Preparing %s..."), String(cb.get("name", ""))));
		} else if (block_type == "thinking") {
			block["type"] = "thinking";
			block["thinking"] = "";
			block["signature"] = "";
			if (!assistant_header_shown) {
				_append_header("Hitbox");
				assistant_header_shown = true;
			}
			transcript->push_italics();
			transcript->push_color(_color(SNAME("font_disabled_color")));
			render_state = RENDER_THINKING;
			_set_status(TTR("Thinking..."));
		} else if (block_type == "mcp_tool_use" || block_type == "mcp_tool_result") {
			// Tool activity from yagami: already executed through the MCP server.
			block = cb.duplicate(true);
			if (!assistant_header_shown) {
				_append_header("Hitbox");
				assistant_header_shown = true;
			}
			if (block_type == "mcp_tool_use") {
				const String name = cb.get("name", "");
				const Dictionary tool_input = cb.get("input", Variant()).get_type() == Variant::DICTIONARY ? Dictionary(cb["input"]) : Dictionary();
				_append_line(String::utf8("\xe2\x96\xb8 ") + HitboxTools::describe_call(name, tool_input), _color(SNAME("font_disabled_color")));
				_set_status(vformat(TTR("Running %s..."), name));
			} else {
				const bool is_error = cb.get("is_error", false);
				String text;
				const Variant result_content = cb.get("content", Variant());
				if (result_content.get_type() == Variant::STRING) {
					text = result_content;
				} else if (result_content.get_type() == Variant::ARRAY) {
					const Array parts = result_content;
					for (int i = 0; i < parts.size(); i++) {
						if (parts[i].get_type() == Variant::DICTIONARY) {
							text += String(Dictionary(parts[i]).get("text", ""));
						}
					}
				}
				_append_line(String::utf8(is_error ? "  \xe2\x9c\x97 " : "  \xe2\x86\x92 ") + text.get_slice("\n", 0).left(110), is_error ? _color(SNAME("error_color")) : _color(SNAME("font_disabled_color")));
				_set_status(TTR("Thinking..."));
			}
		} else {
			// redacted_thinking, fallback markers, anything new: keep verbatim so it replays unchanged.
			block = cb.duplicate(true);
		}
		if (current_blocks.size() <= idx) {
			current_blocks.resize(idx + 1);
		}
		current_blocks[idx] = block;
		return;
	}

	if (type == "content_block_delta") {
		const int idx = (int)(int64_t)p_event.get("index", 0);
		if (idx < 0 || idx >= current_blocks.size()) {
			return;
		}
		Dictionary block = current_blocks[idx];
		Dictionary delta = p_event.get("delta", Dictionary());
		const String delta_type = delta.get("type", "");
		if (delta_type == "text_delta") {
			const String s = delta.get("text", "");
			block["text"] = String(block["text"]) + s;
			_append_stream_text(s);
		} else if (delta_type == "input_json_delta") {
			partial_json[idx] += String(delta.get("partial_json", ""));
		} else if (delta_type == "thinking_delta") {
			const String s = delta.get("thinking", "");
			block["thinking"] = String(block["thinking"]) + s;
			transcript->add_text(s);
		} else if (delta_type == "signature_delta") {
			block["signature"] = delta.get("signature", "");
		}
		return;
	}

	if (type == "content_block_stop") {
		const int idx = (int)(int64_t)p_event.get("index", 0);
		if (idx >= 0 && idx < current_blocks.size()) {
			Dictionary block = current_blocks[idx];
			if (String(block.get("type", "")) == "tool_use") {
				const String raw = partial_json.has(idx) ? partial_json[idx] : String();
				if (raw.strip_edges().is_empty()) {
					block["input"] = Dictionary();
				} else {
					JSON json;
					if (json.parse(raw) == OK && json.get_data().get_type() == Variant::DICTIONARY) {
						block["input"] = json.get_data();
					} else {
						block["input"] = Dictionary();
						block["_invalid_json"] = raw;
					}
				}
			}
		}
		_close_block_rendering();
		return;
	}

	if (type == "message_delta") {
		Dictionary delta = p_event.get("delta", Dictionary());
		if (delta.has("stop_reason")) {
			current_stop_reason = String(delta["stop_reason"]);
		}
		return;
	}

	if (type == "error") {
		Dictionary error = p_event.get("error", Dictionary());
		request_failed = true;
		_close_block_rendering();
		_append_line(vformat("API error: %s", String(error.get("message", "unknown"))), _color(SNAME("error_color")));
		return;
	}
	// message_start, message_stop, ping: nothing to do.
}

void HitboxDock::_rollback_to_last_prompt() {
	// Drop everything back to (and including) the last typed prompt so the
	// history never ends in a dangling tool_use or a doubled user turn.
	while (!messages.is_empty()) {
		Dictionary m = messages[messages.size() - 1];
		messages.pop_back();
		if (String(m.get("role", "")) != "user") {
			continue;
		}
		Array content = m.get("content", Array());
		bool is_tool_result = false;
		for (int i = 0; i < content.size(); i++) {
			Dictionary b = content[i];
			if (String(b.get("type", "")) == "tool_result") {
				is_tool_result = true;
				break;
			}
		}
		if (!is_tool_result) {
			break;
		}
	}
}

void HitboxDock::_on_request_finished() {
	client->finish();
	_close_block_rendering();

	if (request_failed || cancel_requested) {
		if (cancel_requested) {
			// Keep whatever text arrived so the transcript and history agree.
			Array content;
			for (int i = 0; i < current_blocks.size(); i++) {
				Dictionary b = current_blocks[i];
				if (String(b.get("type", "")) == "text" && !String(b.get("text", "")).is_empty()) {
					Dictionary copy;
					copy["type"] = "text";
					copy["text"] = b["text"];
					content.push_back(copy);
				}
			}
			if (content.is_empty()) {
				Dictionary stopped;
				stopped["type"] = "text";
				stopped["text"] = "(stopped by the user)";
				content.push_back(stopped);
			}
			Dictionary m;
			m["role"] = "assistant";
			m["content"] = content;
			messages.push_back(m);
			_append_line(TTR("Stopped."), _color(SNAME("font_disabled_color")));
		} else {
			_rollback_to_last_prompt();
			_append_line(TTR("Your last message was not delivered. Fix the problem above and send it again."), _color(SNAME("font_disabled_color")));
		}
		busy = false;
		_set_status("");
		_update_ui();
		return;
	}

	// Commit the assistant turn exactly as received (thinking blocks included).
	Array content;
	Array tool_uses;
	for (int i = 0; i < current_blocks.size(); i++) {
		if (current_blocks[i].get_type() != Variant::DICTIONARY) {
			continue;
		}
		Dictionary b = current_blocks[i];
		if (b.is_empty()) {
			continue;
		}
		Dictionary clean;
		for (const Variant &key : b.keys()) {
			if (!String(key).begins_with("_")) {
				clean[key] = b[key];
			}
		}
		content.push_back(clean);
		if (String(b.get("type", "")) == "tool_use") {
			tool_uses.push_back(b);
		}
	}
	if (!content.is_empty()) {
		Dictionary m;
		m["role"] = "assistant";
		m["content"] = content;
		messages.push_back(m);
	}

	if (current_stop_reason == "tool_use" && !tool_uses.is_empty()) {
		const int max_rounds = (int)(int64_t)EDITOR_GET("hitbox/anthropic/max_tool_rounds");
		if (tool_rounds >= max_rounds) {
			Array results;
			for (int i = 0; i < tool_uses.size(); i++) {
				Dictionary tu = tool_uses[i];
				Dictionary tr;
				tr["type"] = "tool_result";
				tr["tool_use_id"] = tu.get("id", "");
				tr["content"] = "Tool call limit for this turn reached; the user must send a new message to continue.";
				tr["is_error"] = true;
				results.push_back(tr);
			}
			Dictionary m;
			m["role"] = "user";
			m["content"] = results;
			messages.push_back(m);
			_append_line(vformat(TTR("Stopped after %d tool rounds. Send another message to continue."), max_rounds), _color(SNAME("warning_color")));
		} else {
			tool_rounds++;
			_run_tools(tool_uses);
			_start_request();
			return;
		}
	} else if (current_stop_reason == "max_tokens") {
		_append_line(TTR("(the response hit the output token limit)"), _color(SNAME("warning_color")));
	} else if (current_stop_reason == "refusal") {
		_append_line(TTR("The model declined this request."), _color(SNAME("warning_color")));
	}

	busy = false;
	_set_status("");
	_update_ui();
}

void HitboxDock::_run_tools(const Array &p_tool_uses) {
	Array results;
	for (int i = 0; i < p_tool_uses.size(); i++) {
		Dictionary tu = p_tool_uses[i];
		const String name = tu.get("name", "");
		const String id = tu.get("id", "");
		Dictionary input = tu.get("input", Dictionary());

		bool is_error = false;
		String out;
		if (tu.has("_invalid_json")) {
			is_error = true;
			Dictionary wrapper;
			wrapper["INVALID_JSON"] = tu["_invalid_json"];
			out = JSON::stringify(wrapper);
		} else {
			_set_status(vformat(TTR("Running %s..."), name));
			out = HitboxTools::execute(name, input, is_error);
		}
		if (out.length() > HITBOX_MAX_TOOL_RESULT_CHARS) {
			out = out.left(HITBOX_MAX_TOOL_RESULT_CHARS) + vformat("\n...[truncated: %d of %d characters shown]", HITBOX_MAX_TOOL_RESULT_CHARS, out.length());
		}

		Dictionary tr;
		tr["type"] = "tool_result";
		tr["tool_use_id"] = id;
		tr["content"] = out;
		if (is_error) {
			tr["is_error"] = true;
		}
		results.push_back(tr);

		const String summary = HitboxTools::describe_call(name, input);
		const String first_line = out.get_slice("\n", 0).left(110);
		_append_line((is_error ? String::utf8("✗ ") : String::utf8("▸ ")) + summary + String::utf8("  →  ") + first_line,
				is_error ? _color(SNAME("error_color")) : _color(SNAME("font_disabled_color")));
	}

	Dictionary m;
	m["role"] = "user";
	m["content"] = results;
	messages.push_back(m);
}
