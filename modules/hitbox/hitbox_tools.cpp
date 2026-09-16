/**************************************************************************/
/*  hitbox_tools.cpp                                                      */
/**************************************************************************/
/*                          This file is part of:                         */
/*                                HITBOX                                  */
/*                 https://github.com/justin06lee/hitbox                  */
/**************************************************************************/
/* Hitbox is a fork of Godot Engine. Godot Engine is Copyright (c)        */
/* 2014-present Godot Engine contributors, Copyright (c) 2007-2014 Juan   */
/* Linietsky, Ariel Manzur. Licensed under the MIT license.               */
/**************************************************************************/

#include "hitbox_tools.h"

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/io/resource.h"
#include "core/io/resource_loader.h"
#include "core/object/class_db.h"
#include "core/object/script_language.h"
#include "core/templates/hash_set.h"
#include "core/variant/variant_utility.h"
#include "editor/doc/editor_help.h"
#include "editor/docks/scene_tree_dock.h"
#include "editor/editor_data.h"
#include "editor/editor_interface.h"
#include "editor/editor_log.h"
#include "editor/editor_node.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/script/script_editor_base.h"
#include "editor/script/script_editor_plugin.h"
#include "scene/gui/code_edit.h"
#include "scene/main/node.h"
#include "scene/resources/packed_scene.h"

static const int HITBOX_MAX_READ_CHARS = 262144;
static const int HITBOX_MAX_LIST_ENTRIES = 3000;
static const int HITBOX_MAX_SEARCH_RESULTS = 200;
static const int HITBOX_MAX_SEARCH_FILES = 20000;
static const int HITBOX_MAX_TREE_NODES = 2000;
static const int HITBOX_MAX_DOC_CHARS = 40000;

/* ---------------------------------------------------------------------- */
/* Helpers                                                                 */
/* ---------------------------------------------------------------------- */

static String _norm_path(const String &p_path) {
	String s = p_path.strip_edges();
	if (s.is_empty()) {
		return String();
	}
	if (s.begins_with("res://")) {
		return s.simplify_path();
	}
	if (s.begins_with("/")) {
		s = s.substr(1);
	}
	return ("res://" + s).simplify_path();
}

static bool _path_ok(const String &p_res) {
	return p_res.begins_with("res://") && !p_res.contains("..");
}

static bool _write_ok(const String &p_res) {
	return _path_ok(p_res) && !p_res.begins_with("res://.godot") && !p_res.begins_with("res://.git");
}

static String _truncate(const String &p_text, int p_max) {
	if (p_text.length() <= p_max) {
		return p_text;
	}
	return p_text.left(p_max) + vformat("\n...[truncated: %d of %d characters shown]", p_max, p_text.length());
}

static bool _is_binary_ext(const String &p_ext) {
	static const char *exts[] = { "png", "jpg", "jpeg", "webp", "bmp", "tga", "dds", "ktx", "exr", "hdr", "ico", "icns",
		"wav", "ogg", "mp3", "ttf", "otf", "woff", "woff2", "fbx", "blend", "glb", "res", "scn", "stex", "ctex", "oxt",
		"zip", "pck", "dll", "so", "dylib", "exe", "a", "lib", "aab", "apk", "ipa", "dmg", nullptr };
	const String e = p_ext.to_lower();
	for (int i = 0; exts[i]; i++) {
		if (e == exts[i]) {
			return true;
		}
	}
	return false;
}

static bool _is_text_ext(const String &p_ext) {
	static const char *exts[] = { "gd", "tscn", "tres", "cfg", "godot", "gdshader", "gdshaderinc", "cs", "json", "txt", "md",
		"csv", "xml", "ini", "glsl", "shader", "gdextension", "po", "html", "js", "yaml", "yml", "toml", "sh", "py", "tet",
		"import", "svg", "gltf", "obj", "mtl", "csproj", "sln", "props", nullptr };
	const String e = p_ext.to_lower();
	for (int i = 0; exts[i]; i++) {
		if (e == exts[i]) {
			return true;
		}
	}
	return false;
}

static String _strip_bbcode(const String &p_text) {
	String out;
	bool in_tag = false;
	for (int i = 0; i < p_text.length(); i++) {
		const char32_t c = p_text[i];
		if (c == '[') {
			in_tag = true;
			continue;
		}
		if (c == ']' && in_tag) {
			in_tag = false;
			continue;
		}
		if (!in_tag) {
			out += c;
		}
	}
	return out;
}

static String _first_sentence(const String &p_desc, int p_max = 220) {
	String s = _strip_bbcode(p_desc).strip_edges().replace("\n", " ");
	int dot = s.find(". ");
	if (dot != -1 && dot < p_max) {
		return s.left(dot + 1);
	}
	if (s.length() > p_max) {
		return s.left(p_max) + "...";
	}
	return s;
}

static void _walk(const String &p_dir, bool p_recursive, Vector<String> &r_out, int p_max, bool p_files_only) {
	if (r_out.size() >= p_max) {
		return;
	}
	Ref<DirAccess> da = DirAccess::open(p_dir);
	if (da.is_null()) {
		return;
	}
	da->list_dir_begin();
	String n = da->get_next();
	Vector<String> dirs;
	Vector<String> files;
	while (!n.is_empty()) {
		if (n == "." || n == ".." || n.begins_with(".")) {
			n = da->get_next();
			continue;
		}
		if (da->current_is_dir()) {
			dirs.push_back(n);
		} else if (!n.ends_with(".import") && !n.ends_with(".uid")) {
			files.push_back(n);
		}
		n = da->get_next();
	}
	da->list_dir_end();
	dirs.sort();
	files.sort();

	for (const String &d : dirs) {
		const String full = p_dir.path_join(d);
		if (!p_files_only) {
			r_out.push_back(full + "/");
		}
		if (p_recursive) {
			_walk(full, true, r_out, p_max, p_files_only);
		}
		if (r_out.size() >= p_max) {
			return;
		}
	}
	for (const String &f : files) {
		r_out.push_back(p_dir.path_join(f));
		if (r_out.size() >= p_max) {
			return;
		}
	}
}

