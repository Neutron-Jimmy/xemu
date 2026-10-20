# Xemu UWP

**[xemu](https://xemu.app), the original Xbox emulator, running on Xbox Series X|S in Developer Mode.**

This fork packages xemu v0.8.137 as a UWP app. It gets the full emulator onto the console: the TCG JIT, OpenGL
rendering on the console's own GPU, controller input and a controller-friendly file browser. It is not affiliated
with the xemu project; for xemu itself, see [xemu.app](https://xemu.app).

> **Status: early but working.** Tested on an Xbox Series X: the BIOS boots, the dashboard runs from the HDD image,
> and games load and run. Known rough edges are under [Known limitations](#known-limitations).

## Requirements

- An Xbox Series X|S (or Xbox One) in **Developer Mode**, with Device Portal enabled
- Your own **MCPX boot ROM**, **flash BIOS**, **HDD image** and game discs (XISO). Nothing here includes them.

## Installing

1. Download `Xemu-UWP-<version>.zip` from [Releases](../../releases) and unzip it. Inside are the app package
   (`Xemu_<version>.msix`) and its self-signed certificate (`xemudev.cer`).
2. Open Device Portal in a browser (the address is shown in Dev Home on the console, e.g. `https://<console-ip>:11443`).
3. Go to **Home → My games & apps → Add**. Choose the `.msix`, click **Next**, then **Start** (there are no
   dependencies to add). The console doesn't ask for the certificate; it's included for other install tools that do.
4. Copy your BIOS, MCPX ROM, HDD image and games onto the console, e.g. into `D:\DevelopmentFiles` with Device
   Portal's **File Explorer**.
5. Start **xemu** from Dev Home or the Device Portal app list.

To update, install a newer `.msix` the same way; your settings are kept.

## First run

1. Open xemu's menu: press **View + Menu** together. The Xbox button can't be used, since the console's own guide
   takes it.
2. Go to **Settings → System** and set **MCPX Boot ROM**, **Flash ROM (BIOS)** and **Hard Disk**. Each one opens
   the in-app file browser.
3. Load a game with **Load Disc** from the same menu.

### The file browser

It works entirely with the controller:

| Button | Action |
|---|---|
| **A** | Open a folder / pick a file |
| **B** | Up one folder (from the top: cancel) |
| **Y** | Toggle between matching files and all files |

It opens on a **Locations** page:
- **xemu's folder** (the app's LocalState, where its settings and log live)
- **Development Files** (`D:\DevelopmentFiles`)
- any readable drive, such as USB storage

## Known limitations

- The picture is rendered at **640x480** and scaled up by the console.
- **Audio may be silent.** If the app container won't open WASAPI, SDL falls back to its dummy driver.
- **Performance hasn't been tuned.** Frames are read back from the GPU and re-presented through D3D11.
- The Xbox button doesn't open xemu's menu; use **View + Menu**.
- xemu's built-in updater is disabled: it would fetch upstream's desktop build. New versions of this port come
  from [Releases](../../releases). Links that open a web page don't work on the console.

## How it works

The console gives a Developer Mode app no writable-and-executable memory, no OpenGL and no Win32 windows. Each
workaround was measured on the console first (with `uwp/probe/jitprobe.c`):

- **JIT:** split-W^X. One memory section is mapped twice, a writable view TCG generates code into and an executable
  view it runs from. This uses the `codeGeneration` capability.
- **Graphics:** Mesa 25.0's **OSMesa with the d3d12 driver**, shipped as `opengl32.dll`, gives xemu OpenGL 4.0 core
  on the console's GPU. SDL runs its dummy video driver; xemu's GL context calls go to OSMesa, and each finished
  frame is presented through a D3D11 swap chain on the app's CoreWindow.
- **Input:** controllers come through SDL's Windows.Gaming.Input backend.
- **Files:** the native file dialog is replaced by the in-app browser. The `broadFileSystemAccess` and
  `removableStorage` capabilities let xemu read files outside its own folder.

All changes are behind `XEMU_UWP`. Details, the full list of console quirks, and build instructions are in
[`uwp/README.md`](uwp/README.md).

## Building

The build cross-compiles from WSL with llvm-mingw and packages a signed `.msix`; see
[`uwp/README.md`](uwp/README.md). The scripts in `uwp/tools/` also install and launch the build on a console
through Device Portal.

## License

xemu is licensed under the GNU GPL v2 (see `LICENSE`), and this fork's changes are under the same terms. Release
packages also contain Mesa (MIT), zlib (zlib license) and Microsoft's `dxil.dll` from the Windows SDK
(redistributable). Xbox is a trademark of Microsoft; this project is not affiliated with or endorsed by Microsoft
or the xemu project.
