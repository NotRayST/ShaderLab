# ShaderLab

tool that allows you to use reshade shaders on still images and exports it with the full resolution. you drive it from a tab in the reshade overlay, if you are not a developer, no need to continue reading this, just download it from the releases page.

## whats in here
- `ShaderLab.exe` (host_app/) - the actual renderer. headless d3d11 worker with a dumb little resizable window. the swapchain backbuffer gets resized to the image so reshade actually runs at native res. this was the whole hard part tbh.
- `ShaderLab.addon` (addon/) - the reshade addon. draws the overlay tab and grabs the backbuffer once effects are done, then writes a png.
- they live in the same process (reshade is just dxgi.dll). they talk over a shared memory block called `Local\ShaderLabV_IPC_<pid>`. one job at a time, kept it simple on purpose.

## building
you need vs2022 + cmake 3.20+ and reshade with full addon support from reshade.me. the normal signed reshade build refuses to load .addon files, thats on them not me.

```
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

outputs are `build/host_app/Release/ShaderLab.exe` and `build/addon/Release/ShaderLab.addon`

## quick self test (no reshade needed)
if you just wanna confirm the gpu path aint broken:

```
.\build\host_app\Release\ShaderLab.exe --selftest someimage.png
```

loads the image, resizes to native, presents 30 frames, dumps `_selftest.png`. worked on my machine, should work on yours.

## running it with reshade
1. run the reshade installer (full addon support) and point it at `ShaderLab.exe`, pick dx10/11/12
2. copy `ShaderLab.addon` next to `ShaderLab.exe` (or an `addons/` folder)
3. launch it, open the reshade overlay (Home key) and switch to the **ShaderLab Helper** tab

## the overlay tab
- theres a big drop zone, or click it to pick a file. you can also just drag an image onto the window.
- output folder, defaults to `out`. the Open button just opens explorer there.
- **Export Image** - renders the current image through your active shaders at full res and saves the png. it forces alpha to 255 cause most shaders zero it out and viewers show the image as invisible.
- a collapsible log.

note: depth-based shaders are not supported currently, still wip.

built by notrayst (https://x.com/NotRay_st)
