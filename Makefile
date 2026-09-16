# Hitbox — the Godot editor with an AI agent compiled in.
#
#   make          build the editor, bundle Hitbox.app, install it, launch it
#   make build    compile the editor binary only (bin/godot.macos.editor.arm64)
#   make install  bundle + copy to /Applications and put `hitbox` on $PATH
#   make update   stop a running Hitbox, remove it, rebuild, reinstall, relaunch
#   make smoke    headless editor run that sends one prompt through the dock
#
# macOS on Apple Silicon (Metal). Other platforms build exactly like Godot:
#   scons platform=<linuxbsd|windows> target=editor

APP         := Hitbox
ARCH        := arm64
BINARY      := bin/godot.macos.editor.$(ARCH)
BUNDLE      := bin/$(APP).app
INSTALL_DIR := /Applications
# First writable directory on $PATH for the `hitbox` launcher symlink.
BIN_DIR     := $(shell for d in /usr/local/bin /opt/homebrew/bin $$HOME/.local/bin; do \
                 if [ -d "$$d" ] && [ -w "$$d" ]; then echo $$d; break; fi; done)
BIN_DIR     := $(if $(BIN_DIR),$(BIN_DIR),$(HOME)/.local/bin)
JOBS        := $(shell sysctl -n hw.ncpu)
SCONS_ARGS  := platform=macos arch=$(ARCH) target=editor vulkan=no

.PHONY: all build bundle install update run stop smoke clean

all: build install run

build:
	scons $(SCONS_ARGS) -j$(JOBS)

# Wrap the binary in an .app using Godot's tools bundle template, rebranded.
bundle: build
	rm -rf $(BUNDLE)
	cp -R misc/dist/macos_tools.app $(BUNDLE)
	rm -f $(BUNDLE)/Contents/Resources/GodotLG.icns $(BUNDLE)/Contents/Resources/Assets.car
	cp misc/dist/hitbox/$(APP).icns $(BUNDLE)/Contents/Resources/$(APP).icns
	sed -e 's|<string>Godot</string>|<string>$(APP)</string>|g' \
	    -e 's|org.godotengine.godot|dev.justin06lee.hitbox|' \
	    -e 's|GodotLG.icns|$(APP).icns|' \
	    -e '/<key>CFBundleIconName<\/key>/{N;d;}' \
	    -e 's|© 2007-present Juan Linietsky, Ariel Manzur &amp; Godot Engine contributors|Hitbox is a fork of Godot Engine. © 2007-present Juan Linietsky, Ariel Manzur \&amp; Godot Engine contributors|' \
	    misc/dist/macos_tools.app/Contents/Info.plist > $(BUNDLE)/Contents/Info.plist
	mkdir -p $(BUNDLE)/Contents/MacOS
	cp $(BINARY) $(BUNDLE)/Contents/MacOS/$(APP)
	chmod +x $(BUNDLE)/Contents/MacOS/$(APP)
	codesign --force --deep --sign - $(BUNDLE)

install: bundle
	rm -rf $(INSTALL_DIR)/$(APP).app
	cp -R $(BUNDLE) $(INSTALL_DIR)/$(APP).app
	mkdir -p $(BIN_DIR)
	ln -sf $(INSTALL_DIR)/$(APP).app/Contents/MacOS/$(APP) $(BIN_DIR)/hitbox
	@echo "Installed $(INSTALL_DIR)/$(APP).app and $(BIN_DIR)/hitbox"
	@case ":$$PATH:" in *":$(BIN_DIR):"*) ;; *) echo "Note: add $(BIN_DIR) to your PATH to run 'hitbox' by name." ;; esac

run:
	open -a $(INSTALL_DIR)/$(APP).app

stop:
	-pkill -x $(APP) 2>/dev/null || true

update: stop
	rm -rf $(INSTALL_DIR)/$(APP).app
	$(MAKE) build install run

# Starts the editor headless on misc/hitbox/smoke_project; its plugin sends one
# prompt through the Hitbox dock and prints the transcript. Needs a key in
# ANTHROPIC_API_KEY or in the editor settings; without one it exercises the
# error path (HTTP 401) end to end.
smoke: build
	$(BINARY) --headless --editor --path misc/hitbox/smoke_project 2>&1 | grep HITBOX_SMOKE

clean:
	scons $(SCONS_ARGS) --clean
	rm -rf $(BUNDLE)