static void _after_write(const String &p_path, bool p_made_dirs) {
	EditorFileSystem *efs = EditorFileSystem::get_singleton();
	if (efs) {
		if (p_made_dirs) {
			efs->scan_changes();
		} else {
			efs->update_file(p_path);
		}
	}

	const String ext = p_path.get_extension().to_lower();
	if (ext == "tscn" || ext == "scn" || ext == "escn") {
		PackedStringArray open = EditorInterface::get_singleton()->get_open_scenes();
		if (open.has(p_path)) {
			EditorInterface::get_singleton()->reload_scene_from_path(p_path);
		}
	} else if (ext == "gd" || ext == "cs" || ext == "gdshader" || ext == "gdshaderinc" || ext == "json") {
		if (ScriptEditor::get_singleton()) {
			ScriptEditor::get_singleton()->reload_scripts();
		}
	} else if (ext == "tres" || ext == "res") {
		if (ResourceCache::has(p_path)) {
			Ref<Resource> r = ResourceCache::get_ref(p_path);
			if (r.is_valid()) {
				r->reload_from_file();
			}
		}
	}
}

static Node *_scene_root() {
	return EditorInterface::get_singleton()->get_edited_scene_root();
}

static Node *_find_node(const String &p_path, String &r_error) {
	Node *root = _scene_root();
	if (!root) {
		r_error = "No scene is open in the editor.";
		return nullptr;
	}
	String p = p_path.strip_edges();
	if (p.is_empty() || p == "." || p == "/") {
		return root;
	}
	if (p.begins_with("/root/")) {
		// Absolute paths from a running game: strip down to the scene root's name.
		p = p.substr(6);
		if (p.begins_with(String(root->get_name()) + "/")) {
			p = p.substr(String(root->get_name()).length() + 1);
		} else if (p == String(root->get_name())) {
			return root;
		}
	}
	Node *n = root->get_node_or_null(NodePath(p));
	if (!n) {
		r_error = vformat("No node at path \"%s\" (paths are relative to the scene root; use get_scene_tree to list them).", p_path);
	}
	return n;
}

static String _value_to_text(const Variant &p_value) {
	if (p_value.get_type() == Variant::OBJECT) {
		Object *o = p_value;
		if (!o) {
			return "null";
		}
		Resource *r = Object::cast_to<Resource>(o);
		if (r && !r->get_path().is_empty()) {
			return vformat("%s(\"%s\")", r->get_class(), r->get_path());
		}
		return vformat("%s (built-in)", o->get_class());
	}
	String s = VariantUtilityFunctions::var_to_str(p_value).replace("\n", " ");
	if (s.length() > 300) {
		s = s.left(300) + "...";
	}
	return s;
}

static Variant _coerce_value(const Variant &p_value, const PropertyInfo &p_target, String &r_error) {
	if (p_value.get_type() == Variant::STRING) {
		const String s = p_value;
		if (p_target.type == Variant::OBJECT) {
			if (s.is_empty() || s == "null") {
				return Variant();
			}
			const String path = _norm_path(s);
			Ref<Resource> r = ResourceLoader::load(path);
			if (r.is_null()) {
				r_error = vformat("Could not load resource \"%s\".", path);
			}
			return r;
		}
		if (p_target.type == Variant::STRING) {
			return s;
		}
		if (p_target.type == Variant::STRING_NAME) {
			return StringName(s);
		}
		if (p_target.type == Variant::NODE_PATH) {
			return NodePath(s);
		}
		Variant parsed = VariantUtilityFunctions::str_to_var(s);
		if (parsed.get_type() != Variant::NIL) {
			return parsed;
		}
		return s;
	}

	if (p_value.get_type() == Variant::ARRAY) {
		Array a = p_value;
		bool numeric = true;
		for (int i = 0; i < a.size(); i++) {
			if (a[i].get_type() != Variant::FLOAT && a[i].get_type() != Variant::INT) {
				numeric = false;
				break;
			}
		}
		if (numeric) {
			switch (p_target.type) {
				case Variant::VECTOR2:
					if (a.size() == 2) {
						return Vector2(a[0], a[1]);
					}
					break;
				case Variant::VECTOR2I:
					if (a.size() == 2) {
						return Vector2i(a[0], a[1]);
					}
					break;
				case Variant::VECTOR3:
					if (a.size() == 3) {
						return Vector3(a[0], a[1], a[2]);
					}
					break;
				case Variant::VECTOR3I:
					if (a.size() == 3) {
						return Vector3i(a[0], a[1], a[2]);
					}
					break;
				case Variant::VECTOR4:
					if (a.size() == 4) {
						return Vector4(a[0], a[1], a[2], a[3]);
					}
					break;
				case Variant::COLOR:
					if (a.size() == 3) {
						return Color(a[0], a[1], a[2]);
					}
					if (a.size() == 4) {
						return Color(a[0], a[1], a[2], a[3]);
					}
					break;
				default:
					break;
			}
		}
	}

	if (p_target.type == Variant::INT && p_value.get_type() == Variant::FLOAT) {
		return (int64_t)(double)p_value;
	}
	if (p_target.type == Variant::FLOAT && p_value.get_type() == Variant::INT) {
		return (double)(int64_t)p_value;
	}
	if (p_target.type == Variant::STRING_NAME && p_value.get_type() != Variant::STRING_NAME) {
		return StringName(String(p_value));
	}
	return p_value;
}

/* ---------------------------------------------------------------------- */
/* Tool schema                                                             */
/* ---------------------------------------------------------------------- */

static Dictionary _prop(const String &p_type, const String &p_desc) {
	Dictionary d;
	d["type"] = p_type;
	d["description"] = p_desc;
	return d;
}

static Array _req(const char *p_a, const char *p_b = nullptr, const char *p_c = nullptr) {
	Array r;
	if (p_a) {
		r.push_back(String(p_a));
	}
	if (p_b) {
		r.push_back(String(p_b));
	}
	if (p_c) {
		r.push_back(String(p_c));
	}
	return r;
}

static Dictionary _tool(const String &p_name, const String &p_desc, const Dictionary &p_props, const Array &p_required) {
	Dictionary schema;
	schema["type"] = "object";
	schema["properties"] = p_props;
	schema["required"] = p_required;
	schema["additionalProperties"] = false;

	Dictionary t;
	t["name"] = p_name;
	t["description"] = p_desc;
	t["input_schema"] = schema;
	t["eager_input_streaming"] = true;
	return t;
}

