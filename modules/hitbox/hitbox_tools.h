/**************************************************************************/
/*  hitbox_tools.h                                                        */
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
#include "core/variant/array.h"
#include "core/variant/dictionary.h"

// The tools the agent can call. Definitions are in the Anthropic tool
// schema shape; execution touches editor state and must run on the main
// thread.
class HitboxTools {
public:
	// Anthropic `tools` array for the request body.
	static Array get_tool_definitions();

	// A short description of what the developer is looking at right now:
	// project, open scene, selected nodes, active script and selection.
	static String get_editor_context();

	// Runs one tool. Returns the text to send back as the tool_result.
	static String execute(const String &p_name, const Dictionary &p_input, bool &r_is_error);

	// One-line summary of a call for the transcript, e.g. "read_file res://a.gd".
	static String describe_call(const String &p_name, const Dictionary &p_input);
};
