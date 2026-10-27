# ReShade DLSSNR

A ReShade addon that runs NVIDIA DLSS Neural Rendering on top of a game's existing DLSS upscaling path.

## Requirements

- Windows, DX12.
- ReShade with addon support.
- NVIDIA GPU with DLSS 5 support.
- Pre-DLSS and Post-DLSS passes require a game that has DLSS active. Backbuffer passes do not.
- Any version of `nvngx_dlssnr.dll`, with Streamline 1.x, Streamline 2.x, or without Streamline.

## Install

- Copy `reshade_dlssnr64.addon64` and `reshade_dlssnr64.json` into the game directory.
- Enable the addon from the ReShade overlay.

Note: copying `reshade_dlssnr64.json` is optional. It contains pre-made presets.

## Build

- Open `reshade_dlssnr/reshade_dlssnr.sln` and build `Release|x64`.
- Output: `reshade_dlssnr/build/Release/reshade_dlssnr64.addon64`.
- Requires the ReShade SDK, NGX SDK headers, Detours, D3DX12, and the `FidelityFX-CAS` / `tracy` submodules under `reshade_dlssnr/third_party/`:

```bash
git submodule update --init --recursive
```

## Features

- Named presets.
- Injection points: Pre-DLSS, Post-DLSS, and Backbuffer (independent of DLSS).
- Per-pass settings.
- Gain-map HDR composition for scRGB, HDR10, and SDR.
- Game / Adaptive / Local exposure layers.
- Optional OKLab grades, edge-aware softening, and CAS sharpening on the model input and output.
- Temporal reprojection (experimental).
- Debug views for inputs, outputs, guides, masks, HDR round-trip, and temporal states.

## Limitations

- Pre-DLSS is skipped under Ray Reconstruction.
- Only tested on TLOU2, TLOU1, and AW2.

## Third-party

This addon is 0BSD (`LICENSE.md`). It builds against the submodules under
`reshade_dlssnr/third_party/`, each under its own license:

| Component | License |
| --- | --- |
| [ReShade](https://github.com/crosire/reshade) | BSD 3-Clause |
| [FidelityFX-CAS](https://github.com/GPUOpen-Effects/FidelityFX-CAS) | MIT |
| [Detours](https://github.com/microsoft/Detours) | MIT |
| [Tracy](https://github.com/wolfpld/tracy) | BSD 3-Clause |
| [DirectX-Headers](https://github.com/microsoft/DirectX-Headers) | MIT |
| [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS) | NVIDIA DLSS SDK license |

`hooks/local_vtable.h` is derived from RenoDX (`renodx/src/utils/vtable.hpp`),
MIT, copyright (C) 2025 Carlos Lopez Jr.; the notice is kept in the file.

A built `.addon64` contains compiled FidelityFX CAS shader code and links
Detours, so the MIT notices above apply to binary distribution as well.
