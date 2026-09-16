@tool
extends EditorPlugin
## Sends one prompt through the Hitbox dock at editor startup, waits for the
## agent to finish, prints the transcript and quits. Every line is prefixed
## with HITBOX_SMOKE so the Makefile can grep it out of the editor's output.

const PROMPT := "Reply with exactly the three words: hitbox smoke ok"
const TIMEOUT_SECONDS := 120.0


func _enter_tree() -> void:
	_run.call_deferred()


func _run() -> void:
	# Let the editor finish loading its docks.
	await get_tree().create_timer(2.0).timeout
	var dock: Node = EditorInterface.get_base_control().find_child("Hitbox", true, false)
	if dock == null:
		print("HITBOX_SMOKE FAIL: no dock named Hitbox in the editor tree")
		get_tree().quit(1)
		return
	print("HITBOX_SMOKE dock found: ", dock.get_class())
	dock.send_prompt(PROMPT)
	var waited := 0.0
	while dock.is_busy() and waited < TIMEOUT_SECONDS:
		await get_tree().create_timer(0.5).timeout
		waited += 0.5
	var transcript: String = dock.get_transcript_text()
	print("HITBOX_SMOKE busy after %.1fs: %s" % [waited, str(dock.is_busy())])
	for line in transcript.split("\n"):
		print("HITBOX_SMOKE | ", line)
	get_tree().quit(0)
