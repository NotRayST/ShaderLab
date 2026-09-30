import os
import sys
import gc

# local model code and vendored site-packages take priority over any system install
script_dir = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, script_dir)
sys.path.insert(0, os.path.join(script_dir, "site-packages"))
sys.path.insert(0, os.path.abspath(os.path.join(script_dir, "..", "..", "common", "site-packages")))

import argparse
import time
import struct
import zlib
import numpy as np
import cv2
import torch
if torch.cuda.is_available():
    torch.backends.cuda.matmul.allow_tf32 = True
    torch.backends.cudnn.allow_tf32 = True
    torch.backends.cudnn.benchmark = True

from dpt import DepthAnythingV2



def resolve_device(requested_device):
    req = requested_device.lower()
    if req in ("auto", "cuda") and torch.cuda.is_available():
        name = torch.cuda.get_device_name(0) if torch.cuda.device_count() > 0 else "CUDA GPU"
        return torch.device("cuda"), f"NVIDIA CUDA ({name})"
    if req in ("auto", "directml", "dml"):
        try:
            import torch_directml
            if torch_directml.is_available():
                return torch_directml.device(), "DirectX 12 DirectML (AMD Radeon / Intel Arc / NVIDIA)"
        except ImportError:
            pass
    return torch.device("cpu"), f"CPU ({os.cpu_count() or 4} threads, AVX2/AVX-512)"


def parse_args():
    parser = argparse.ArgumentParser(description="Run Depth Anything V2 inference for ShaderLab")
    parser.add_argument("--input", required=True, help="Path to input image")
    parser.add_argument("--model", default="", help="Path to model weights")
    parser.add_argument("--encoder", default="vitl", choices=["vits", "vitb", "vitl"], help="Model encoder type")
    parser.add_argument("--input-size", type=int, default=1008, help="Model input resolution (default: 1008)")
    parser.add_argument("--gamma", type=float, default=2.0, help="Depth distribution gamma curve exponent (default: 2.0)")
    parser.add_argument("--near-threshold", type=float, default=0.0, help="Foreground / near clamp threshold (0.0 = off)")
    parser.add_argument("--sky-threshold", type=float, default=0.0, help="Sky / infinity threshold clamping (0.0 = off)")
    parser.add_argument("--edge-refine", action="store_true", help="Refine depth contours to match RGB color edges")
    parser.add_argument("--invert", action="store_true", help="Invert depth polarity")
    parser.add_argument("--smooth-normals", action="store_true", help="Apply RGB-guided depth smoothing for surface normals (experimental)")
    parser.add_argument("--smooth-radius", type=int, default=8, help="Guided filter radius (default: 8)")
    parser.add_argument("--smooth-eps", type=float, default=1e-3, help="Guided filter edge preservation epsilon (default: 0.001)")
    parser.add_argument("--output-png", default="", help="Optional output 16-bit PNG depth map")
    parser.add_argument("--output-sidecar", default="", help="Optional output .sldepth sidecar file")
    parser.add_argument("--output-raw", default="", help="Optional output raw float32 binary depth")
    parser.add_argument("--far-plane", type=float, default=100.0, help="Linear depth far plane value")
    parser.add_argument("--device", default="auto", help="Device (auto/cuda/directml/cpu)")
    return parser.parse_args()


def get_model_path(requested_path, encoder="vits"):
    if requested_path and os.path.exists(requested_path):
        return os.path.abspath(requested_path)
    filename = f"depth_anything_v2_{encoder}.pth"
    candidates = [
        os.path.join(script_dir, filename),
        os.path.join(script_dir, "..", "..", "common", filename),
    ]
    for c in candidates:
        if os.path.exists(c):
            return os.path.abspath(c)
    return requested_path


def guided_filter(guide, src, radius=8, eps=1e-3):
    # edge-preserving guided filter (he et al. 2010)
    # guide is grayscale float [0, 1] and src is float [0, 1]
    I = guide.astype(np.float64)
    p = src.astype(np.float64)
    ksize = (2 * radius + 1, 2 * radius + 1)

    mean_I = cv2.boxFilter(I, -1, ksize, borderType=cv2.BORDER_REFLECT)
    mean_p = cv2.boxFilter(p, -1, ksize, borderType=cv2.BORDER_REFLECT)
    mean_Ip = cv2.boxFilter(I * p, -1, ksize, borderType=cv2.BORDER_REFLECT)
    mean_II = cv2.boxFilter(I * I, -1, ksize, borderType=cv2.BORDER_REFLECT)

    cov_Ip = mean_Ip - mean_I * mean_p
    var_I = np.maximum(mean_II - mean_I * mean_I, 0.0)

    a = cov_Ip / (var_I + eps)
    b = mean_p - a * mean_I

    mean_a = cv2.boxFilter(a, -1, ksize, borderType=cv2.BORDER_REFLECT)
    mean_b = cv2.boxFilter(b, -1, ksize, borderType=cv2.BORDER_REFLECT)

    q = mean_a * I + mean_b
    return q.astype(np.float32)


