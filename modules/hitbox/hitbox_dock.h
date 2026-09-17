/**************************************************************************/
/*  hitbox_dock.h                                                         */
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

#include "hitbox_backend.h"

#include "core/templates/hash_map.h"
#include "core/variant/array.h"
#include "editor/docks/editor_dock.h"

class Button;
class HBoxContainer;
class HitboxClient;
class HitboxMcpServer;
class InputEvent;
class Label;
class LineEdit;
class OptionButton;
class RichTextLabel;
class TextEdit;

// The Hitbox chat dock: a conversation with an agent that has tools over the
// open project. Each request streams on a worker thread.
//
// Against the Anthropic API, tool calls come back as tool_use blocks and run
// on the main thread between requests. Against yagami, the dock's MCP server
// hands the same tools to Claude Code, which calls them inside the turn; the
// stream reports them as mcp_tool_use / mcp_tool_result blocks.
class HitboxDock : public EditorDock {
	GDCLASS(HitboxDock, EditorDock);

	enum RenderState {
		RENDER_NONE,
		RENDER_TEXT,
		RENDER_THINKING,
	};

	// UI
	OptionButton *model_button = nullptr;
	Button *new_chat_button = nullptr;
	RichTextLabel *transcript = nullptr;
	Label *status_label = nullptr;
	HBoxContainer *key_row = nullptr;
	LineEdit *key_edit = nullptr;
	Button *key_save_button = nullptr;
	TextEdit *input = nullptr;
	Button *send_button = nullptr;
	Button *stop_button = nullptr;

	// Backend.
	HitboxMcpServer *mcp_server = nullptr;
	HitboxBackendConfig backend;
	// Kind the current conversation was started on, or -1.
	int conversation_backend = -1;
	// False once a yagami older than 0.10 refused the MCP toolset.
	bool yagami_tools = true;

	// Conversation state (Anthropic message shape).
	Ref<HitboxClient> client;
	Array messages;
	bool busy = false;
	bool cancel_requested = false;
	bool request_failed = false;
	int tool_rounds = 0;

	// Per-request streaming state.
	Array current_blocks;
	HashMap<int, String> partial_json;
	String current_stop_reason;
	RenderState render_state = RENDER_NONE;
	bool assistant_header_shown = false;
	bool in_code_fence = false;
	// Swallowing the language tag after an opening fence (```gdscript).
	bool skipping_fence_tag = false;
	int backtick_run = 0;
	bool transcript_empty = true;

	void _send();
	void _stop();
	void _new_chat();
	void _save_key();
	void _on_model_selected(int p_index);
	void _on_input_gui_input(const Ref<InputEvent> &p_event);

	void _on_client_event(const Dictionary &p_event);
	void _handle_sse(const Dictionary &p_event);
	void _start_request();
	void _on_request_finished();
	void _run_tools(const Array &p_tool_uses);
	void _rollback_to_last_prompt();
	void _on_settings_changed();
	void _show_intro();
	String _system_prompt() const;
	int _get_model_index() const;
	Dictionary _build_request(Vector<String> &r_headers);

	void _update_ui();
	void _set_status(const String &p_text);
	void _append_header(const String &p_who);
	void _append_line(const String &p_text, const Color &p_color);
	void _append_stream_text(const String &p_text);
	void _toggle_fence();
	void _close_block_rendering();
	Color _color(const StringName &p_name) const;

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	// Scriptable surface: used by the smoke test and available to editor plugins.
	void send_prompt(const String &p_text);
	bool is_busy() const { return busy; }
	String get_transcript_text() const;
	String get_backend_label() const;
	int get_mcp_call_count() const;

	HitboxDock();
	~HitboxDock();
};