Array HitboxTools::get_tool_definitions() {
	Array tools;

	{
		Dictionary p;
		p["path"] = _prop("string", "Directory to list, e.g. \"res://\" or \"res://scenes\". Defaults to the project root.");
		p["recursive"] = _prop("boolean", "Recurse into subdirectories. Defaults to true.");
		tools.push_back(_tool("list_files", "List files and directories in the project. Hidden entries, the .godot/ cache and .import sidecars are skipped. Directories end with \"/\".", p, _req(nullptr)));
	}
	{
		Dictionary p;
		p["path"] = _prop("string", "Project path such as \"res://player.gd\".");
		tools.push_back(_tool("read_file", "Read a text file from the project and return its full contents.", p, _req("path")));
	}
	{
		Dictionary p;
		p["path"] = _prop("string", "Project path such as \"res://scripts/enemy.gd\". Parent directories are created.");
		p["content"] = _prop("string", "The complete new contents of the file.");
		tools.push_back(_tool("write_file", "Create or overwrite a text file. The editor picks the change up immediately: open scripts reload, and if the file is a scene that is open in the editor it is reloaded from disk (discarding unsaved editor changes to that scene).", p, _req("path", "content")));
	}
	{
		Dictionary p;
		p["path"] = _prop("string", "Project path of an existing file.");
		p["old_string"] = _prop("string", "Exact text to find. Must appear exactly once unless replace_all is true. Include enough surrounding lines to make it unique.");
		p["new_string"] = _prop("string", "Replacement text.");
		p["replace_all"] = _prop("boolean", "Replace every occurrence instead of requiring a unique match. Defaults to false.");
		tools.push_back(_tool("edit_file", "Replace an exact string in an existing file. Prefer this over write_file for changes to existing files. The editor picks the change up immediately.", p, _req("path", "old_string", "new_string")));
	}
	{
		Dictionary p;
		p["query"] = _prop("string", "Text to search for (case-insensitive substring).");
		p["path"] = _prop("string", "Directory to search under. Defaults to the project root.");
		tools.push_back(_tool("search_files", "Search all text files in the project for a string. Returns \"path:line: text\" matches.", p, _req("query")));
	}
	{
		Dictionary p;
		tools.push_back(_tool("get_scene_tree", "Dump the node tree of the scene currently open in the editor: each node's path relative to the root (\".\" is the root), type, attached script, instanced scene and whether it is selected.", p, _req(nullptr)));
	}
	{
		Dictionary p;
		p["node_path"] = _prop("string", "Node path relative to the scene root, e.g. \".\" or \"Player/Sprite2D\".");
		tools.push_back(_tool("get_node_properties", "List the editor-visible properties of a node that differ from the class defaults (plus all script-exported variables), as \"name = value\" in Godot literal syntax.", p, _req("node_path")));
	}
	{
		Dictionary p;
		p["node_path"] = _prop("string", "Node path relative to the scene root.");
		p["property"] = _prop("string", "Property name, e.g. \"position\", \"texture\", \"speed\".");
		p["value"] = _prop("string", "New value. Use Godot literal syntax for engine types: \"Vector2(10, 20)\", \"Color(1, 0, 0)\", \"true\", \"3.5\", \"\\\"text\\\"\" for strings, or a \"res://\" path for resource properties such as texture or script.");
		tools.push_back(_tool("set_node_property", "Set a property on a node in the open scene. Undoable from the editor. Numbers, booleans and strings can also be passed as-is.", p, _req("node_path", "property", "value")));
	}
	{
		Dictionary p;
		p["parent_path"] = _prop("string", "Path of the parent node relative to the scene root (\".\" for the root). Pass \"\" when the scene has no root yet to create the root node.");
		p["type"] = _prop("string", "Engine class to instantiate, e.g. \"Sprite2D\", \"CharacterBody2D\", \"Area2D\", \"CollisionShape2D\". Ignored when scene_path is given.");
		p["name"] = _prop("string", "Name for the new node. Defaults to the type name.");
		p["scene_path"] = _prop("string", "Optional \"res://\" path of a .tscn to instance instead of creating a plain node.");
		tools.push_back(_tool("add_node", "Add a node to the open scene (undoable). Returns the new node's path. Follow with set_node_property to configure it.", p, _req("parent_path")));
	}
	{
		Dictionary p;
		p["node_path"] = _prop("string", "Node path relative to the scene root. The root itself cannot be removed.");
		tools.push_back(_tool("remove_node", "Remove a node (and its children) from the open scene. Undoable.", p, _req("node_path")));
	}
	{
		Dictionary p;
		p["node_path"] = _prop("string", "Node path relative to the scene root.");
		p["script_path"] = _prop("string", "\"res://\" path of an existing script file. Write it first with write_file.");
		tools.push_back(_tool("attach_script", "Attach a script file to a node in the open scene. Undoable.", p, _req("node_path", "script_path")));
	}
	{
		Dictionary p;
		tools.push_back(_tool("save_all", "Save every open scene to disk. Call this after scene edits so they persist and so run_project sees them.", p, _req(nullptr)));
	}
	{
		Dictionary p;
		p["path"] = _prop("string", "\"res://\" path of a .tscn file.");
		tools.push_back(_tool("open_scene", "Open a scene in the editor and make it the current scene.", p, _req("path")));
	}
	{
		Dictionary p;
		p["path"] = _prop("string", "\"res://\" path of a script or shader.");
		tools.push_back(_tool("open_script", "Open a script in the editor's script view so the developer can see it.", p, _req("path")));
	}
	{
		Dictionary p;
		p["scene"] = _prop("string", "\"main\" (default) to run the project's main scene, \"current\" to run the scene open in the editor, or a \"res://\" scene path.");
		tools.push_back(_tool("run_project", "Run the game from the editor. Save first. Afterwards use get_output_log to read prints and errors, and stop_project to stop it.", p, _req(nullptr)));
	}
	{
		Dictionary p;
		tools.push_back(_tool("stop_project", "Stop the running game.", p, _req(nullptr)));
	}
	{
		Dictionary p;
		p["max_lines"] = _prop("integer", "How many of the most recent lines to return. Defaults to 80.");
		tools.push_back(_tool("get_output_log", "Return the most recent lines of the editor's Output panel: prints, warnings and errors from the editor and from the running game.", p, _req(nullptr)));
	}
	{
		Dictionary p;
		p["class_name"] = _prop("string", "Engine class name in PascalCase, e.g. \"CharacterBody2D\", \"Tween\", \"Input\", \"@GlobalScope\".");
		tools.push_back(_tool("get_class_docs", "Class reference for an engine class from this exact engine build: inheritance, properties with defaults, method signatures, signals and constants. Use it whenever you are not certain about an API.", p, _req("class_name")));
	}

	return tools;
}

