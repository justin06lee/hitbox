/**************************************************************************/
/*  register_types.cpp                                                    */
/**************************************************************************/
/*                          This file is part of:                         */
/*                                HITBOX                                  */
/*                 https://github.com/justin06lee/hitbox                  */
/**************************************************************************/
/* Hitbox is a fork of Godot Engine. Godot Engine is Copyright (c)        */
/* 2014-present Godot Engine contributors, Copyright (c) 2007-2014 Juan   */
/* Linietsky, Ariel Manzur. Licensed under the MIT license.               */
/**************************************************************************/

#include "register_types.h"

#ifdef TOOLS_ENABLED
#include "hitbox_dock.h"
#include "hitbox_editor_plugin.h"

#include "core/object/class_db.h"
#include "editor/plugins/editor_plugin.h"
#endif // TOOLS_ENABLED

void initialize_hitbox_module(ModuleInitializationLevel p_level) {
#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		GDREGISTER_INTERNAL_CLASS(HitboxDock);
		EditorPlugins::add_by_type<HitboxEditorPlugin>();
	}
#endif // TOOLS_ENABLED
}

void uninitialize_hitbox_module(ModuleInitializationLevel p_level) {
	// Nothing to tear down: the editor owns the plugin and the dock.
}