def write_sldepth(path, width, height, linear_floats, far_plane=1000.0):
    raw_data = linear_floats.tobytes()
    compressed = zlib.compress(raw_data)
    timestamp = int(time.time() * 1000)
    game_name = b"DepthAnythingV2\x00".ljust(64, b"\x00")
    
    header = struct.pack(
        "<IHHIIIfIIQ64s",
        0x534C4431,            # magic 'SLD1'
        1,                     # version
        0,                     # encoding (0 = R32F)
        int(width),
        int(height),
        1,                     # flags (kSidecarFlagValid)
        float(far_plane),
        len(raw_data),
        len(compressed),
        timestamp,
        game_name
    )
    
    # Calculate CRC32 of header + compressed
    crc = zlib.crc32(header)
    crc = zlib.crc32(compressed, crc)
    
    with open(path, "wb") as f:
        f.write(header)
        f.write(compressed)
        f.write(struct.pack("<I", crc))


def main():
    args = parse_args()
    
    model_path = get_model_path(args.model, args.encoder)
    if not model_path or not os.path.exists(model_path):
        print(f"Error: model checkpoint not found at '{args.model}'", file=sys.stderr, flush=True)
        sys.exit(1)
        
    if not os.path.exists(args.input):
        print(f"Error: input file '{args.input}' not found", file=sys.stderr, flush=True)
        sys.exit(1)
        
    print("[PROGRESS 10%] [PROGRESS] 10: Loading input image...", flush=True)
    print(f"[DepthAnythingV2] Loading image: {args.input}", flush=True)
    raw_img = cv2.imread(args.input)
    if raw_img is None:
        print(f"Error: failed to read image {args.input}", file=sys.stderr, flush=True)
        sys.exit(1)
        
    h, w, _ = raw_img.shape
    print(f"[DepthAnythingV2] Image dimensions: {w}x{h}", flush=True)
    
    print("[PROGRESS 20%] [PROGRESS] 20: Initializing neural network...", flush=True)
    device, device_desc = resolve_device(args.device)
    print(f"[DepthAnythingV2] Hardware Device: {device_desc}", flush=True)
    print(f"[DepthAnythingV2] Initializing model ({args.encoder})...", flush=True)
    
    model_configs = {
        'vits': {'encoder': 'vits', 'features': 64, 'out_channels': [48, 96, 192, 384]},
        'vitb': {'encoder': 'vitb', 'features': 128, 'out_channels': [96, 192, 384, 768]},
        'vitl': {'encoder': 'vitl', 'features': 256, 'out_channels': [256, 512, 1024, 1024]}
    }
    
    print("[PROGRESS 35%] [PROGRESS] 35: Loading model weights...", flush=True)
    model = DepthAnythingV2(**model_configs[args.encoder])
    state_dict = torch.load(model_path, map_location="cpu")
    model.load_state_dict(state_dict)
    del state_dict
    gc.collect()

    model = model.to(device).eval()
    if device.type == "cuda":
        torch.cuda.empty_cache()
    
    print("[PROGRESS 45%] [PROGRESS] 45: Preparing neural inference...", flush=True)
    print(f"[DepthAnythingV2] Running depth estimation (resolution={args.input_size}px)...", flush=True)
    t0 = time.perf_counter()
    raw_depth = None
    cur_size = args.input_size

    hook_handles = []
    total_blocks = len(model.pretrained.blocks) if hasattr(model, "pretrained") and hasattr(model.pretrained, "blocks") else 0
    if total_blocks > 0:
        for idx, blk in enumerate(model.pretrained.blocks):
            def make_hook(i):
                def hook(mod, inp, out):
                    pct = int(46 + (i + 1) / total_blocks * 38)
                    print(f"[PROGRESS {pct}%] [PROGRESS] {pct}: Depth estimation ({i + 1}/{total_blocks})...", flush=True)
                return hook
            hook_handles.append(blk.register_forward_hook(make_hook(idx)))
    
    while raw_depth is None and cur_size >= 392:
        try:
            with torch.no_grad():
                if device.type == "cuda":
                    with torch.amp.autocast('cuda', dtype=torch.float16):
                        raw_depth = model.infer_image(raw_img, input_size=cur_size)
                else:
                    raw_depth = model.infer_image(raw_img, input_size=cur_size)
        except Exception as e:
            err_str = str(e).lower()
            if "out of memory" in err_str or "oom" in err_str or "allocate" in err_str or "cuda" in err_str:
                if torch.cuda.is_available():
                    torch.cuda.empty_cache()
                prev_size = cur_size
                cur_size = max(int(round((cur_size * 0.70) / 14.0) * 14), 392)
                if cur_size == prev_size:
                    print(f"Error: GPU ran out of memory at {prev_size}px. Please select a smaller model or lower resolution.", file=sys.stderr, flush=True)
                    sys.exit(1)
                print(f"[DepthAnythingV2] Warning: Out of VRAM at {prev_size}px! Auto-recovering at {cur_size}px...", flush=True)
            else:
                print(f"Error during neural inference: {e}", file=sys.stderr, flush=True)
                sys.exit(1)

    for hook_handle in hook_handles:
        hook_handle.remove()

    t1 = time.perf_counter()
    if device.type == "cuda":
        torch.cuda.empty_cache()
    print(f"[DepthAnythingV2] Depth estimation completed in {(t1 - t0)*1000:.1f} ms", flush=True)
    print("[PROGRESS 88%] [PROGRESS] 88: Post-processing depth map...", flush=True)
    
    # Invert disparity so 0.0 is near and 1.0 is far (linear depth representation in ShaderLab)
    d_min = float(raw_depth.min())
    d_max = float(raw_depth.max())
    eps = 1e-6
    if (d_max - d_min) > eps:
        linear_depth = (d_max - raw_depth) / (d_max - d_min)
    else:
        linear_depth = np.zeros_like(raw_depth)

    # 0.5 rgb-guided depth smoothing for clean normals (eliminates corduroy banding in rtgi/mxao)
    if args.smooth_normals:
        try:
            guide_gray = cv2.cvtColor(raw_img, cv2.COLOR_BGR2GRAY).astype(np.float32) / 255.0
            if guide_gray.shape != linear_depth.shape:
                guide_gray = cv2.resize(guide_gray, (linear_depth.shape[1], linear_depth.shape[0]), interpolation=cv2.INTER_LINEAR)
            linear_depth = guided_filter(guide_gray, linear_depth, radius=args.smooth_radius, eps=args.smooth_eps)
            linear_depth = np.clip(linear_depth, 0.0, 1.0).astype(np.float32)
            print(f"[DepthAnythingV2] Applied RGB-guided depth smoothing (r={args.smooth_radius}, eps={args.smooth_eps})", flush=True)
        except Exception as e:
            print(f"[DepthAnythingV2] Note: normal smoothing pass skipped: {e}", flush=True)

    # 1. Edge-preserving bilateral filter alignment (snaps contours to color edges)
    if args.edge_refine:
        try:
            linear_depth = cv2.bilateralFilter(linear_depth.astype(np.float32), d=9, sigmaColor=0.08, sigmaSpace=7.0)
        except Exception as e:
            print(f"[DepthAnythingV2] Note: edge refinement skipped: {e}", flush=True)

    # 2. Depth distribution gamma curve (D_out = D_in^gamma)
    if abs(args.gamma - 1.0) > 0.01 and args.gamma > 0.01:
        linear_depth = np.power(np.clip(linear_depth, 0.0, 1.0), args.gamma)

    # 3. Near clamp / foreground threshold (flattens foreground subjects onto camera focus plane)
    if args.near_threshold > 0.005:
        linear_depth[linear_depth <= args.near_threshold] = 0.0

    # 4. Sky & infinity threshold clamp
    if args.sky_threshold > 0.01:
        linear_depth[linear_depth >= args.sky_threshold] = 1.0

    # 5. Invert polarity if requested
    if args.invert:
        linear_depth = 1.0 - linear_depth
        
    linear_depth = np.clip(linear_depth, 0.0, 1.0).astype(np.float32)
    
    print("[PROGRESS 95%] [PROGRESS] 95: Saving depth outputs...", flush=True)
    if args.output_png:
        u16_depth = (linear_depth * 65535.0 + 0.5).astype(np.uint16)
        cv2.imwrite(args.output_png, u16_depth)
        print(f"[DepthAnythingV2] Saved 16-bit PNG depth map: {args.output_png}", flush=True)
        
    if args.output_sidecar:
        write_sldepth(args.output_sidecar, w, h, linear_depth, args.far_plane)
        print(f"[DepthAnythingV2] Saved .sldepth sidecar: {args.output_sidecar}", flush=True)
        
    if args.output_raw:
        with open(args.output_raw, "wb") as f:
            f.write(linear_depth.tobytes())
        print(f"[DepthAnythingV2] Saved raw depth: {args.output_raw}", flush=True)
        
    print("[PROGRESS 100%] [PROGRESS] 100: Done!", flush=True)
    print("[DepthAnythingV2] Complete!", flush=True)


if __name__ == "__main__":
    main()