/* ---------------------------------------------------------------------- */
/* Editor context                                                          */
/* ---------------------------------------------------------------------- */

String HitboxTools::get_editor_context() {
	String out = "<editor_context>\n";

	const String project_name = GLOBAL_GET("application/config/name");
	out += "Project: " + (project_name.is_empty() ? String("(unnamed)") : project_name) + "\n";
	const String main_scene = GLOBAL_GET("application/run/main_scene");
	out += "Main scene: " + (main_scene.is_empty() ? String("(not set)") : main_scene) + "\n";

	Node *root = _scene_root();
	if (root) {
		const String path = root->get_scene_file_path();
		out += vformat("Open scene: %s (root \"%s\", type %s)\n", path.is_empty() ? String("(unsaved)") : path, String(root->get_name()), root->get_class());
		List<Node *> selection = EditorNode::get_singleton()->get_editor_selection()->get_top_selected_node_list();
		if (!selection.is_empty()) {
			out += "Selected nodes:";
			for (Node *n : selection) {
				out += vformat(" %s (%s)", n == root ? String(".") : String(root->get_path_to(n)), n->get_class());
			}
			out += "\n";
		}
	} else {
		out += "Open scene: none\n";
	}

	ScriptEditor *se = ScriptEditor::get_singleton();
	if (se) {
		ScriptEditorBase *seb = se->get_current_editor();
		if (seb) {
			Ref<Resource> res = seb->get_edited_resource();
			if (res.is_valid() && !res->get_path().is_empty()) {
				out += "Active script: " + res->get_path();
				CodeEdit *ce = Object::cast_to<CodeEdit>(seb->get_base_editor());
				if (ce) {
					out += vformat(" (caret at line %d)", ce->get_caret_line() + 1);
					if (ce->has_selection()) {
						out += "\nSelected text:\n" + _truncate(ce->get_selected_text(), 2000);
					}
				}
				out += "\n";
			}
		}
	}

	if (EditorInterface::get_singleton()->is_playing_scene()) {
		out += "Game: running (" + EditorInterface::get_singleton()->get_playing_scene() + ")\n";
	}

	out += "</editor_context>";
	return out;
}

/* ---------------------------------------------------------------------- */
/* Tool implementations                                                    */
/* ---------------------------------------------------------------------- */

#define HITBOX_ARG_STRING(m_var, m_key)                                                \
	if (!p_input.has(m_key) || p_input[m_key].get_type() != Variant::STRING) {         \
		r_is_error = true;                                                             \
		return vformat("Missing or non-string required parameter \"%s\".", m_key);     \
	}                                                                                  \
	const String m_var = p_input[m_key];

static String _tool_list_files(const Dictionary &p_input, bool &r_is_error) {
	String dir = _norm_path(p_input.get("path", "res://"));
	if (dir.is_empty()) {
		dir = "res://";
	}
	if (!_path_ok(dir)) {
		r_is_error = true;
		return "Invalid path.";
	}
	if (!DirAccess::exists(dir)) {
		r_is_error = true;
		return vformat("Directory not found: %s", dir);
	}
	const bool recursive = (bool)p_input.get("recursive", true);
	Vector<String> entries;
	_walk(dir, recursive, entries, HITBOX_MAX_LIST_ENTRIES, false);
	if (entries.is_empty()) {
		return vformat("%s is empty.", dir);
	}
	String out;
	for (const String &e : entries) {
		out += e + "\n";
	}
	if (entries.size() >= HITBOX_MAX_LIST_ENTRIES) {
		out += vformat("...[listing capped at %d entries; narrow the path]\n", HITBOX_MAX_LIST_ENTRIES);
	}
	return out;
}

static String _tool_read_file(const Dictionary &p_input, bool &r_is_error) {
	HITBOX_ARG_STRING(raw_path, "path");
	const String path = _norm_path(raw_path);
	if (!_path_ok(path)) {
		r_is_error = true;
		return "Invalid path.";
	}
	if (!FileAccess::exists(path)) {
		r_is_error = true;
		return vformat("File not found: %s", path);
	}
	if (_is_binary_ext(path.get_extension())) {
		r_is_error = true;
		return vformat("%s is a binary file and cannot be read as text.", path);
	}
	Error err = OK;
	const String text = FileAccess::get_file_as_string(path, &err);
	if (err != OK) {
		r_is_error = true;
		return vformat("Could not read %s (error %d).", path, (int)err);
	}
	return _truncate(text, HITBOX_MAX_READ_CHARS);
}

static String _tool_write_file(const Dictionary &p_input, bool &r_is_error) {
	HITBOX_ARG_STRING(raw_path, "path");
	HITBOX_ARG_STRING(content, "content");
	const String path = _norm_path(raw_path);
	if (!_write_ok(path)) {
		r_is_error = true;
		return "Invalid or protected path.";
	}
	const String dir = path.get_base_dir();
	bool made_dirs = false;
	if (!DirAccess::exists(dir)) {
		Ref<DirAccess> da = DirAccess::open("res://");
		if (da.is_null() || da->make_dir_recursive(dir) != OK) {
			r_is_error = true;
			return vformat("Could not create directory %s.", dir);
		}
		made_dirs = true;
	}
	const bool existed = FileAccess::exists(path);
	Error err = OK;
	Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE, &err);
	if (f.is_null()) {
		r_is_error = true;
		return vformat("Could not open %s for writing (error %d).", path, (int)err);
	}
	f->store_string(content);
	f.unref();
	_after_write(path, made_dirs);
	return vformat("%s %s (%d bytes).", existed ? "Overwrote" : "Created", path, content.utf8().length());
}

