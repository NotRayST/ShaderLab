# ShaderLab

An image editor using ReShade shaders.

If you just want to use the tool, grab the pre-built zip from the [Releases](https://github.com/NotRayST/ShaderLab/releases) tab.

<img width="1280" height="720" alt="Image" src="https://github.com/user-attachments/assets/8c1c8ba2-1fe0-4005-ac18-9a757464a90d" />

## Table of Contents
- [What is this?](#what-is-this)
- [Requirements](#requirements)
- [How to Run](#how-to-run)
- [Controls & Hotkeys](#controls--hotkeys)
- [In-Game Depth Capture](#in-game-depth-capture-shaderlabcaptureaddon)
- [Building from Source](#building-from-source)
- [Project Structure](#project-structure)
- [License](#license)
- [Pull Requests](#pull-requests)
- [Author & Socials](#author--socials)

---

## What is this?
ShaderLab is a lightweight app with an addon that leverages ReShade's shader pipeline for state-of-the-art image processing. It allows you to apply various effects such as color grading, changing lighting, bloom, sharpening, depth of field, and more to your images, the limit is your creativity, as with ReShade itself.

### Highlights:
- **Project saving**: Saves your original image, current parameters, and shaders settings to a `.shaderlab` file, so you can come back and tweak it later.
- **Native Resolution Export**: Renders images through active ReShade pipelines at their true resolution, without any downsampling.
- **In-Game Depth Capture (`ShaderLabCapture.addon`)**: ReShade companion add-on for games. Screenshots automatically get their depth buffer embedded right into your png file.
- **Machine Learning Depth Estimation**: One-click local depth inference directly on your machine for any image with no depth. So your depth of field effects and other depth-related shaders will work. (Lacks good normals though, RTGI is recommended only on depth captured from games.)
- **Full Undo / Redo feature**: Just like any other software, use (`Ctrl+Z` / `Ctrl+Y`) to undo / redo your changes.
- **Interactive Viewport**: Pan, zoom, smooth sub-degree rotation, 45° snapping, lock modes, and live depth peeking (`D` key).
- **CLI**: Batch processing, preset rendering, single-shader testing (`.fx`), and parameter overrides without opening the UI (see [CLI_MANUAL.txt](CLI_MANUAL.txt)).

---

## Requirements
- **OS**: Windows 10 / 11 64-bit, DirectX 11 capable GPU.
- **ReShade**: **ReShade with Full Add-on Support 6.8+** (download from [reshade.me](https://reshade.me)). Standard signed ReShade builds do not load unsigned `.addon` binaries (**they won't work**)

---

## How to Run
1. Download **ReShade with full add-on support** from [reshade.me](https://reshade.me).
2. Run the ReShade setup, browse and select `ShaderLab.exe`, choose DirectX 10/11/12, and pick your shader packages.
3. Launch `ShaderLab.exe`.
4. Drag and drop any image (`.png`, `.jpg`, `.jpeg`, `.bmp`) or `.shaderlab` project onto the window.
5. Press `Home` to open the overlay:
   - **Home tab**: Enable and tweak your ReShade shaders.
   - **ShaderLab Helper tab**: On first launch it'll be off to the far right of the tabs, just drag it out for ease of use. You can manage depth, settings, keybinds, and **Export** there.

---

## Controls & Hotkeys

### Mouse Controls
- **Left drag**: Pan image
- **Right drag** (or **Alt + Left drag**): Rotate image (snaps every 45°)
- **Mouse wheel**: Zoom in / out (centered on cursor)
- **Left double-click**: Reset entire view (zoom, pan, rotation)
- **Shift (hold)**: Fine-tune mode (smooth sub-degree rotation, precision zoom, slow pan)

### Hotkeys (rebindable)
| Shortcut | Action |
| :--- | :--- |
| `Home` | Toggle ReShade overlay |
| `D` (hold) | Depth Peek (temporarily bypass effects to view raw depth) |
| `Ctrl + Z` / `Ctrl + Y` | Undo / Redo shader & parameter change |
| `Q` | Reset zoom & pan |
| `R` | Reset rotation |
| `Left` / `Right Arrow` | Nudge image horizontally |
| `H` / `J` / `K` / `L` | Toggle Lock Pan / Zoom / Rotation / All |
| `F11` | Toggle Borderless Fullscreen |
| `Ctrl + S` / `Ctrl + Shift + S` | Save Project / Save Project As... |
| `Ctrl + E` / `Ctrl + Shift + E` | Export Image / Export Image As... |

---

## In-Game Depth Capture (`ShaderLabCapture.addon`)
To capture the true depth buffer from games:
1. Copy `ShaderLabCapture.addon` into your game's executable folder (e.g. `Binaries\Win64\` for Unreal Engine games).
2. Whenever you take a ReShade screenshot in-game, the add-on embeds the camera depth into the PNG automatically, shouldn't be more than 1-4MB depending on the resolution you play at.
3. Drop that screenshot into ShaderLab, and depth effects like DoF, SSAO, RTGI, ReLight, etc... will work out of the box!

---

## Building from Source

### Prerequisites
- Visual Studio 2022 (C++20 desktop workload)
- CMake 3.20+

Build with (PowerShell):

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Built outputs:
- `build/host_app/Release/ShaderLab.exe`
- `build/addon/Release/ShaderLab.addon`
- `build/game_capture_addon/Release/ShaderLabCapture.addon`

---

## Project Structure
- `host_app/`: D3D11 host application, CLI parser & subcommands, HUD overlays, viewport controller.
- `addon/`: ReShade add-on UI, undo/redo system, IPC hooks, export pipeline.
- `game_capture_addon/`: Standalone in-game camera depth grabber.
- `common/`: Shared memory IPC protocol (`ipc_protocol.h`), custom `.sldepth` chunk serializers, project file handlers.
- `tools/depth_anything_v2/`: Python inference backend for AI depth estimation.
- `third_party/`: Header-only vendor libraries (`reshade`, `imgui`, `stb`, `miniz`).

---

## License
ShaderLab is released under the [MIT License](LICENSE).

For third-party dependencies, open-source libraries, and model licenses (Depth Anything V2, ReShade SDK, Dear ImGui, stb, miniz), see [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).

---

## Pull Requests
When submitting a pull request:
- Test it out, make sure it works as intended, just because it compiles doesn't mean it works.
- If you coded it with AI, please disclose it in the pull request, and please test it out really well before submitting.  

You can also open an issue in the issues tab for the bug or feature request instead.

## Author & Socials
Built by **RaySt**

- **Patreon**: [patreon.com/cw/RayST](https://www.patreon.com/cw/RayST)
- **Twitter / X**: [@NotRay_st](https://x.com/NotRay_st)
