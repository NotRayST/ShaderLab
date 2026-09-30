import os
import sys
import argparse
import time
import struct
import zlib
import warnings
warnings.filterwarnings("ignore")

import numpy as np
import cv2
import torch

if sys.version_info >= (3, 7):
    try:
        sys.stdout.reconfigure(line_buffering=True)
        sys.stderr.reconfigure(line_buffering=True)
    except Exception:
        pass

script_dir = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, script_dir)
sys.path.insert(0, os.path.join(script_dir, "site-packages"))
sys.path.insert(0, os.path.abspath(os.path.join(script_dir, "..", "depth_anything_v2", "site-packages")))
sys.path.insert(0, os.path.abspath(os.path.join(script_dir, "..", "..", "common", "site-packages")))

if torch.cuda.is_available():
    torch.backends.cuda.matmul.allow_tf32 = True
    torch.backends.cudnn.allow_tf32 = True
    torch.backends.cudnn.benchmark = True


def resolve_device(requested):
    req = requested.lower()
    if req in ("auto", "cuda") and torch.cuda.is_available():
        name = torch.cuda.get_device_name(0) if torch.cuda.device_count() > 0 else "CUDA GPU"
        return torch.device("cuda"), f"NVIDIA CUDA ({name})"
    if req in ("auto", "directml", "dml"):
        try:
            import torch_directml
            if torch_directml.is_available():
                return torch_directml.device(), "DirectX 12 DirectML"
        except ImportError:
            pass
    return torch.device("cpu"), "CPU (AVX2)"


def parse_args():
    p = argparse.ArgumentParser(description="Run LaMa Big HD Inpainting for ShaderLab")
    p.add_argument("--daemon", action="store_true", help="Run as persistent background daemon")
    p.add_argument("--input", default="", help="Input RGB image")
    p.add_argument("--mask", default="", help="Input binary mask (white=erase, black=keep)")
    p.add_argument("--output", default="", help="Output image path")
    p.add_argument("--model", default="", help="Path to big-lama.pt")
    p.add_argument("--device", default="auto", help="Hardware device (auto/cuda/directml/cpu)")
    p.add_argument("--depth-sidecar", default="", help="Optional .sldepth sidecar to inpaint")
    p.add_argument("--expand-mask", type=int, default=-1, help="Dilation radius (-1=adaptive)")
    return p.parse_args()


def get_model_path(requested):
    if requested and os.path.exists(requested):
        return os.path.abspath(requested)
    candidates = [
        os.path.join(script_dir, "big-lama.pt"),
        os.path.join(script_dir, "..", "..", "common", "big-lama.pt"),
        os.path.join(script_dir, "..", "common", "big-lama.pt"),
        os.path.join(script_dir, "..", "lama", "big-lama.pt"),
    ]
    for c in candidates:
        if os.path.exists(c):
            return os.path.abspath(c)
    return requested


# ---------------------------------------------------------------------------
# core forward pass (LaMa Big HD)
# ---------------------------------------------------------------------------

def forward_patch(model, patch_bgr, patch_mask, device):
    orig_h, orig_w = patch_bgr.shape[:2]
    h, w = orig_h, orig_w
    pad_h = (8 - h % 8) % 8
    pad_w = (8 - w % 8) % 8
    if (w + pad_w) in (1024, 2048, 4096):
        pad_w += 16
    if (h + pad_h) in (1024, 2048, 4096):
        pad_h += 16
    if pad_h > 0 or pad_w > 0:
        patch_bgr = cv2.copyMakeBorder(patch_bgr, 0, pad_h, 0, pad_w, cv2.BORDER_REFLECT_101)
        patch_mask = cv2.copyMakeBorder(patch_mask, 0, pad_h, 0, pad_w, cv2.BORDER_REPLICATE)

    t_img = torch.from_numpy(np.ascontiguousarray(patch_bgr[:, :, ::-1])).permute(2, 0, 1).unsqueeze(0).to(device, dtype=torch.float32).div_(255.0)
    t_mask = torch.from_numpy(patch_mask > 120).unsqueeze(0).unsqueeze(0).to(device, dtype=torch.float32)
    t_img.mul_(1.0 - t_mask)

    with torch.inference_mode():
        out = model(t_img, t_mask)
    res = (out[0].permute(1, 2, 0) * 255.0).clamp_(0, 255).to(torch.uint8).cpu().numpy()
    res_bgr = res[:, :, ::-1]
    return res_bgr[:orig_h, :orig_w]