static String _tool_edit_file(const Dictionary &p_input, bool &r_is_error) {
	HITBOX_ARG_STRING(raw_path, "path");
	HITBOX_ARG_STRING(old_string, "old_string");
	HITBOX_ARG_STRING(new_string, "new_string");
	const bool replace_all = (bool)p_input.get("replace_all", false);
	const String path = _norm_path(raw_path);
	if (!_write_ok(path)) {
		r_is_error = true;
		return "Invalid or protected path.";
	}
	if (!FileAccess::exists(path)) {
		r_is_error = true;
		return vformat("File not found: %s", path);
	}
	if (old_string.is_empty()) {
		r_is_error = true;
		return "old_string must not be empty.";
	}
	Error err = OK;
	const String text = FileAccess::get_file_as_string(path, &err);
	if (err != OK) {
		r_is_error = true;
		return vformat("Could not read %s (error %d).", path, (int)err);
	}
	const int count = text.count(old_string);
	if (count == 0) {
		r_is_error = true;
		return vformat("old_string was not found in %s. Read the file and copy the exact text.", path);
	}
	if (count > 1 && !replace_all) {
		r_is_error = true;
		return vformat("old_string matches %d times in %s. Include more context to make it unique, or set replace_all.", count, path);
	}
	String out;
	if (replace_all) {
		out = text.replace(old_string, new_string);
	} else {
		const int idx = text.find(old_string);
		out = text.substr(0, idx) + new_string + text.substr(idx + old_string.length());
	}
	Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE, &err);
	if (f.is_null()) {
		r_is_error = true;
		return vformat("Could not open %s for writing (error %d).", path, (int)err);
	}
	f->store_string(out);
	f.unref();
	_after_write(path, false);
	return vformat("Replaced %d occurrence%s in %s.", replace_all ? count : 1, (replace_all && count != 1) ? "s" : "", path);
}

static String _tool_search_files(const Dictionary &p_input, bool &r_is_error) {
	HITBOX_ARG_STRING(query, "query");
	if (query.strip_edges().is_empty()) {
		r_is_error = true;
		return "query must not be empty.";
	}
	String dir = _norm_path(p_input.get("path", "res://"));
	if (dir.is_empty()) {
		dir = "res://";
	}
	if (!_path_ok(dir) || !DirAccess::exists(dir)) {
		r_is_error = true;
		return vformat("Directory not found: %s", dir);
	}
	Vector<String> files;
	_walk(dir, true, files, HITBOX_MAX_SEARCH_FILES, true);

	Vector<String> results;
	for (const String &file : files) {
		if (!_is_text_ext(file.get_extension())) {
			continue;
		}
		Error err = OK;
		const String text = FileAccess::get_file_as_string(file, &err);
		if (err != OK) {
			continue;
		}
		Vector<String> lines = text.split("\n");
		for (int i = 0; i < lines.size(); i++) {
			if (lines[i].findn(query) != -1) {
				results.push_back(vformat("%s:%d: %s", file, i + 1, lines[i].strip_edges().left(200)));
				if (results.size() >= HITBOX_MAX_SEARCH_RESULTS) {
					break;
				}
			}
		}
		if (results.size() >= HITBOX_MAX_SEARCH_RESULTS) {
			break;
		}
	}
	if (results.is_empty()) {
		return "No matches.";
	}
	String out;
	for (const String &r : results) {
		out += r + "\n";
	}
	if (results.size() >= HITBOX_MAX_SEARCH_RESULTS) {
		out += vformat("...[capped at %d results]\n", HITBOX_MAX_SEARCH_RESULTS);
	}
	return out;
}

static void _dump_node(Node *p_node, Node *p_root, int p_depth, const HashSet<Node *> &p_selected, String &r_out, int &r_count) {
	if (r_count >= HITBOX_MAX_TREE_NODES) {
		return;
	}
	r_count++;
	String line = String("  ").repeat(p_depth);
	line += p_node == p_root ? String(".") : String(p_root->get_path_to(p_node));
	line += " (" + p_node->get_class() + ")";
	Ref<Script> scr = p_node->get_script();
	if (scr.is_valid()) {
		line += " script=" + (scr->get_path().is_empty() ? String("(built-in)") : scr->get_path());
	}
	if (p_node != p_root && !p_node->get_scene_file_path().is_empty()) {
		line += " instance=" + p_node->get_scene_file_path();
	}
	if (p_node != p_root && p_node->get_owner() != p_root) {
		line += " [inside instance]";
	}
	if (p_selected.has(p_node)) {
		line += " [selected]";
	}
	r_out += line + "\n";
	for (int i = 0; i < p_node->get_child_count(false); i++) {
		_dump_node(p_node->get_child(i, false), p_root, p_depth + 1, p_selected, r_out, r_count);
	}
}

static String _tool_get_scene_tree(const Dictionary &p_input, bool &r_is_error) {
	Node *root = _scene_root();
	if (!root) {
		return "No scene is open in the editor, or the open scene is empty. Use add_node with parent_path \"\" to create a root node, or open_scene to open one.";
	}
	HashSet<Node *> selected;
	List<Node *> selection = EditorNode::get_singleton()->get_editor_selection()->get_top_selected_node_list();
	for (Node *n : selection) {
		selected.insert(n);
	}
	const String path = root->get_scene_file_path();
	String out = "Scene: " + (path.is_empty() ? String("(unsaved)") : path) + "\n";
	int count = 0;
	_dump_node(root, root, 0, selected, out, count);
	if (count >= HITBOX_MAX_TREE_NODES) {
		out += vformat("...[tree capped at %d nodes]\n", HITBOX_MAX_TREE_NODES);
	}
	return out;
}

