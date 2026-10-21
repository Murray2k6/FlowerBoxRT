# 3D FlowerBox RT

The Windows NT "3D FlowerBox (OpenGL)" screen saver rebuilt as a native 64-bit
Windows 11 screen saver on Direct3D 12, with ray tracing and NVIDIA DLSS.

The shapes, bloom, colours, lighting and floating square follow the original
(`ssflwbox`, NT 5.0 build 1679): the side tables, bloom ranges, colour tables,
lighting values and per-step increments were read from that binary. No
Microsoft code is used; the icon is the original's.

## Install

Right-click `3D FlowerBox RT.scr` and choose **Install**, or copy it to
`C:\Windows\System32` and pick it in Screen Saver Settings. It is a single
file: the DLSS runtime is built in (see below).

## What changed from the original

- **Timing.** The original moved one step per 16 ms `WM_TIMER` tick, so its
  speed depended on the system timer: 32 steps a second on the standard
  15.6 ms clock, 62.5 when any program raised the timer resolution. The new
  one animates on the real clock: Speed 100% is the original's 32 steps per
  second, adjustable from 10% to 300%.
- **Redraw.** The whole screen is redrawn every frame, so the floating square
  leaves no trails when it moves.
- **Ray tracing** (DXR 1.1, a toggle): soft shadows, ambient occlusion and
  reflections, with samples per pixel, reflection bounces and reflectivity.
  On graphics cards without DXR 1.1 the toggle is greyed out and the classic
  lighting is used.
- **DLSS** (built in, always on when the card supports it): each square is
  rendered at DLSS Quality resolution and upscaled; DLSS also does the
  anti-aliasing. Other cards use MSAA.
- **Floor** (optional): a dark mirror floor under the shape.
- **Frame rate cap**: 30, 60 (default) or the display's refresh rate.

## Settings

Stored in `HKCU\Software\FlowerBoxRT`. The original's options keep their
names: Coloring (Checkerboard, Per Side, One Color, Smooth, Slanted, Cycle),
Spin, Bloom, Two-sided, Shape, Complexity, Size.

## DLSS runtime

`nvngx_dlss.dll` is embedded in the `.scr`. NVIDIA's loader only loads it from
disk, so on first use it is unpacked to `%LOCALAPPDATA%\FlowerBoxRT\ngx` and
reused afterwards; it is unpacked again only if the embedded copy changes.

## Building

Needs Visual Studio with the C++ x64 tools and a Windows 10/11 SDK (for
`dxc`). Run `build.cmd`; the result is `out\3D FlowerBox RT.scr`.

The DLSS SDK files are not in this repository, because NVIDIA's license only
allows distributing them built into an application. On the first build,
`build.cmd` runs `fetch_dlss.ps1`, which downloads the files it needs from
<https://github.com/NVIDIA/DLSS> (tag v310.9.1) into `third_party\dlss` and
checks their SHA-256 sums:

- `include\` — `nvsdk_ngx*.h`
- `lib\nvsdk_ngx_s.lib` — from `lib/Windows_x86_64/x64/`
- `lib\nvngx_dlss.dll` — from `lib/Windows_x86_64/rel/`

### Testing switches

- `/snapshot <file.png> <seconds> <width> <height> [Name=value ...]` renders
  offscreen and writes `<file.png>.txt` with the status.
- `FLOWERBOX_D3D_DEBUG=1` (or `2` for GPU validation) enables the D3D12 debug
  layer; `FLOWERBOX_LOG=<file>` writes a run report; `FLOWERBOX_RT_VIEW=1`
  shows light visibility and occlusion; `FLOWERBOX_NO_DLSS=1` turns DLSS off.

## License

The project's source code is under the MIT License (`LICENSE`). The NVIDIA
DLSS SDK and runtime, and the original icon, are not covered by it; see the
credits below.

## Credits

### Original work

- **3D FlowerBox (OpenGL) screen saver** (`ssflwbox`, Windows NT 5.0 build
  1679, © 1994–1997 Microsoft Corporation). The shapes, side tables, bloom
  ranges, colour tables, lighting, camera and animation steps were reproduced
  from studying that binary; no Microsoft code is included. The icon is the
  original's icon.

### Included third-party software

- **NVIDIA DLSS SDK and runtime** (`nvsdk_ngx` headers and library,
  `nvngx_dlss.dll`, v310.9.1, <https://github.com/NVIDIA/DLSS>), © NVIDIA
  Corporation, used under the NVIDIA RTX SDKs license
  (fetched to `third_party\dlss\LICENSE.txt`). This software contains source code
  provided by NVIDIA Corporation. NVIDIA, RTX and DLSS are trademarks of
  NVIDIA Corporation. The DLSS SDK may not be placed under an open-source
  license.

### Techniques and published algorithms

- **Halton sequence** (J. H. Halton, 1960): the sub-pixel jitter pattern for
  DLSS.
- **R2 low-discrepancy sequence** (Martin Roberts, "The Unreasonable
  Effectiveness of Quasirandom Sequences", 2018): ray sample placement.
- **lowbias32 integer hash** (Chris Wellons, "Prospecting for Hash
  Functions", 2018): per-pixel sample rotation.
- **FNV-1a hash** (Glenn Fowler, Landon Curt Noll, Kiem-Phong Vo): checking
  the unpacked DLSS runtime.
- **Schlick's Fresnel approximation** (Christophe Schlick, 1994):
  reflection strength.
- **Phong / Blinn-Phong lighting** (Bui Tuong Phong, 1975; Jim Blinn, 1977),
  as in OpenGL's fixed-function pipeline, which the classic look follows.

### Tools

- Microsoft Visual Studio (C++ compiler), the Windows SDK, the DirectX Shader
  Compiler (DXC), Direct3D 12 and DXR, and the Windows Imaging Component
  (screenshots), all by Microsoft.
- For studying the original binary (not shipped): Python with the `pefile`
  library (Ero Carrera) and the Capstone disassembler (Nguyen Anh Quynh and
  contributors).