# ---------------------------------------------------------------------------
# adaptive mask preprocessing
# ---------------------------------------------------------------------------

def adaptive_dilate(mask_gray):
    thresh = (mask_gray > 120).astype(np.uint8) * 255
    contours, _ = cv2.findContours(thresh, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    if not contours:
        return mask_gray

    all_pts = np.concatenate(contours)
    bx, by, bw, bh = cv2.boundingRect(all_pts)
    diag = np.sqrt(bw * bw + bh * bh)
    radius = int(np.clip(diag * 0.025 + 14, 14, 40))

    kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (radius * 2 + 1, radius * 2 + 1))
    return cv2.dilate(thresh, kernel)


def merge_bounding_boxes(boxes):
    if not boxes:
        return []
    merged = [list(b) for b in boxes]
    changed = True
    while changed:
        changed = False
        i = 0
        while i < len(merged):
            j = i + 1
            while j < len(merged):
                a, b = merged[i], merged[j]
                if not (a[0] > b[2] or a[2] < b[0] or a[1] > b[3] or a[3] < b[1]):
                    merged[i] = [min(a[0], b[0]), min(a[1], b[1]), max(a[2], b[2]), max(a[3], b[3])]
                    merged.pop(j)
                    changed = True
                else:
                    j += 1
            i += 1
    return merged


# ---------------------------------------------------------------------------
# main inpainting orchestrator
# ---------------------------------------------------------------------------