static String _tool_get_node_properties(const Dictionary &p_input, bool &r_is_error) {
	HITBOX_ARG_STRING(node_path, "node_path");
	String err;
	Node *node = _find_node(node_path, err);
	if (!node) {
		r_is_error = true;
		return err;
	}
	Node *root = _scene_root();
	String out = vformat("Node: %s (%s)\n", node == root ? String(".") : String(root->get_path_to(node)), node->get_class());
	Ref<Script> scr = node->get_script();
	if (scr.is_valid()) {
		out += "script = " + (scr->get_path().is_empty() ? String("(built-in)") : scr->get_path()) + "\n";
	}

	List<PropertyInfo> plist;
	node->get_property_list(&plist);
	for (const PropertyInfo &pi : plist) {
		if (!(pi.usage & PROPERTY_USAGE_EDITOR)) {
			continue;
		}
		if (pi.usage & (PROPERTY_USAGE_CATEGORY | PROPERTY_USAGE_GROUP | PROPERTY_USAGE_SUBGROUP | PROPERTY_USAGE_INTERNAL)) {
			continue;
		}
		if (pi.type == Variant::NIL || pi.name == "script") {
			continue;
		}
		bool valid = false;
		Variant value = node->get(pi.name, &valid);
		if (!valid) {
			continue;
		}
		bool has_default = false;
		Variant def = ClassDB::class_get_default_property_value(node->get_class(), pi.name, &has_default);
		if (has_default && value == def) {
			continue;
		}
		out += pi.name + " = " + _value_to_text(value) + "\n";
	}
	return out;
}

static String _tool_set_node_property(const Dictionary &p_input, bool &r_is_error) {
	HITBOX_ARG_STRING(node_path, "node_path");
	HITBOX_ARG_STRING(property, "property");
	if (!p_input.has("value")) {
		r_is_error = true;
		return "Missing required parameter \"value\".";
	}
	String err;
	Node *node = _find_node(node_path, err);
	if (!node) {
		r_is_error = true;
		return err;
	}
	bool valid = false;
	Variant old_value = node->get(property, &valid);
	if (!valid) {
		r_is_error = true;
		return vformat("%s has no property \"%s\". Use get_node_properties or get_class_docs to find the right name.", node->get_class(), property);
	}
	PropertyInfo target;
	target.type = old_value.get_type();
	List<PropertyInfo> plist;
	node->get_property_list(&plist);
	for (const PropertyInfo &pi : plist) {
		if (pi.name == property) {
			target = pi;
			break;
		}
	}
	String coerce_err;
	Variant new_value = _coerce_value(p_input["value"], target, coerce_err);
	if (!coerce_err.is_empty()) {
		r_is_error = true;
		return coerce_err;
	}

	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action(vformat("Hitbox: Set %s.%s", node->get_name(), property));
	ur->add_do_property(node, property, new_value);
	ur->add_undo_property(node, property, old_value);
	ur->commit_action();

	Variant applied = node->get(property);
	return vformat("Set %s.%s = %s", node->get_name(), property, _value_to_text(applied));
}

static String _tool_add_node(const Dictionary &p_input, bool &r_is_error) {
	HITBOX_ARG_STRING(parent_path, "parent_path");
	const String type = p_input.get("type", "");
	const String name = p_input.get("name", "");
	const String scene_path_raw = p_input.get("scene_path", "");

	Node *node = nullptr;
	if (!scene_path_raw.is_empty()) {
		const String scene_path = _norm_path(scene_path_raw);
		if (!_path_ok(scene_path) || !FileAccess::exists(scene_path)) {
			r_is_error = true;
			return vformat("Scene not found: %s", scene_path);
		}
		Ref<PackedScene> ps = ResourceLoader::load(scene_path, "PackedScene");
		if (ps.is_null()) {
			r_is_error = true;
			return vformat("Could not load %s as a scene.", scene_path);
		}
		node = ps->instantiate(PackedScene::GEN_EDIT_STATE_INSTANCE);
		if (!node) {
			r_is_error = true;
			return vformat("Could not instantiate %s.", scene_path);
		}
		if (!name.is_empty()) {
			node->set_name(name);
		}
	} else {
		if (type.is_empty()) {
			r_is_error = true;
			return "Pass either type (an engine class) or scene_path.";
		}
		if (!ClassDB::class_exists(type)) {
			r_is_error = true;
			return vformat("Unknown class \"%s\". Godot class names are PascalCase (Sprite2D, CharacterBody2D, Area3D...).", type);
		}
		if (!ClassDB::can_instantiate(type) || !ClassDB::is_parent_class(type, "Node")) {
			r_is_error = true;
			return vformat("\"%s\" is not an instantiable Node class.", type);
		}
		Object *obj = ClassDB::instantiate(type);
		node = Object::cast_to<Node>(obj);
		if (!node) {
			if (obj) {
				memdelete(obj);
			}
			r_is_error = true;
			return vformat("\"%s\" is not a Node.", type);
		}
		node->set_name(name.is_empty() ? type : name);
	}

	Node *root = _scene_root();
	if (!root) {
		if (!parent_path.strip_edges().is_empty() && parent_path.strip_edges() != ".") {
			memdelete(node);
			r_is_error = true;
			return "The open scene has no root node yet. Pass parent_path \"\" to create the root.";
		}
		SceneTreeDock::get_singleton()->add_root_node(node);
		return vformat("Created root node \"%s\" (%s). Its path is \".\".", node->get_name(), node->get_class());
	}

	String err;
	Node *parent = _find_node(parent_path, err);
	if (!parent) {
		memdelete(node);
		r_is_error = true;
		return err;
	}

	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action(vformat("Hitbox: Add %s", node->get_name()));
	ur->add_do_method(parent, "add_child", node, true);
	ur->add_do_method(node, "set_owner", root);
	ur->add_do_reference(node);
	ur->add_undo_method(parent, "remove_child", node);
	ur->commit_action();

	EditorSelection *sel = EditorNode::get_singleton()->get_editor_selection();
	sel->clear();
	sel->add_node(node);

	return vformat("Added \"%s\" (%s) at path %s", node->get_name(), node->get_class(), String(root->get_path_to(node)));
}

