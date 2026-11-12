import argparse
import math
import os
import sys
import urllib.request
from pathlib import Path
from typing import Dict, Optional, Tuple


def _setup_sys_path() -> Path:
    script_dir = Path(__file__).resolve().parent
    candidates = [
        script_dir,
        script_dir / "site-packages",
        script_dir.parent / "depth_anything_v2" / "site-packages",
        script_dir.parent / "lama" / "site-packages",
        script_dir.parent.parent / "common" / "site-packages",
    ]
    for p in candidates:
        if p.exists():
            p_str = str(p)
            if p_str not in sys.path:
                sys.path.insert(0, p_str)
    return script_dir


SCRIPT_DIR = _setup_sys_path()

try:
    sys.stdout.reconfigure(line_buffering=True, encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(line_buffering=True, encoding="utf-8", errors="replace")
except Exception:
    pass


def report_progress(pct: int, msg: str) -> None:
    pct_clamped = max(0, min(100, int(pct)))
    print(f"[PROGRESS] {pct_clamped}: {msg}", flush=True)


# Permissive open-source checkpoints only (BSD-3-Clause / Open weights)
MODEL_CATALOG: Dict[str, dict] = {
    "RealESRGAN_x4plus": {
        "filename": "RealESRGAN_x4plus.pth",
        "url": "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.1.0/RealESRGAN_x4plus.pth",
        "arch": "rrdb",
        "native_scale": 4,
        "num_block": 23,
        "license": "BSD-3-Clause",
    },
    "RealESRGAN_x2plus": {
        "filename": "RealESRGAN_x2plus.pth",
        "url": "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.1/RealESRGAN_x2plus.pth",
        "arch": "rrdb",
        "native_scale": 2,
        "num_block": 23,
        "license": "BSD-3-Clause",
    },
    "RealESRGAN_x4plus_anime_6B": {
        "filename": "RealESRGAN_x4plus_anime_6B.pth",
        "url": "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.2.4/RealESRGAN_x4plus_anime_6B.pth",
        "arch": "rrdb",
        "native_scale": 4,
        "num_block": 6,
        "license": "BSD-3-Clause",
    },
    "RealPLKSR": {
        "filename": "4xNomosWebPhoto_RealPLKSR.pth",
        "url": "https://github.com/Phhofm/models/releases/download/4xNomosWebPhoto_RealPLKSR/4xNomosWebPhoto_RealPLKSR.pth",
        "arch": "spandrel",
        "native_scale": 4,
        "num_block": 0,
        "license": "Apache-2.0 / CC-BY-4.0",
    },
}


def download_checkpoint(url: str, dst_path: Path) -> None:
    dst_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = dst_path.with_suffix(dst_path.suffix + ".part")
    report_progress(5, f"Downloading {dst_path.name}...")

    last_reported = -1

    def _reporthook(block_num: int, block_size: int, total_size: int) -> None:
        nonlocal last_reported
        if total_size > 0:
            frac = min(1.0, (block_num * block_size) / float(total_size))
            pct = 5 + int(frac * 15)
            if pct != last_reported:
                last_reported = pct
                mb_done = (block_num * block_size) / (1024.0 * 1024.0)
                mb_total = total_size / (1024.0 * 1024.0)
                report_progress(pct, f"Downloading {dst_path.name} ({mb_done:.1f}/{mb_total:.1f} MB)...")

    req = urllib.request.Request(url, headers={"User-Agent": "ShaderLab-Upscaler/1.0"})
    with urllib.request.urlopen(req, timeout=60) as resp, open(tmp_path, "wb") as out_f:
        total = int(resp.headers.get("Content-Length") or 0)
        downloaded = 0
        chunk_size = 256 * 1024
        block_num = 0
        while True:
            chunk = resp.read(chunk_size)
            if not chunk:
                break
            out_f.write(chunk)
            downloaded += len(chunk)
            block_num += 1
            _reporthook(block_num, chunk_size, total)

    if tmp_path.stat().st_size < 1024 * 1024:
        tmp_path.unlink(missing_ok=True)
        raise RuntimeError(f"Downloaded checkpoint {dst_path.name} appears invalid or truncated.")
    os.replace(tmp_path, dst_path)


def build_rrdb_net(torch_mod, nn_mod, F_mod):
    # BSD-3-Clause RRDBNet matching xinntao/Real-ESRGAN & BasicSR parameter naming

    def make_layer(basic_block, num_basic_block: int, **kwarg):
        layers = [basic_block(**kwarg) for _ in range(num_basic_block)]
        return nn_mod.Sequential(*layers)

    class ResidualDenseBlock5C(nn_mod.Module):
        def __init__(self, num_feat: int = 64, num_grow_ch: int = 32):
            super().__init__()
            self.conv1 = nn_mod.Conv2d(num_feat, num_grow_ch, 3, 1, 1)
            self.conv2 = nn_mod.Conv2d(num_feat + num_grow_ch, num_grow_ch, 3, 1, 1)
            self.conv3 = nn_mod.Conv2d(num_feat + 2 * num_grow_ch, num_grow_ch, 3, 1, 1)
            self.conv4 = nn_mod.Conv2d(num_feat + 3 * num_grow_ch, num_grow_ch, 3, 1, 1)
            self.conv5 = nn_mod.Conv2d(num_feat + 4 * num_grow_ch, num_feat, 3, 1, 1)
            self.lrelu = nn_mod.LeakyReLU(negative_slope=0.2, inplace=True)

        def forward(self, x):
            x1 = self.lrelu(self.conv1(x))
            x2 = self.lrelu(self.conv2(torch_mod.cat((x, x1), 1)))
            x3 = self.lrelu(self.conv3(torch_mod.cat((x, x1, x2), 1)))
            x4 = self.lrelu(self.conv4(torch_mod.cat((x, x1, x2, x3), 1)))
            x5 = self.conv5(torch_mod.cat((x, x1, x2, x3, x4), 1))
            return x5 * 0.2 + x

    class RRDB(nn_mod.Module):
        def __init__(self, num_feat: int, num_grow_ch: int = 32):
            super().__init__()
            self.rdb1 = ResidualDenseBlock5C(num_feat, num_grow_ch)
            self.rdb2 = ResidualDenseBlock5C(num_feat, num_grow_ch)
            self.rdb3 = ResidualDenseBlock5C(num_feat, num_grow_ch)

        def forward(self, x):
            out = self.rdb1(x)
            out = self.rdb2(out)
            out = self.rdb3(out)
            return out * 0.2 + x

    class RRDBNet(nn_mod.Module):
        def __init__(
            self,
            num_in_ch: int = 3,
            num_out_ch: int = 3,
            scale: int = 4,
            num_feat: int = 64,
            num_block: int = 23,
            num_grow_ch: int = 32,
        ):
            super().__init__()
            self.scale = scale
            in_ch_eff = num_in_ch
            if scale == 2:
                in_ch_eff = num_in_ch * 4
            elif scale == 1:
                in_ch_eff = num_in_ch * 16

            self.conv_first = nn_mod.Conv2d(in_ch_eff, num_feat, 3, 1, 1)
            self.body = make_layer(RRDB, num_block, num_feat=num_feat, num_grow_ch=num_grow_ch)
            self.conv_body = nn_mod.Conv2d(num_feat, num_feat, 3, 1, 1)
            self.conv_up1 = nn_mod.Conv2d(num_feat, num_feat, 3, 1, 1)
            self.conv_up2 = nn_mod.Conv2d(num_feat, num_feat, 3, 1, 1)
            self.conv_hr = nn_mod.Conv2d(num_feat, num_feat, 3, 1, 1)
            self.conv_last = nn_mod.Conv2d(num_feat, num_out_ch, 3, 1, 1)
            self.lrelu = nn_mod.LeakyReLU(negative_slope=0.2, inplace=True)

        def forward(self, x):
            feat = x
            if self.scale == 2:
                feat = F_mod.pixel_unshuffle(x, 2)
            elif self.scale == 1:
                feat = F_mod.pixel_unshuffle(x, 4)

            feat = self.conv_first(feat)
            body_feat = self.conv_body(self.body(feat))
            feat = feat + body_feat
            feat = self.lrelu(self.conv_up1(F_mod.interpolate(feat, scale_factor=2, mode="nearest")))
            feat = self.lrelu(self.conv_up2(F_mod.interpolate(feat, scale_factor=2, mode="nearest")))
            out = self.conv_last(self.lrelu(self.conv_hr(feat)))
            return out

    return RRDBNet


def resolve_device(torch_mod, requested: str):
    req = (requested or "auto").lower()
    if req == "cpu":
        return torch_mod.device("cpu"), "cpu"
    if req in ("auto", "cuda") and torch_mod.cuda.is_available():
        return torch_mod.device("cuda"), "cuda"
    if req in ("auto", "dml"):
        try:
            import torch_directml  # type: ignore

            return torch_directml.device(), "dml"
        except Exception:
            if req == "dml":
                raise
    return torch_mod.device("cpu"), "cpu"


def load_model(
    torch_mod,
    model_name: str,
    model_dir: Path,
    device,
    device_kind: str,
    use_fp16: bool,
    auto_download: bool,
) -> Tuple[object, int, bool, int]:
    """Return (model, native_scale, actual_fp16, min_pad_multiple)."""
    custom_path = Path(model_name)
    if custom_path.exists() and custom_path.is_file():
        ckpt_path = custom_path
        spec = None
    elif model_name in MODEL_CATALOG:
        spec = MODEL_CATALOG[model_name]
        ckpt_path = model_dir / spec["filename"]
        if not ckpt_path.exists():
            # also inspect legacy shared folders if already placed there
            alt_paths = [
                SCRIPT_DIR / spec["filename"],
                SCRIPT_DIR / "weights" / spec["filename"],
                SCRIPT_DIR.parent / "upscale" / "models" / spec["filename"],
            ]
            for alt in alt_paths:
                if alt.exists():
                    ckpt_path = alt
                    break
        if not ckpt_path.exists():
            if not auto_download:
                raise FileNotFoundError(
                    f"Checkpoint not found at '{ckpt_path}'. Run with --auto-download or download {spec['url']}"
                )
            download_checkpoint(spec["url"], ckpt_path)
    else:
        raise ValueError(f"Unknown upscale model '{model_name}' and file does not exist.")

    report_progress(22, f"Loading model {ckpt_path.name}...")

    prefer_spandrel = (spec is not None and spec["arch"] == "spandrel") or ckpt_path.suffix.lower() == ".safetensors"

    # Try MIT-licensed Spandrel first when requested or non-RRDB checkpoint
    spandrel_mod = None
    try:
        import spandrel  # type: ignore

        spandrel_mod = spandrel
    except ImportError:
        spandrel_mod = None

    model_obj = None
    native_scale = spec["native_scale"] if spec else 4
    pad_multiple = 8

    if prefer_spandrel:
        if spandrel_mod is None:
            raise RuntimeError(
                f"Model '{ckpt_path.name}' requires the MIT 'spandrel' package. "
                "Please install it via `pip install spandrel` or select a Real-ESRGAN model."
            )
        desc = spandrel_mod.ModelLoader(device=device).load_from_file(str(ckpt_path))
        desc.eval()
        model_obj = desc.model
        native_scale = int(getattr(desc, "scale", native_scale) or native_scale)
        pad_multiple = 16
    else:
        raw_ckpt = torch_mod.load(str(ckpt_path), map_location="cpu", weights_only=True)
        if isinstance(raw_ckpt, dict):
            state_dict = raw_ckpt.get("params_ema") or raw_ckpt.get("params") or raw_ckpt.get("state_dict") or raw_ckpt
        else:
            state_dict = raw_ckpt

        # Detect RRDBNet hyperparameters directly from weights so x4plus, x2plus, anime_6B all work seamlessly
        if isinstance(state_dict, dict) and "conv_first.weight" in state_dict:
            conv_first_w = state_dict["conv_first.weight"]
            num_feat = int(conv_first_w.shape[0])
            in_ch_eff = int(conv_first_w.shape[1])
            if in_ch_eff == 12:
                native_scale = 2
            elif in_ch_eff == 48:
                native_scale = 1
            else:
                native_scale = 4

            block_ids = set()
            for k in state_dict.keys():
                if k.startswith("body."):
                    parts = k.split(".")
                    if len(parts) > 1 and parts[1].isdigit():
                        block_ids.add(int(parts[1]))
            num_block = (max(block_ids) + 1) if block_ids else (spec["num_block"] if spec else 23)

            num_grow_ch = 32
            if "body.0.rdb1.conv1.weight" in state_dict:
                num_grow_ch = int(state_dict["body.0.rdb1.conv1.weight"].shape[0])

            RRDBNet = build_rrdb_net(torch_mod, torch_mod.nn, torch_mod.nn.functional)
            model_obj = RRDBNet(
                num_in_ch=3,
                num_out_ch=3,
                scale=native_scale,
                num_feat=num_feat,
                num_block=num_block,
                num_grow_ch=num_grow_ch,
            )
            model_obj.load_state_dict(state_dict, strict=True)
            model_obj.eval()
            model_obj = model_obj.to(device)
            pad_multiple = 4 if native_scale == 4 else 8
        elif spandrel_mod is not None:
            desc = spandrel_mod.ModelLoader(device=device).load_from_file(str(ckpt_path))
            desc.eval()
            model_obj = desc.model
            native_scale = int(getattr(desc, "scale", native_scale) or native_scale)
            pad_multiple = 16
        else:
            raise RuntimeError(
                f"Unrecognized architecture in '{ckpt_path.name}' and 'spandrel' is not installed."
            )

    actual_fp16 = bool(use_fp16 and device_kind == "cuda")
    if actual_fp16:
        try:
            model_obj = model_obj.half()
        except Exception:
            actual_fp16 = False

    return model_obj, native_scale, actual_fp16, pad_multiple


def _pad_to_multiple(torch_mod, F_mod, tensor, multiple: int):
    _, _, h, w = tensor.shape
    pad_h = (multiple - h % multiple) % multiple
    pad_w = (multiple - w % multiple) % multiple
    if pad_h == 0 and pad_w == 0:
        return tensor, h, w
    mode = "reflect" if (h > pad_h and w > pad_w) else "replicate"
    padded = F_mod.pad(tensor, (0, pad_w, 0, pad_h), mode=mode)
    return padded, h, w


def _make_feather_mask(torch_mod, out_h: int, out_w: int, overlap_out: int, is_left: bool, is_right: bool, is_top: bool, is_bottom: bool):
    wy = torch_mod.ones(out_h, dtype=torch_mod.float32)
    wx = torch_mod.ones(out_w, dtype=torch_mod.float32)
    if overlap_out > 0:
        ov_y = min(overlap_out, max(1, out_h // 2))
        ov_x = min(overlap_out, max(1, out_w // 2))
        ramp_y = 0.5 * (1.0 - torch_mod.cos(torch_mod.linspace(0.0, math.pi, ov_y + 2)[1:-1]))
        ramp_x = 0.5 * (1.0 - torch_mod.cos(torch_mod.linspace(0.0, math.pi, ov_x + 2)[1:-1]))
        if not is_top and ov_y > 0:
            wy[:ov_y] = ramp_y
        if not is_bottom and ov_y > 0:
            wy[-ov_y:] = torch_mod.flip(ramp_y, dims=(0,))
        if not is_left and ov_x > 0:
            wx[:ov_x] = ramp_x
        if not is_right and ov_x > 0:
            wx[-ov_x:] = torch_mod.flip(ramp_x, dims=(0,))
    return (wy[:, None] * wx[None, :]).unsqueeze(0).unsqueeze(0)


def run_tiled_sr(
    torch_mod,
    model,
    img_rgb_float01,
    native_scale: int,
    tile_size: int,
    tile_pad: int,
    device,
    use_fp16: bool,
    pad_multiple: int,
    progress_start: int,
    progress_end: int,
    stage_label: str,
):
    import numpy as np

    F_mod = torch_mod.nn.functional
    # HWC float32 [0..1] -> NCHW tensor
    in_tensor = torch_mod.from_numpy(np.ascontiguousarray(img_rgb_float01.transpose(2, 0, 1))).unsqueeze(0)
    _, _, height, width = in_tensor.shape

    def _infer_patch(patch_cpu):
        patch_padded, orig_ph, orig_pw = _pad_to_multiple(torch_mod, F_mod, patch_cpu, pad_multiple)
        patch_dev = patch_padded.to(device)
        if use_fp16:
            patch_dev = patch_dev.half()
        with torch_mod.no_grad():
            out_dev = model(patch_dev)
        out_cpu = out_dev[:, :, : orig_ph * native_scale, : orig_pw * native_scale].float().cpu()
        return out_cpu

    # Single-shot attempt when tiling disabled or image already fits within one tile
    if tile_size <= 0 or (width <= tile_size and height <= tile_size):
        report_progress(progress_start, f"{stage_label}...")
        try:
            out_cpu = _infer_patch(in_tensor)
            out_np = out_cpu.squeeze(0).clamp_(0.0, 1.0).numpy().transpose(1, 2, 0)
            report_progress(progress_end, f"{stage_label} complete")
            return out_np
        except RuntimeError as exc:
            if "out of memory" not in str(exc).lower():
                raise
            if torch_mod.cuda.is_available():
                torch_mod.cuda.empty_cache()
            # Automatically fall back to safe 256px tiling on VRAM exhaustion
            tile_size = 256
            report_progress(progress_start, f"VRAM limit hit, retrying {stage_label} tiled (256px)...")

    eff_tile = max(64, int(tile_size))
    overlap = max(0, min(int(tile_pad), eff_tile // 2))
    stride = max(32, eff_tile - overlap)

    def _tile_starts(total_len: int, tile_len: int, step: int) -> list:
        if total_len <= tile_len:
            return [0]
        starts = list(range(0, total_len - tile_len + 1, step))
        if starts[-1] + tile_len < total_len:
            starts.append(total_len - tile_len)
        return starts

    xs = _tile_starts(width, eff_tile, stride)
    ys = _tile_starts(height, eff_tile, stride)
    total_tiles = len(xs) * len(ys)

    out_h = height * native_scale
    out_w = width * native_scale
    output_acc = torch_mod.zeros((1, 3, out_h, out_w), dtype=torch_mod.float32)
    weight_acc = torch_mod.zeros((1, 1, out_h, out_w), dtype=torch_mod.float32)

    tile_idx = 0
    for y0 in ys:
        y1 = min(y0 + eff_tile, height)
        for x0 in xs:
            x1 = min(x0 + eff_tile, width)
            tile_idx += 1
            pct = progress_start + int((progress_end - progress_start) * (tile_idx / float(max(1, total_tiles))))
            report_progress(pct, f"{stage_label} tile {tile_idx}/{total_tiles}...")

            patch = in_tensor[:, :, y0:y1, x0:x1]
            try:
                out_patch = _infer_patch(patch)
            except RuntimeError as exc:
                if "out of memory" in str(exc).lower() and (x1 - x0) > 128:
                    # Sub-divide oversized tile gracefully if CUDA runs out of VRAM
                    if torch_mod.cuda.is_available():
                        torch_mod.cuda.empty_cache()
                    raise RuntimeError(
                        f"CUDA out of memory at tile size {eff_tile}px. Select a smaller Tile Size (e.g. 256)."
                    ) from exc
                raise

            oy0 = y0 * native_scale
            oy1 = y1 * native_scale
            ox0 = x0 * native_scale
            ox1 = x1 * native_scale
            mask = _make_feather_mask(
                torch_mod,
                oy1 - oy0,
                ox1 - ox0,
                overlap * native_scale,
                is_left=(x0 == 0),
                is_right=(x1 == width),
                is_top=(y0 == 0),
                is_bottom=(y1 == height),
            )
            output_acc[:, :, oy0:oy1, ox0:ox1] += out_patch * mask
            weight_acc[:, :, oy0:oy1, ox0:ox1] += mask

    output_acc /= weight_acc.clamp_(min=1e-6)
    out_np = output_acc.squeeze(0).clamp_(0.0, 1.0).numpy().transpose(1, 2, 0)
    return out_np


def read_image_unicode(cv2_mod, np_mod, path: Path):
    data = np_mod.fromfile(str(path), dtype=np_mod.uint8)
    img = cv2_mod.imdecode(data, cv2_mod.IMREAD_UNCHANGED)
    if img is None:
        raise RuntimeError(f"Failed to decode image: {path}")
    return img


def write_image_unicode(cv2_mod, path: Path, img) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    ext = path.suffix.lower()
    if ext not in (".png", ".jpg", ".jpeg", ".webp", ".bmp", ".tif", ".tiff"):
        ext = ".png"
    ok, buf = cv2_mod.imencode(ext, img)
    if not ok:
        raise RuntimeError(f"Failed to encode output image: {path}")
    buf.tofile(str(path))


def _resize_channel(cv2_mod, arr, target_w: int, target_h: int):
    src_h, src_w = arr.shape[:2]
    if src_w == target_w and src_h == target_h:
        return arr
    interp = cv2_mod.INTER_AREA if (target_w < src_w or target_h < src_h) else cv2_mod.INTER_LANCZOS4
    return cv2_mod.resize(arr, (target_w, target_h), interpolation=interp)


def _bleed_transparent_rgb(cv2_mod, np_mod, bgr, alpha):
    # extend border colors into alpha=0 pixels so SR doesn't smear dark fringes at edges
    empty = alpha == 0
    if not np_mod.any(empty) or np_mod.all(empty):
        return bgr
    out = bgr.copy()
    valid = (~empty).astype(np_mod.uint8)
    kernel = np_mod.ones((3, 3), np_mod.uint8)
    for _ in range(4):
        dilated_mask = cv2_mod.dilate(valid, kernel)
        fill_mask = (dilated_mask > 0) & (valid == 0)
        if not np_mod.any(fill_mask):
            break
        blurred = cv2_mod.dilate(out, kernel)
        out[fill_mask] = blurred[fill_mask]
        valid = dilated_mask
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description="ShaderLab AI Image Upscaler (Real-ESRGAN / RealPLKSR)")
    parser.add_argument("--input", required=True, help="Input image path")
    parser.add_argument("--output", required=True, help="Output image path")
    parser.add_argument(
        "--model",
        default="RealESRGAN_x4plus",
        help="Model key (RealESRGAN_x4plus, RealESRGAN_x2plus, RealESRGAN_x4plus_anime_6B, RealPLKSR) or .pth path",
    )
    parser.add_argument("--model-dir", default=str(SCRIPT_DIR / "models"), help="Directory storing checkpoints")
    parser.add_argument("--scale", type=float, default=4.0, help="Desired output scale factor (e.g., 2, 4)")
    parser.add_argument("--target-width", type=int, default=0, help="Explicit custom output width")
    parser.add_argument("--target-height", type=int, default=0, help="Explicit custom output height")
    parser.add_argument("--tile", type=int, default=512, help="VRAM tile size (0 = no tiling, 256/512/768)")
    parser.add_argument("--tile-pad", type=int, default=32, help="Overlap padding per tile for feather blending")
    parser.add_argument("--blend", type=float, default=1.0, help="Denoise/blend strength 0..1 (1=full AI, 0=bicubic)")
    parser.add_argument("--device", default="auto", help="Compute device: auto, cuda, dml, cpu")
    parser.add_argument("--fp16", action="store_true", default=True, help="Use FP16 inference on CUDA")
    parser.add_argument("--no-fp16", dest="fp16", action="store_false", help="Force FP32 inference")
    parser.add_argument("--auto-download", action="store_true", default=True, help="Download missing checkpoints")
    parser.add_argument("--no-auto-download", dest="auto_download", action="store_false")
    parser.add_argument("--upscale-alpha", action="store_true", default=True, help="AI upscale non-trivial alpha")
    parser.add_argument("--no-upscale-alpha", dest="upscale_alpha", action="store_false")
    args = parser.parse_args()

    in_path = Path(args.input)
    out_path = Path(args.output)
    model_dir = Path(args.model_dir)
    if not in_path.exists():
        print(f"ERROR: Input file does not exist: {in_path}", file=sys.stderr)
        return 2

    report_progress(2, "Initializing runtime...")

    import cv2  # type: ignore
    import numpy as np  # type: ignore
    import torch  # type: ignore

    device, device_kind = resolve_device(torch, args.device)
    report_progress(4, f"Using compute device: {device_kind.upper()}")

    raw_img = read_image_unicode(cv2, np, in_path)
    if raw_img.ndim == 2:
        bgr = cv2.cvtColor(raw_img, cv2.COLOR_GRAY2BGR)
        alpha = None
    elif raw_img.shape[2] == 4:
        bgr = raw_img[:, :, :3]
        alpha = raw_img[:, :, 3]
    else:
        bgr = raw_img[:, :, :3]
        alpha = None

    orig_h, orig_w = bgr.shape[:2]
    if args.target_width > 0 and args.target_height > 0:
        out_w, out_h = int(args.target_width), int(args.target_height)
    else:
        scale_val = max(0.1, float(args.scale))
        out_w = max(1, int(round(orig_w * scale_val)))
        out_h = max(1, int(round(orig_h * scale_val)))

    blend = max(0.0, min(1.0, float(args.blend)))
    has_varying_alpha = alpha is not None and int(alpha.min()) != int(alpha.max())
    bgr_for_sr = _bleed_transparent_rgb(cv2, np, bgr, alpha) if has_varying_alpha else bgr

    if blend <= 1e-4:
        report_progress(50, "Applying bicubic resize...")
        out_bgr = cv2.resize(bgr_for_sr, (out_w, out_h), interpolation=cv2.INTER_CUBIC)
        if alpha is not None:
            out_a = cv2.resize(alpha, (out_w, out_h), interpolation=cv2.INTER_CUBIC)
            out_img = np.dstack([out_bgr, out_a])
        else:
            out_img = out_bgr
        write_image_unicode(cv2, out_path, out_img)
        report_progress(100, f"Upscaled to {out_w}x{out_h}")
        return 0

    model, native_scale, actual_fp16, pad_multiple = load_model(
        torch,
        args.model,
        model_dir,
        device,
        device_kind,
        args.fp16,
        args.auto_download,
    )

    rgb_f = cv2.cvtColor(bgr_for_sr, cv2.COLOR_BGR2RGB).astype(np.float32) / 255.0
    rgb_end_pct = 82 if (has_varying_alpha and args.upscale_alpha) else 94
    ai_rgb_f = run_tiled_sr(
        torch,
        model,
        rgb_f,
        native_scale,
        args.tile,
        args.tile_pad,
        device,
        actual_fp16,
        pad_multiple,
        progress_start=26,
        progress_end=rgb_end_pct,
        stage_label="Upscaling color",
    )
    ai_bgr_u8 = cv2.cvtColor(np.clip(ai_rgb_f * 255.0 + 0.5, 0.0, 255.0).astype(np.uint8), cv2.COLOR_RGB2BGR)
    ai_bgr_scaled = _resize_channel(cv2, ai_bgr_u8, out_w, out_h).astype(np.float32)

    if blend < 0.999:
        bicubic_bgr = cv2.resize(bgr, (out_w, out_h), interpolation=cv2.INTER_CUBIC).astype(np.float32)
        final_bgr = np.clip(ai_bgr_scaled * blend + bicubic_bgr * (1.0 - blend) + 0.5, 0.0, 255.0).astype(np.uint8)
    else:
        final_bgr = np.clip(ai_bgr_scaled + 0.5, 0.0, 255.0).astype(np.uint8)

    if alpha is not None:
        if has_varying_alpha and args.upscale_alpha:
            alpha_f = np.repeat((alpha.astype(np.float32) / 255.0)[:, :, None], 3, axis=2)
            ai_alpha_f = run_tiled_sr(
                torch,
                model,
                alpha_f,
                native_scale,
                args.tile,
                args.tile_pad,
                device,
                actual_fp16,
                pad_multiple,
                progress_start=83,
                progress_end=95,
                stage_label="Upscaling alpha",
            )
            ai_alpha_gray = np.clip(ai_alpha_f.mean(axis=2) * 255.0 + 0.5, 0.0, 255.0).astype(np.uint8)
            ai_alpha_scaled = _resize_channel(cv2, ai_alpha_gray, out_w, out_h).astype(np.float32)
            if blend < 0.999:
                bicubic_a = cv2.resize(alpha, (out_w, out_h), interpolation=cv2.INTER_CUBIC).astype(np.float32)
                final_a = np.clip(ai_alpha_scaled * blend + bicubic_a * (1.0 - blend) + 0.5, 0.0, 255.0).astype(np.uint8)
            else:
                final_a = np.clip(ai_alpha_scaled + 0.5, 0.0, 255.0).astype(np.uint8)
        else:
            final_a = _resize_channel(cv2, alpha, out_w, out_h)
        out_img = np.dstack([final_bgr, final_a])
    else:
        out_img = final_bgr

    report_progress(97, "Writing output image...")
    write_image_unicode(cv2, out_path, out_img)
    report_progress(100, f"Upscaled to {out_w}x{out_h}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
