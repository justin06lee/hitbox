@tool
extends EditorPlugin
## Sends one prompt through the Hitbox dock at editor startup, waits for the
## agent to finish, prints the transcript and quits. Every line is prefixed
## with HITBOX_SMOKE so the Makefile can grep it out of the editor's output.
##
## The prompt needs a tool call, so a passing run proves the whole path: the
## backend (Anthropic API or yagami), the tools, and the reply.

const PROMPT := "Call the get_scene_tree tool once. Then reply with only the root node's name and how many direct children it has, formatted exactly like: Root=Name Children=N"
const FOLLOW_UP := "Without calling any tool, name the two children you saw in that tree, formatted exactly like: Kids=First,Second"
const TIMEOUT_SECONDS := 300.0


func _enter_tree() -> void:
	_run.call_deferred()


func _run() -> void:
	# Let the editor finish loading its docks and open the main scene.
	await get_tree().create_timer(2.0).timeout
	if EditorInterface.get_edited_scene_root() == null:
		EditorInterface.open_scene_from_path("res://main.tscn")
		await get_tree().create_timer(1.0).timeout
	var dock: Node = EditorInterface.get_base_control().find_child("Hitbox", true, false)
	if dock == null:
		print("HITBOX_SMOKE FAIL: no dock named Hitbox in the editor tree")
		get_tree().quit(1)
		return
	print("HITBOX_SMOKE backend: ", dock.get_backend_label())
	var waited := await _ask(dock, PROMPT)
	var waited_follow_up := await _ask(dock, FOLLOW_UP)
	var transcript: String = dock.get_transcript_text()
	print("HITBOX_SMOKE turns took %.1fs and %.1fs, busy=%s, mcp tool calls=%d" % [waited, waited_follow_up, str(dock.is_busy()), dock.get_mcp_call_count()])
	for line in transcript.split("\n"):
		print("HITBOX_SMOKE | ", line)
	# The follow-up can only be answered from the first turn's tool result.
	var kids := ""
	for line in transcript.split("\n"):
		if line.strip_edges().begins_with("Kids="):
			kids = line
	var ok := transcript.contains("Root=Main") and transcript.contains("Children=2") and kids.contains("Player") and kids.contains("Label")
	print("HITBOX_SMOKE result: ", "PASS" if ok else "FAIL")
	# HITBOX_SMOKE_KEEP_OPEN=1 leaves the editor up to look at the dock.
	if OS.get_environment("HITBOX_SMOKE_KEEP_OPEN").is_empty():
		get_tree().quit(0 if ok else 1)


func _ask(dock: Node, text: String) -> float:
	dock.send_prompt(text)
	var waited := 0.0
	while dock.is_busy() and waited < TIMEOUT_SECONDS:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	return waited