static String _tool_remove_node(const Dictionary &p_input, bool &r_is_error) {
	HITBOX_ARG_STRING(node_path, "node_path");
	String err;
	Node *node = _find_node(node_path, err);
	if (!node) {
		r_is_error = true;
		return err;
	}
	Node *root = _scene_root();
	if (node == root) {
		r_is_error = true;
		return "The scene root cannot be removed with this tool.";
	}
	Node *parent = node->get_parent();
	const int index = node->get_index(false);
	List<Node *> owned;
	node->get_owned_by(root, &owned);
	const String removed_path = String(root->get_path_to(node));

	EditorNode::get_singleton()->get_editor_selection()->clear();

	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action(vformat("Hitbox: Remove %s", node->get_name()));
	ur->add_do_method(parent, "remove_child", node);
	ur->add_undo_method(parent, "add_child", node, true);
	ur->add_undo_method(parent, "move_child", node, index);
	for (Node *o : owned) {
		ur->add_undo_method(o, "set_owner", root);
	}
	ur->add_undo_reference(node);
	ur->commit_action();

	return vformat("Removed %s.", removed_path);
}

static String _tool_attach_script(const Dictionary &p_input, bool &r_is_error) {
	HITBOX_ARG_STRING(node_path, "node_path");
	HITBOX_ARG_STRING(script_raw, "script_path");
	String err;
	Node *node = _find_node(node_path, err);
	if (!node) {
		r_is_error = true;
		return err;
	}
	const String script_path = _norm_path(script_raw);
	if (!_path_ok(script_path) || !FileAccess::exists(script_path)) {
		r_is_error = true;
		return vformat("Script not found: %s", script_path);
	}
	Ref<Script> scr = ResourceLoader::load(script_path, "Script");
	if (scr.is_null()) {
		r_is_error = true;
		return vformat("Could not load %s as a script. Check the output log for parse errors.", script_path);
	}

	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action(vformat("Hitbox: Attach %s", script_path.get_file()));
	ur->add_do_method(node, "set_script", scr);
	ur->add_undo_method(node, "set_script", node->get_script());
	ur->commit_action();

	return vformat("Attached %s to %s.", script_path, node->get_name());
}

static String _tool_save_all(const Dictionary &p_input, bool &r_is_error) {
	EditorInterface::get_singleton()->save_all_scenes();
	return "Saved all open scenes.";
}

static String _tool_open_scene(const Dictionary &p_input, bool &r_is_error) {
	HITBOX_ARG_STRING(raw_path, "path");
	const String path = _norm_path(raw_path);
	if (!_path_ok(path) || !FileAccess::exists(path)) {
		r_is_error = true;
		return vformat("Scene not found: %s", path);
	}
	EditorInterface::get_singleton()->open_scene_from_path(path);
	return vformat("Opened %s.", path);
}

static String _tool_open_script(const Dictionary &p_input, bool &r_is_error) {
	HITBOX_ARG_STRING(raw_path, "path");
	const String path = _norm_path(raw_path);
	if (!_path_ok(path) || !FileAccess::exists(path)) {
		r_is_error = true;
		return vformat("File not found: %s", path);
	}
	Ref<Resource> res = ScriptEditor::get_singleton()->open_file(path);
	if (res.is_null()) {
		r_is_error = true;
		return vformat("The script editor could not open %s.", path);
	}
	EditorInterface::get_singleton()->set_main_screen_editor("Script");
	return vformat("Opened %s in the script editor.", path);
}

static String _tool_run_project(const Dictionary &p_input, bool &r_is_error) {
	EditorInterface *ei = EditorInterface::get_singleton();
	String scene = p_input.get("scene", "main");
	scene = scene.strip_edges();
	if (ei->is_playing_scene()) {
		ei->stop_playing_scene();
	}
	if (scene.is_empty() || scene == "main") {
		const String main_scene = GLOBAL_GET("application/run/main_scene");
		if (main_scene.is_empty()) {
			r_is_error = true;
			return "The project has no main scene. Pass scene=\"current\" or a res:// scene path, or set application/run/main_scene in project.godot.";
		}
		ei->play_main_scene();
	} else if (scene == "current") {
		if (!_scene_root()) {
			r_is_error = true;
			return "No scene is open in the editor.";
		}
		ei->play_current_scene();
	} else {
		const String path = _norm_path(scene);
		if (!_path_ok(path) || !FileAccess::exists(path)) {
			r_is_error = true;
			return vformat("Scene not found: %s", path);
		}
		ei->play_custom_scene(path);
	}
	return "Game started. Wait a moment, then call get_output_log to read its prints and errors. Call stop_project when done.";
}

static String _tool_stop_project(const Dictionary &p_input, bool &r_is_error) {
	EditorInterface *ei = EditorInterface::get_singleton();
	if (!ei->is_playing_scene()) {
		return "The game is not running.";
	}
	ei->stop_playing_scene();
	return "Stopped the game.";
}

static String _tool_get_output_log(const Dictionary &p_input, bool &r_is_error) {
	int max_lines = (int)(int64_t)p_input.get("max_lines", 80);
	max_lines = CLAMP(max_lines, 1, 1000);
	EditorLog *log = EditorNode::get_log();
	if (!log) {
		r_is_error = true;
		return "The output log is not available.";
	}
	const int count = log->get_log_message_count();
	if (count == 0) {
		return "(the output log is empty)";
	}
	const int start = MAX(0, count - max_lines);
	String out;
	for (int i = start; i < count; i++) {
		String prefix;
		switch (log->get_log_message_type(i)) {
			case EditorLog::MSG_TYPE_ERROR:
				prefix = "[error] ";
				break;
			case EditorLog::MSG_TYPE_WARNING:
				prefix = "[warning] ";
				break;
			case EditorLog::MSG_TYPE_EDITOR:
				prefix = "[editor] ";
				break;
			default:
				break;
		}
		String text = log->get_log_message_text(i);
		if (log->get_log_message_type(i) == EditorLog::MSG_TYPE_STD_RICH) {
			text = _strip_bbcode(text);
		}
		out += prefix + text;
		const int repeat = log->get_log_message_repeat(i);
		if (repeat > 1) {
			out += vformat(" (x%d)", repeat);
		}
		out += "\n";
	}
	if (start > 0) {
		out = vformat("[showing the last %d of %d messages]\n", count - start, count) + out;
	}
	return out;
}