def inpaint_image(model, img, mask, device):
    thresh = (mask > 120).astype(np.uint8) * 255
    contours_d, _ = cv2.findContours(thresh, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    if not contours_d:
        return img.copy()

    h, w = img.shape[:2]
    out_img = img.copy()

    raw_boxes = []
    for cnt in contours_d:
        cbx, cby, cbw, cbh = cv2.boundingRect(cnt)
        if cbw < 2 or cbh < 2:
            continue
        margin = min(512, max(128, int(max(cbw, cbh) * 0.7)))
        x1 = max(0, cbx - margin)
        y1 = max(0, cby - margin)
        x2 = min(w, cbx + cbw + margin)
        y2 = min(h, cby + cbh + margin)
        raw_boxes.append([x1, y1, x2, y2])

    merged_boxes = merge_bounding_boxes(raw_boxes)
    if not merged_boxes:
        return out_img

    num_boxes = len(merged_boxes)
    for box_idx, (x1, y1, x2, y2) in enumerate(merged_boxes):
        crop_w = x2 - x1
        crop_h = y2 - y1
        if crop_w <= 0 or crop_h <= 0:
            continue

        base_p = 35 + int((box_idx / num_boxes) * 50)
        print(f"[PROGRESS] {base_p}: Preparing region {box_idx + 1}/{num_boxes}...", flush=True)

        crop_img = out_img[y1:y2, x1:x2].copy()
        crop_mask = thresh[y1:y2, x1:x2].copy()

        # Pad canvas boundaries with reflection so FFC layers have surrounding context
        EDGE_PAD = 32
        pad_t = EDGE_PAD if y1 == 0 else 0
        pad_b = EDGE_PAD if y2 == h else 0
        pad_l = EDGE_PAD if x1 == 0 else 0
        pad_r = EDGE_PAD if x2 == w else 0

        if pad_t > 0 or pad_b > 0 or pad_l > 0 or pad_r > 0:
            padded_crop_img = cv2.copyMakeBorder(crop_img, pad_t, pad_b, pad_l, pad_r, cv2.BORDER_REFLECT_101)
            padded_crop_mask = cv2.copyMakeBorder(crop_mask, pad_t, pad_b, pad_l, pad_r, cv2.BORDER_REPLICATE)
        else:
            padded_crop_img = crop_img
            padded_crop_mask = crop_mask

        max_dim = max(padded_crop_img.shape[0], padded_crop_img.shape[1])

        # Scale down deep voids (>120px from boundary) so they fit inside LaMa's ~512px receptive field
        dist = cv2.distanceTransform((padded_crop_mask > 120).astype(np.uint8), cv2.DIST_L2, 5)
        max_dist = float(np.max(dist)) if dist.size > 0 else 0.0

        if max_dist > 120.0:
            scale = min(1.0, max(384.0 / max_dim, 140.0 / max_dist))
        elif max_dim > 1536:
            scale = 1536.0 / max_dim
        else:
            scale = 1.0

        print(f"[PROGRESS] {base_p + int(25 / num_boxes)}: Neural inpainting {box_idx + 1}/{num_boxes}...", flush=True)
        if scale < 0.95:
            cw_s = int(round(padded_crop_img.shape[1] * scale))
            ch_s = int(round(padded_crop_img.shape[0] * scale))
            scaled_img = cv2.resize(padded_crop_img, (cw_s, ch_s), interpolation=cv2.INTER_AREA)
            scaled_mask = cv2.resize(padded_crop_mask, (cw_s, ch_s), interpolation=cv2.INTER_NEAREST)
            scaled_out = forward_patch(model, scaled_img, scaled_mask, device)
            refined_padded = cv2.resize(scaled_out, (padded_crop_img.shape[1], padded_crop_img.shape[0]), interpolation=cv2.INTER_CUBIC)
        else:
            refined_padded = forward_patch(model, padded_crop_img, padded_crop_mask, device)

        end_y = (padded_crop_img.shape[0] - pad_b) if pad_b > 0 else None
        end_x = (padded_crop_img.shape[1] - pad_r) if pad_r > 0 else None
        refined = refined_padded[pad_t:end_y, pad_l:end_x]

        print(f"[PROGRESS] {base_p + int(40 / num_boxes)}: Filtering and blending...", flush=True)
        if refined.shape[0] >= 10 and refined.shape[1] >= 10:
            refined = cv2.edgePreservingFilter(refined, flags=1, sigma_s=20, sigma_r=0.10)

        m_f = (crop_mask > 120).astype(np.float32)
        area = np.sum(m_f)
        fr = int(np.clip(np.sqrt(area) * 0.05, 5, 25))
        ksize = fr * 2 + 1
        alpha = cv2.GaussianBlur(m_f, (ksize, ksize), fr * 0.35)[:, :, np.newaxis]

        final = crop_img.astype(np.float32) * (1.0 - alpha) + refined.astype(np.float32) * alpha
        out_img[y1:y2, x1:x2] = np.clip(final, 0, 255).astype(np.uint8)

    return out_img


def inpaint_depth_sidecar(sidecar_path, mask_gray):
    # depth buffer is never modified during erase
    return


def run_daemon(args):
    import json
    model_path = get_model_path(args.model)
    if not model_path or not os.path.exists(model_path):
        print(f"[ERROR] Model checkpoint not found at '{args.model}'", file=sys.stderr, flush=True)
        sys.exit(1)

    device, dev_name = resolve_device(args.device)
    print(f"[LaMa] Daemon initializing LaMa Big HD on {dev_name}...", flush=True)

    try:
        model = torch.jit.load(model_path, map_location="cpu").to(device)
        model.eval()
        if device.type == "cuda":
            dummy_img = torch.zeros(1, 3, 256, 256, device=device, dtype=torch.float32)
            dummy_mask = torch.zeros(1, 1, 256, 256, device=device, dtype=torch.float32)
            with torch.inference_mode():
                _ = model(dummy_img, dummy_mask)
            torch.cuda.synchronize()
    except Exception as e:
        print(f"[ERROR] Failed to load model: {e}", file=sys.stderr, flush=True)
        sys.exit(1)

    print("[READY]", flush=True)

    while True:
        line = sys.stdin.readline()
        if not line:
            break
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except Exception as e:
            print(f"[ERROR] JSON parse failed: {e}", flush=True)
            continue

        cmd = req.get("cmd", "inpaint")
        if cmd == "quit":
            break
        elif cmd == "ping":
            print("[PONG]", flush=True)
            continue
        elif cmd == "inpaint":
            inp_path = req.get("input", "")
            mask_path = req.get("mask", "")
            out_path = req.get("output", "")
            sidecar_path = req.get("depth_sidecar", "")
            expand_mask = req.get("expand_mask", -1)

            if not os.path.exists(inp_path) or not os.path.exists(mask_path):
                print(f"[ERROR] Input or mask file not found", flush=True)
                continue

            try:
                print("[PROGRESS] 10: Loading image and mask...", flush=True)
                img = cv2.imread(inp_path, cv2.IMREAD_COLOR)
                mask = cv2.imread(mask_path, cv2.IMREAD_GRAYSCALE)
                if img is None or mask is None:
                    print(f"[ERROR] Failed to decode image or mask", flush=True)
                    continue

                if mask.shape[:2] != img.shape[:2]:
                    mask = cv2.resize(mask, (img.shape[1], img.shape[0]), interpolation=cv2.INTER_NEAREST)

                print("[PROGRESS] 25: Preprocessing mask...", flush=True)
                mask = cv2.morphologyEx(mask, cv2.MORPH_CLOSE, cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (7, 7)))
                if expand_mask == -1:
                    mask = adaptive_dilate(mask)
                elif expand_mask > 0:
                    kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (expand_mask * 2 + 1, expand_mask * 2 + 1))
                    mask = cv2.dilate(mask, kernel)

                t0 = time.perf_counter()
                out_bgr = inpaint_image(model, img, mask, device)
                t1 = time.perf_counter()
                print(f"[LaMa] Inpainting took {(t1 - t0)*1000:.1f} ms", flush=True)

                print("[PROGRESS] 88: Saving result...", flush=True)
                os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
                cv2.imwrite(out_path, out_bgr)

                print("[PROGRESS] 100: Done!", flush=True)
                print("[DONE]", flush=True)
            except Exception as ex:
                print(f"[ERROR] Inpaint exception: {ex}", flush=True)


