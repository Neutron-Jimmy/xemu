#!/bin/bash
# patch_subprojects.sh -- changes to xemu's downloaded meson subprojects (not in xemu's git), applied in the WSL
# build tree. Idempotent: each patch is applied only if it is not already in.
#
#   sdl3: SDL_Init(JOYSTICK|HAPTIC) on Windows makes a message-only "helper window" for DirectInput and fails
#         outright when it cannot. The console has no Win32 windows (RegisterClass: not supported), and the UWP
#         build disables DirectInput anyway (ui/xemu-uwp.c), so the failure is logged and ignored.
set -eu
SDL=~/xemu-uwp/xemu/subprojects/sdl3
if ! grep -q "helper window unavailable" "$SDL/src/SDL.c"; then
  python3 - "$SDL/src/SDL.c" <<'EOF'
import sys
p = sys.argv[1]
s = open(p).read()
old = """        if (!SDL_HelperWindowCreate()) {
            goto quit_and_error;
        }"""
new = """        if (!SDL_HelperWindowCreate()) {
            /* xemu UWP: no Win32 windows on the console; only DirectInput needs this one */
            SDL_Log("SDL: helper window unavailable (%s); DirectInput will not work", SDL_GetError());
            SDL_ClearError();
        }"""
assert s.count(old) == 1, "SDL.c: the helper window block moved"
open(p, "w").write(s.replace(old, new))
print("  sdl3: helper window failure made non-fatal")
EOF
fi
