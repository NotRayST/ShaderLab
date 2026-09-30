ShaderLab
============================
Version: 1.2.2
Author: RaySt
----------------------------------------------------------------

   Support: https://www.patreon.com/cw/RayST
   Repository: https://github.com/NotRayST/ShaderLab

----------------------------------------------------------------

Requirements
===============
Windows 10 / 11 64-bit, DirectX 11 GPU.
ReShade with Full Add-on Support 6.8+ (download from https://reshade.me).


How to use
===========
1. Download "ReShade with full add-on support" (version 6.8 or newer) from https://reshade.me.
2. Run the ReShade installer, click Browse, and select ShaderLab.exe.
3. Select DirectX 10/11/12 and choose whichever shader packs you want.
4. Launch ShaderLab.exe.
5. Drag and drop any image (.png, .jpg, .jpeg, .bmp) onto the window.
6. Press Home to open the ReShade overlay.
7. Use the "ShaderLab Helper" tab to configure depth and options, and the "Home" tab to tweak effects.
8. Click "Export Image" in ShaderLab Helper to render the final image at native resolution.


Export Settling & Shader Compiling
===================================
When exporting, ShaderLab renders at native image resolution, which causes ReShade to recompile all active shaders for the new dimensions and temporal effects have to settle from scratch.

That's where our Smart settle system comes in, just leave it enabled and export, its very reliable.

In case it does not prove reliable for some edge cases, you can use manual settle, 15 secs by default, you can select up to 60 secs via the slider, or ctrl+click on the slider to choose a custom value.


In-game Depth Capture (ShaderLabCapture.addon)
================================================
To capture camera depth directly from games (Requires ReShade 6.8+):
Copy ShaderLabCapture.addon into your game's 64-bit executable folder (where the game .exe and reshade-shaders folder live).
Note for Unreal Engine games: this is usually inside the "Binaries\Win64" folder.

When you take a screenshot with ReShade in-game, the add-on automatically embeds the depth inside the png. (default 16-BIT; pick 32/16/8/4-BIT in the add-ons tab, lower bits = much smaller files)
Drop that screenshot png into ShaderLab and depth effects (DoF, SSAO, etc.) will work.

Normal maps from the Machine learning solution are not that good for effects that rely on that like RTGI, so extracting from the game is currently the best depth solution possible.

Controls
===========
Mouse Controls:
  Left drag                     Pan image
  Middle drag                   Pan image
  Right drag                    Rotate image (snaps every 45°)
  Alt + Left drag               Rotate image (alternative to right drag)
  Mouse wheel                   Zoom in / out (centered on cursor)
  Left double-click             Reset entire view (zoom, pan, rotation)
  Shift (hold)                  Fine-tune mode (smooth sub-degree rotation,
                                precision zoom & slow pan)

Hotkeys (default keybinds, rebindable in ShaderLab Helper -> Keybinds tab):
  Home                          Toggle ReShade overlay
  D (hold)                      Depth Peek (temporarily view raw depth)
  Ctrl + Z                      Undo shader / parameter change (erase stroke while in Erase Mode)
  Ctrl + Y                      Redo shader / parameter change
  Q                             Reset zoom & pan
  R                             Reset rotation
  Left / Right Arrow            Nudge image horizontally
  H                             Toggle Lock Pan
  J                             Toggle Lock Zoom
  K                             Toggle Lock Rotation
  L                             Toggle Lock View (locks Pan, Zoom & Rotation)
  F11                           Toggle Borderless Fullscreen
  B                             Toggle Before / After Split Comparison (LMB drag move, RMB drag rotate)
  E                             Toggle Erase Mode (paint over an object, release to remove it;
                                [ / ] brush size, right-click unmask, Esc exit)
  Ctrl + S                      Save project (.shaderlab)
  Ctrl + Shift + S              Save project as...
  Ctrl + E                      Export image
  Ctrl + Shift + E              Export image as...


Cheers!

- RaySt

