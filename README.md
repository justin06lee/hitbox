<div align="center">

<img src="assets/hitbox.svg" alt="hitbox" width="340" />

# hitbox

**Cursor for game development: the Godot editor with an AI agent compiled in.**<br>
*A fork of Godot 4.7, not a plugin. The agent reads your project, edits scenes and scripts, runs the game and reads what it printed. Runs on your Claude Code sign-in through yagami, or on an Anthropic API key.*

</div>

---

Hitbox is a fork of [Godot Engine](https://godotengine.org) 4.7.2 with an AI agent built into the editor binary, the way Cursor forked VS Code. It opens like Godot, builds like Godot, and every Godot project works in it unchanged. The difference is a **Hitbox** dock next to the Inspector where you talk to Claude about the project in front of you, and it does the work in the editor.

## What the agent can do

Every message you send carries the editor's current context: the open scene and its root, the selected nodes, the active script and any selected text. On top of that the agent has tools over the editor:

- **Files.** List the project, read files, write files, make exact string edits, and search across all text files. Edited scripts reload in the script editor; an edited `.tscn` that is open reloads in the editor.
- **Scenes.** Dump the open scene's node tree, read a node's non-default properties, set properties, add nodes (engine classes or instanced `.tscn` files), remove nodes, attach scripts, save, and open scenes or scripts. Every scene change goes through the editor's undo history.
- **Running the game.** Play the main scene, the current scene or any scene, stop it, and read the Output panel, including prints and errors from the running game.
- **Engine reference.** Pull the class reference for any engine class from this exact build: inheritance, properties with defaults, method signatures, signals and constants. The agent is told to check it before using an API it is not sure about, which is what keeps it on Godot 4 syntax.

The agent streams its reply and its reasoning summary into the dock, shows each tool call as it runs, and keeps going through tool calls until the task is done or you press stop.

## Build and install

Hitbox builds exactly like Godot. On macOS with Apple Silicon:

```sh
brew install scons        # Python 3.9+ and Xcode command line tools are required too
make                      # build, bundle Hitbox.app, install to /Applications, launch
```

| Target | What it does |
| --- | --- |
| `make` | Build the editor, bundle it as `Hitbox.app`, copy it to `/Applications`, put `hitbox` on `$PATH`, launch it. |
| `make build` | Compile the editor binary only, into `bin/`. |
| `make install` | Bundle and install without launching. |
| `make update` | Stop a running Hitbox, remove it, rebuild, reinstall and relaunch. |
| `make smoke` | Headless editor run that sends two prompts through the dock, the first needing a tool call and the second needing its result, then prints the transcript and PASS or FAIL. |

The first build takes a while; later builds only recompile what changed. The macOS build uses Metal and does not need the Vulkan SDK. Other platforms use Godot's own build line, for example `scons platform=linuxbsd target=editor`; everything Hitbox adds is platform independent, only the Makefile is macOS specific.

## Setup

Hitbox needs somewhere to send the agent's requests. It picks one on its own, and **Editor Settings > Hitbox > Backend > Mode** overrides the choice.

**yagami, on your Claude Code sign-in.** [yagami](https://github.com/justin06lee/yagami) serves the Messages API from the `claude` CLI you are already logged into, so no API key is needed. Hitbox reads yagami's URL and key from `~/.config/yagami/config.json` (or `$YAGAMI_CONFIG_DIR`). If the server isn't running when you send a message, Hitbox starts it: an installed `yagami` binary first, then the npm package through `bunx @justin06lee/yagami` or `npx`. Any of these installs work:

```sh
bun add -g @justin06lee/yagami     # from npm
cd yagami && make                  # from a checkout
```

With yagami 0.10 or newer the agent has every editor tool. Older versions can't pass tools through, so Hitbox says so in the dock and keeps chatting without them.

**The Anthropic API, on an API key.** Paste a key into the field in the dock, set it in Editor Settings under **Hitbox > Anthropic > API Key**, or export `ANTHROPIC_API_KEY` before launching.

In auto mode, a key typed into Hitbox wins. Otherwise yagami wins whenever it is installed or configured. Next comes an `ANTHROPIC_API_KEY` from the environment. Last, if the Claude Code CLI and `bunx` or `npx` are present, Hitbox runs yagami straight from npm.

| Setting | Default | Meaning |
| --- | --- | --- |
| Hitbox > Backend > Mode | `auto` | `auto`, `yagami` or `anthropic_api`. |
| Hitbox > Yagami > Url | from yagami's config | Base URL of the yagami server. |
| Hitbox > Yagami > Api Key | from yagami's config | The `ygm_` key. |
| Hitbox > Yagami > Auto Start | on | Start yagami when nothing answers on a local URL. |
| Hitbox > Anthropic > Model | Claude Opus 5 | Also switchable from the dock's picker. Applies to both backends. |
| Hitbox > Anthropic > Effort | `high` | Reasoning effort. Applies to both backends. |

The model picker offers Claude Opus 5, Claude Sonnet 5, Claude Fable 5.1 and Claude Haiku 4.5. Hitbox keeps its own editor settings, separate from Godot's, so installing it next to Godot does not touch your Godot configuration.

## How it is built

Everything Hitbox adds lives in one engine module, `modules/hitbox/`:

- `hitbox_dock.cpp` is the chat dock and the agent loop. Each request streams on a worker thread. Against the Anthropic API, tool calls come back to the dock and run on the main thread between requests.
- `hitbox_mcp_server.cpp` is a small MCP server, speaking JSON-RPC over streamable HTTP, that serves the same tools to yagami. It listens on `127.0.0.1` on a random port behind a per-session bearer token. Hitbox sends it with each request in the Anthropic MCP connector shape, yagami connects Claude Code to it, and the model calls the editor's tools inside the turn. Requests are polled on the main thread, so tools touch editor state safely either way.
- `hitbox_backend.cpp` decides between yagami and the Anthropic API and discovers yagami's URL and key.
- `hitbox_client.cpp` is a small streaming client for the Messages API over Godot's own HTTP stack, and starts yagami when it isn't running. No external dependencies.
- `hitbox_tools.cpp` defines the tools the model sees and implements them against the editor: file system, scene tree, undo history, run bar, output log and the class reference.
- `hitbox_editor_plugin.cpp` registers the editor settings and mounts the dock.

The smoke test lives in `misc/hitbox/smoke_project/`: a minimal project whose editor plugin calls the dock's scriptable surface (`send_prompt`, `is_busy`, `get_transcript_text`), which any editor plugin can use too.

Outside the module the fork touches two files: `version.py`, which names the product, and `editor/editor_log.h`, which gains four one-line accessors so the agent can read the Output panel. Keeping the footprint that small is deliberate. Upstream Godot releases merge in with `git fetch upstream` followed by a merge of the new stable tag.

## Upstream

Godot Engine is Copyright (c) 2014-present Godot Engine contributors and Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur, released under the MIT license. Hitbox keeps that license; see `LICENSE.txt` and `COPYRIGHT.txt`. Godot's own README, documentation and community live at [godotengine.org](https://godotengine.org).
