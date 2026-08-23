# ShaderLab

**ReShade shaders, on still images.**

ShaderLab is a lightweight desktop app that lets you apply ReShade shaders to static images in real time. Instead of booting up a game just to redo a shot in ReShade, or paying for Adobe Lightroom, you can use ReShade shaders directly on your photos, at full resolution, thanks to the ShaderLab helper addon.

## Features

* **Native Resolution Export** - Renders through your active shader chain and exports the output at your image's original full resolution, saved to the `out` folder.
* **Live Fullscreen Preview** - A real time viewport that scales and aspect fits your image to any window size.
* **Drag and Drop** - Drag any image (.png, .jpg, .bmp, .tga, .hdr) straight onto the window, or use the built-in file picker.

## Installation & Setup

1. Download and run the latest ReShade Setup installer with full add-on support from [reshade.me](https://reshade.me).
2. When prompted to select a game or application, click **Browse** and choose `ShaderLab.exe`.
3. Select **DirectX 10/11/12** as the rendering API.
4. Complete the installer and download any effect packages you want.
   > Depth-dependent shaders (like depth of field, which needs a 3D depth buffer) don't work yet.
5. Once ReShade is installed, start ShaderLab from the `.exe` file.

## How to Use

1. When ShaderLab starts, a sample image will display on screen, automatically fitted to the window.
2. Drag and drop the image you want to edit onto the window. Alternatively, open ReShade with `Home`, go to the ShaderLab Helper tab, and use the button there to browse for an image instead.
3. Once you're happy with your edits, go to the ShaderLab Helper tab and click **Export Image**.
   * The app renders the image through your active shaders at full native resolution and saves the final PNG into the `out` folder.

> **Note:** ReShade loads the ShaderLab Helper addon on the far right side of the tab bar. If you don't see it right away, scroll over.

## Credits

Developed by Ray_st ([@NotRay_st](https://x.com/NotRay_st))

**V1.0** - 2026/08/23
