/**************************************************************************/
/*  hitbox_editor_plugin.h                                                */
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

#include "editor/plugins/editor_plugin.h"

class HitboxDock;

// Registers the Hitbox editor settings and mounts the AI dock.
class HitboxEditorPlugin : public EditorPlugin {
	GDCLASS(HitboxEditorPlugin, EditorPlugin);

	HitboxDock *dock = nullptr;

protected:
	void _notification(int p_what);
	static void _bind_methods() {}

public:
	virtual String get_plugin_name() const override { return "Hitbox"; }

	HitboxEditorPlugin();
};