def main():
    args = parse_args()
    if args.daemon:
        run_daemon(args)
        return

    model_path = get_model_path(args.model)
    if not model_path or not os.path.exists(model_path):
        print(f"Error: model checkpoint not found at '{args.model}'", file=sys.stderr, flush=True)
        sys.exit(1)

    if not args.input or not os.path.exists(args.input):
        print(f"Error: input file '{args.input}' not found", file=sys.stderr, flush=True)
        sys.exit(1)

    if not args.mask or not os.path.exists(args.mask):
        print(f"Error: mask file '{args.mask}' not found", file=sys.stderr, flush=True)
        sys.exit(1)

    print("[PROGRESS] 10: Loading input image and mask...", flush=True)
    img = cv2.imread(args.input, cv2.IMREAD_COLOR)
    if img is None:
        print(f"Error: failed to load input image: {args.input}", file=sys.stderr, flush=True)
        sys.exit(1)

    mask = cv2.imread(args.mask, cv2.IMREAD_GRAYSCALE)
    if mask is None:
        print(f"Error: failed to load mask: {args.mask}", file=sys.stderr, flush=True)
        sys.exit(1)

    if mask.shape[:2] != img.shape[:2]:
        mask = cv2.resize(mask, (img.shape[1], img.shape[0]), interpolation=cv2.INTER_NEAREST)

    mask = cv2.morphologyEx(mask, cv2.MORPH_CLOSE, cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (7, 7)))

    if args.expand_mask == -1:
        print("[LaMa] Applying adaptive mask dilation...", flush=True)
        mask = adaptive_dilate(mask)
    elif args.expand_mask > 0:
        kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (args.expand_mask * 2 + 1, args.expand_mask * 2 + 1))
        mask = cv2.dilate(mask, kernel)

    print("[PROGRESS] 25: Initializing neural model...", flush=True)
    device, dev_name = resolve_device(args.device)
    print(f"[LaMa] Engine: LaMa Big HD on {dev_name}", flush=True)
    print(f"[LaMa] Pipeline: global context scaling + sub-pixel feathering", flush=True)
    print(f"[LaMa] Loading TorchScript model: {model_path}", flush=True)

    t0 = time.perf_counter()
    try:
        model = torch.jit.load(model_path, map_location="cpu").to(device)
        model.eval()
        print("[PROGRESS] 50: Inpainting with LaMa Big HD pipeline...", flush=True)
        with torch.inference_mode():
            out_bgr = inpaint_image(model, img, mask, device)
    except (torch.cuda.OutOfMemoryError, RuntimeError) as e:
        if "out of memory" in str(e).lower():
            print("[LaMa] CUDA OOM! Attempting CPU fallback...", flush=True)
            if torch.cuda.is_available():
                torch.cuda.empty_cache()
            cpu_dev = torch.device("cpu")
            model = torch.jit.load(model_path, map_location="cpu").to(cpu_dev).eval()
            with torch.inference_mode():
                out_bgr = inpaint_image(model, img, mask, cpu_dev)
        else:
            raise

    t1 = time.perf_counter()
    print(f"[LaMa] Inpainting finished in {(t1 - t0)*1000:.1f} ms", flush=True)

    print("[PROGRESS] 85: Saving result image...", flush=True)
    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    cv2.imwrite(args.output, out_bgr)

    print("[PROGRESS] 100: Done!", flush=True)
    print("[LaMa] Object removal complete with LaMa Big HD!", flush=True)


if __name__ == "__main__":
    main()