static String _format_method(const DocData::MethodDoc &p_m) {
	String args;
	for (int i = 0; i < p_m.arguments.size(); i++) {
		const DocData::ArgumentDoc &a = p_m.arguments[i];
		if (i > 0) {
			args += ", ";
		}
		args += a.name + ": " + a.type;
		if (!a.default_value.is_empty()) {
			args += " = " + a.default_value;
		}
	}
	String line = "  " + p_m.name + "(" + args + ")";
	if (!p_m.return_type.is_empty() && p_m.return_type != "void") {
		line += " -> " + p_m.return_type;
	}
	if (!p_m.qualifiers.is_empty()) {
		line += " " + p_m.qualifiers;
	}
	const String desc = _first_sentence(p_m.description);
	if (!desc.is_empty()) {
		line += " - " + desc;
	}
	return line;
}

static String _tool_get_class_docs(const Dictionary &p_input, bool &r_is_error) {
	HITBOX_ARG_STRING(raw_class, "class_name");
	const String cls = raw_class.strip_edges();
	DocTools *docs = EditorHelp::get_doc_data();
	if (!docs) {
		r_is_error = true;
		return "The class reference is not loaded yet; try again in a moment.";
	}
	DocData::ClassDoc *doc = EditorHelp::get_doc(cls);
	if (!doc) {
		for (KeyValue<String, DocData::ClassDoc> &kv : docs->class_list) {
			if (kv.key.nocasecmp_to(cls) == 0) {
				doc = &kv.value;
				break;
			}
		}
	}
	if (!doc) {
		r_is_error = true;
		return vformat("Unknown class \"%s\". Godot class names are PascalCase (CharacterBody2D, AnimationPlayer, Input...).", cls);
	}

	String out = "class " + doc->name;
	if (!doc->inherits.is_empty()) {
		out += " inherits " + doc->inherits;
	}
	out += "\n";
	const String brief = _strip_bbcode(doc->brief_description).strip_edges();
	if (!brief.is_empty()) {
		out += brief + "\n";
	}
	const String desc = _strip_bbcode(doc->description).strip_edges();
	if (!desc.is_empty()) {
		out += "\n" + _truncate(desc, 2500) + "\n";
	}

	if (!doc->properties.is_empty()) {
		out += "\nProperties:\n";
		for (const DocData::PropertyDoc &p : doc->properties) {
			String line = "  " + p.name + ": " + p.type;
			if (!p.default_value.is_empty()) {
				line += " = " + p.default_value;
			}
			const String d = _first_sentence(p.description);
			if (!d.is_empty()) {
				line += " - " + d;
			}
			out += line + "\n";
		}
	}
	if (!doc->methods.is_empty()) {
		out += "\nMethods:\n";
		for (const DocData::MethodDoc &m : doc->methods) {
			out += _format_method(m) + "\n";
		}
	}
	if (!doc->signals.is_empty()) {
		out += "\nSignals:\n";
		for (const DocData::MethodDoc &s : doc->signals) {
			out += _format_method(s) + "\n";
		}
	}
	if (!doc->constants.is_empty()) {
		out += "\nConstants:\n";
		int shown = 0;
		for (const DocData::ConstantDoc &c : doc->constants) {
			out += "  " + c.name + " = " + c.value;
			if (!c.enumeration.is_empty()) {
				out += " (enum " + c.enumeration + ")";
			}
			out += "\n";
			if (++shown >= 150) {
				out += vformat("  ...[%d more constants]\n", doc->constants.size() - shown);
				break;
			}
		}
	}
	return _truncate(out, HITBOX_MAX_DOC_CHARS);
}

/* ---------------------------------------------------------------------- */
/* Dispatch                                                                */
/* ---------------------------------------------------------------------- */

String HitboxTools::execute(const String &p_name, const Dictionary &p_input, bool &r_is_error) {
	r_is_error = false;
	if (p_name == "list_files") {
		return _tool_list_files(p_input, r_is_error);
	} else if (p_name == "read_file") {
		return _tool_read_file(p_input, r_is_error);
	} else if (p_name == "write_file") {
		return _tool_write_file(p_input, r_is_error);
	} else if (p_name == "edit_file") {
		return _tool_edit_file(p_input, r_is_error);
	} else if (p_name == "search_files") {
		return _tool_search_files(p_input, r_is_error);
	} else if (p_name == "get_scene_tree") {
		return _tool_get_scene_tree(p_input, r_is_error);
	} else if (p_name == "get_node_properties") {
		return _tool_get_node_properties(p_input, r_is_error);
	} else if (p_name == "set_node_property") {
		return _tool_set_node_property(p_input, r_is_error);
	} else if (p_name == "add_node") {
		return _tool_add_node(p_input, r_is_error);
	} else if (p_name == "remove_node") {
		return _tool_remove_node(p_input, r_is_error);
	} else if (p_name == "attach_script") {
		return _tool_attach_script(p_input, r_is_error);
	} else if (p_name == "save_all") {
		return _tool_save_all(p_input, r_is_error);
	} else if (p_name == "open_scene") {
		return _tool_open_scene(p_input, r_is_error);
	} else if (p_name == "open_script") {
		return _tool_open_script(p_input, r_is_error);
	} else if (p_name == "run_project") {
		return _tool_run_project(p_input, r_is_error);
	} else if (p_name == "stop_project") {
		return _tool_stop_project(p_input, r_is_error);
	} else if (p_name == "get_output_log") {
		return _tool_get_output_log(p_input, r_is_error);
	} else if (p_name == "get_class_docs") {
		return _tool_get_class_docs(p_input, r_is_error);
	}
	r_is_error = true;
	return vformat("Unknown tool \"%s\".", p_name);
}

String HitboxTools::describe_call(const String &p_name, const Dictionary &p_input) {
	if (p_name == "add_node") {
		const String type = p_input.get("type", "");
		const String scene = p_input.get("scene_path", "");
		const String parent = p_input.get("parent_path", "");
		return vformat("add_node %s -> %s", scene.is_empty() ? type : scene, parent.is_empty() ? String("(root)") : parent);
	}
	if (p_name == "set_node_property") {
		return vformat("set_node_property %s.%s", String(p_input.get("node_path", "")), String(p_input.get("property", "")));
	}
	static const char *keys[] = { "path", "node_path", "class_name", "query", "scene", "script_path", nullptr };
	for (int i = 0; keys[i]; i++) {
		if (p_input.has(keys[i]) && p_input[keys[i]].get_type() == Variant::STRING) {
			return p_name + " " + String(p_input[keys[i]]);
		}
	}
	return p_name;
}
