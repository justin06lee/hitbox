/**************************************************************************/
/*  hitbox_editor_plugin.cpp                                              */
/**************************************************************************/
/*                          This file is part of:                         */
/*                                HITBOX                                  */
/*                 https://github.com/justin06lee/hitbox                  */
/**************************************************************************/
/* Hitbox is a fork of Godot Engine. Godot Engine is Copyright (c)        */
/* 2014-present Godot Engine contributors, Copyright (c) 2007-2014 Juan   */
/* Linietsky, Ariel Manzur. Licensed under the MIT license.               */
/**************************************************************************/

#include "hitbox_editor_plugin.h"

#include "hitbox_dock.h"

#include "editor/docks/editor_dock_manager.h"
#include "editor/settings/editor_settings.h"

HitboxEditorPlugin::HitboxEditorPlugin() {
	EditorSettings *es = EditorSettings::get_singleton();

	EDITOR_DEF("hitbox/anthropic/api_key", "");
	es->add_property_hint(PropertyInfo(Variant::STRING, "hitbox/anthropic/api_key", PROPERTY_HINT_PLACEHOLDER_TEXT, "sk-ant-... (falls back to the ANTHROPIC_API_KEY environment variable)"));

	EDITOR_DEF("hitbox/anthropic/model", "claude-opus-5");
	es->add_property_hint(PropertyInfo(Variant::STRING, "hitbox/anthropic/model", PROPERTY_HINT_ENUM, "claude-opus-5,claude-sonnet-5,claude-fable-5-1,claude-haiku-4-5"));

	EDITOR_DEF("hitbox/anthropic/effort", "high");
	es->add_property_hint(PropertyInfo(Variant::STRING, "hitbox/anthropic/effort", PROPERTY_HINT_ENUM, "low,medium,high,xhigh,max"));

	EDITOR_DEF("hitbox/anthropic/max_tool_rounds", 40);
	es->add_property_hint(PropertyInfo(Variant::INT, "hitbox/anthropic/max_tool_rounds", PROPERTY_HINT_RANGE, "1,200,1"));
}

void HitboxEditorPlugin::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			dock = memnew(HitboxDock);
			EditorDockManager::get_singleton()->add_dock(dock);
		} break;

		case NOTIFICATION_EXIT_TREE: {
			if (dock) {
				EditorDockManager::get_singleton()->remove_dock(dock);
				dock->queue_free();
				dock = nullptr;
			}
		} break;
	}
}
