# Xemu UWP: xemu on Xbox (Developer Mode)

xemu v0.8.137 running as a UWP app on an Xbox Series console in Developer Mode. Tested on a Series X. The BIOS
boots, the dashboard runs from the HDD image, and games load from `D:\DevelopmentFiles` and run.

Still rough: the picture is rendered at 640x480, audio may be silent (SDL falls back to its dummy driver if WASAPI
won't open), and nothing has been tuned for speed.

## How it works

Everything in the table was measured on the console with `probe/jitprobe.c` before it was built on.

| What the console does to a Dev Mode app | What the port does |
|---|---|
| No memory that is writable and executable at once, so TCG's JIT can't allocate its buffer | split-W^X: one section mapped twice, RW to write code and RX to run it (`tcg/region.c`, `xu_jit_map_views` in `ui/xemu-uwp.c`). Needs the `codeGeneration` capability. |
| No `opengl32.dll`, and no Win32 windows (`RegisterClassExW` returns error 120) | Mesa 25.0.7's **OSMesa with the d3d12 driver**, shipped as `opengl32.dll`, gives GL 4.0 core on the GPU. SDL runs its *dummy* video driver, and `ui/xemu-uwp.c` sends xemu's SDL GL calls to OSMesa. Each frame's buffer is copied to a D3D11 swap chain on the CoreWindow. |
| No native file dialog | `ui/xui/file-browser.cc`: an ImGui browser that works with the controller (A open, B up/cancel, Y all files). It starts at a Locations page: LocalState, `D:\DevelopmentFiles` and any readable drive. |
| Files outside the app's folders are off limits | `removableStorage` + `broadFileSystemAccess` in the manifest. With them, the plain Win32 file calls read `D:\DevelopmentFiles`. |
| XInput delivers no pads to a UWP app | SDL's Windows.Gaming.Input backend |
| `GetDiskFreeSpace` fails in the app container | `block/file-win32.c` checks its result (it used to use stack garbage as the sector size and assert) |
| 4 of xemu's 872 imports don't exist on the console | Defined as `__imp_` pointers to stubs in `ui/xemu-uwp.c` |
| No `dxil.dll` (Mesa's d3d12 driver signs shaders with it) | Shipped in the package, from the Windows SDK Redist |
| The B button sends a UWP app home | `BackRequested` is handled |
| SDL3's joystick init insists on a Win32 helper window | `tools/patch_subprojects.sh` makes that failure non-fatal |

Everything is behind `XEMU_UWP` (`-DXEMU_UWP=1`); the desktop build is unchanged apart from the `file-win32.c` fix.

## Layout

All of this lives in `uwp/` in this repository:

- `tools/`: build, package and deploy scripts
- `probe/jitprobe.c`: the console probe (JIT routes, memory, D3D, platform and import census, OSMesa, file access)
- `mesa/xu_wgl.c`: the three `wgl*` exports that let osmesa.dll stand in for `opengl32.dll`
- `xemu.toml`: starter config for LocalState
- `build/`: outputs, including the self-signed dev certificate and package version counters (gitignored)

The build side lives in WSL under `~/xemu-uwp/` for speed:

- `xemu/`: the tree that gets built. It's an upstream checkout kept in step with `tools/sync.sh`, or just a clone of this branch.
- `mesa-25.0.7/`: Mesa source plus `build/win` (osmesa.dll)
- `toolchain/`: llvm-mingw 20261006 (UCRT, clang 23)
- `sysroot/`: MSYS2 clang64 libraries (`tools/msys2_fetch.py`)
- `hosttools/`: pkgconf, bison, flex and m4, unpacked from debs (no sudo needed)
- `bin/`: the cross pkg-config wrapper (made by `tools/env.sh`)

## Setting up (once, in WSL)

```bash
mkdir -p ~/xemu-uwp/toolchain && cd ~/xemu-uwp/toolchain
curl -LO https://github.com/mstorsjo/llvm-mingw/releases/download/20261006/llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64.tar.xz
tar xf llvm-mingw-*.tar.xz
pip3 install --user --break-system-packages meson ninja mako zstandard cmake pyyaml packaging
mkdir -p ~/xemu-uwp/hosttools/debs && cd ~/xemu-uwp/hosttools/debs
apt-get download pkgconf pkgconf-bin libpkgconf7 bison flex m4 libfl2
for d in *.deb; do dpkg-deb -x "$d" ..; done
python3 uwp/tools/msys2_fetch.py ~/xemu-uwp/sysroot glib2 libepoxy libsamplerate openssl zlib libslirp pixman zstd libpng vulkan-headers
```

Then symlink the Vulkan headers into the toolchain's include directory (`.../x86_64-w64-mingw32/include/vulkan`
and `vk_video`, pointing into the sysroot). Fetch Mesa 25.0.7 into `~/xemu-uwp/mesa-25.0.7` (it's the last
series with OSMesa).

## Build and deploy (in WSL, from `uwp/`)

```bash
tools/sync.sh                      # Windows checkout -> WSL build tree (skip if building a clone directly)
tools/configure.sh uwp             # once
tools/patch_subprojects.sh         # after configure has downloaded the subprojects
ninja -C ~/xemu-uwp/xemu/build/uwp -j4 qemu-system-i386w.exe
tools/mesa_build.sh                # once: osmesa.dll
tools/stage_xemu.sh                # package payload + import check against the console census
tools/xpkg.sh xemu                 # signed .msix
tools/xdeploy.sh xemu install
tools/xdeploy.sh xemu launch
tools/xdeploy.sh xemu get xemu.log
```

For a downloadable release, run `tools/release.sh` after the ninja build. It stages and packages the build, then
writes `build/release/Xemu-UWP-<version>.zip` (the `.msix`, the `.cer` and `INSTALL.txt`, never the signing
key). Configure strips the builder's home directory out of the source paths compiled into the binary.

The scripts expect the Windows SDK at `/mnt/e/Windows Kits/10` (set `XEMU_WINKITS`) and Device Portal at
`https://192.168.1.91:11443` (set `XBOX`), with portal authentication off.

## Files you supply

You need your own MCPX boot ROM, flash BIOS, HDD image and game discs (XISO). Either:
- copy them into `D:\DevelopmentFiles` with Device Portal's File Explorer and pick them in xemu's
  **Settings → System** (open xemu's menu with **View + Menu** together); or
- put them in the app's LocalState with `tools/xdeploy.sh xemu put SRC NAME`.

xemu rewrites `xemu.toml` when it exits, so stop it before editing that file directly.
