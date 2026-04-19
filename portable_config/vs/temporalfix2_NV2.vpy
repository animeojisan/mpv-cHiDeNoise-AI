import math
import os
import shutil
import subprocess
from pathlib import Path

import vapoursynth as vs

core = vs.core

# Based on vs_temporalfix by pifroggi.
# Special thanks and respect to the original author for the design and implementation.
# This script is a concise mpv-oriented adaptation of the original TensorRT workflow.
# Additional playback-side adjustments are included for stable mpv use.

STRENGTH = 2
NUM_STREAMS = 1
TILES = 2
FORCE_REBUILD_ENGINE = False
AUTO_SET_OUTPUT = True
OUTPUT_TO_SOURCE_FORMAT = True

PAD_LEFT = 0
PAD_RIGHT = 128
PAD_TOP = 0
PAD_BOTTOM = 0

MODEL_SUBDIR = os.path.join("vs-plugins", "models", "55ai")
MODEL_FILENAME_MAP = {
    1: "temporalfix_s1_v1_op18_fp16.onnx",
    2: "temporalfix_s2_v1_op18_fp16.onnx",
    3: "temporalfix_s3_v1_op18_fp16.onnx",
}


def _log(msg: str) -> None:
    print(f"[temporalfix] {msg}")


def _guess_matrix_from_src(src: vs.VideoNode) -> str:
    w = int(src.width)
    h = int(src.height)
    if h <= 486:
        return "170m"
    if h <= 576 and w <= 1024:
        return "470bg"
    if w >= 3840 or h >= 2160:
        return "2020ncl"
    return "709"


def _find_root_dir() -> str:
    script_dir = os.path.dirname(os.path.realpath(__file__))
    candidates = [
        os.path.normpath(os.path.join(script_dir, "..", "..")),
        os.path.normpath(os.path.join(script_dir, "..")),
        os.path.normpath(script_dir),
        os.path.normpath(os.getcwd()),
        os.path.normpath(os.path.join(os.getcwd(), "..")),
    ]
    checked = []
    for c in candidates:
        if not c:
            continue
        c = os.path.normpath(c)
        if c in checked:
            continue
        checked.append(c)
        if os.path.isdir(os.path.join(c, "portable_config")) and os.path.isdir(os.path.join(c, "vs-plugins")):
            return c
        if os.path.isfile(os.path.join(c, "mpv.exe")) and os.path.isdir(os.path.join(c, "vs-plugins")):
            return c
    return checked[0]


def _resolve_local_dirs():
    root_dir = _find_root_dir()
    model_dir = os.path.join(root_dir, MODEL_SUBDIR)
    os.makedirs(model_dir, exist_ok=True)
    return root_dir, model_dir, model_dir


def _model_info_for_strength(strength: int):
    try:
        model_file = MODEL_FILENAME_MAP[int(strength)]
    except Exception as e:
        raise ValueError("Strength must be 1, 2, or 3.") from e
    model_name = os.path.splitext(model_file)[0].replace("_op18_fp16", "")
    return model_file, model_name


def _get_trtexec(root_dir: str) -> Path:
    exe_name = "trtexec.exe" if os.name == "nt" else "trtexec"
    local_trtexec = Path(root_dir) / "vs-plugins" / "vsmlrt-cuda" / exe_name
    if local_trtexec.is_file() and os.access(str(local_trtexec), os.X_OK):
        return local_trtexec

    try:
        info = core.trt.Version()
        path = info.get("path")
        if isinstance(path, bytes):
            path = path.decode(errors="ignore")
        if path:
            alt = Path(os.path.dirname(path)) / "vsmlrt-cuda" / exe_name
            if alt.is_file() and os.access(str(alt), os.X_OK):
                return alt
    except Exception as e:
        raise RuntimeError("Please install a version of vs-mlrt with TensorRT support.") from e

    system_trtexec = shutil.which("trtexec")
    if system_trtexec is not None:
        return Path(system_trtexec)

    raise FileNotFoundError("trtexec not found.")


def gen_shifts(clip: vs.VideoNode, radius: int):
    frames = clip.num_frames
    prefix = clip[1:radius + 1][::-1]
    suffix = clip[-2:-radius - 2:-1]
    padded = prefix + clip + suffix
    shifts = []
    for offset in range(-radius, radius + 1):
        if offset == 0:
            shifts.append(clip)
        else:
            start = radius + offset
            shifts.append(padded[start:start + frames])
    return shifts


def get_tiles(clip_w: int, clip_h: int, tiles: int, overlap: int = 0):
    if tiles not in (1, 2, 4, 6, 8):
        raise ValueError("Tiles must be 1, 2, 4, 6, or 8.")
    layouts = {
        1: [(1, 1)],
        2: [(2, 1), (1, 2)],
        4: [(4, 1), (2, 2), (1, 4)],
        6: [(6, 1), (3, 2), (2, 3), (1, 6)],
        8: [(8, 1), (4, 2), (2, 4), (1, 8)],
    }[tiles]

    def _tile_size(layout):
        cols, rows = layout
        tile_w = math.ceil((clip_w + 2 * overlap * (cols - 1)) / cols)
        tile_h = math.ceil((clip_h + 2 * overlap * (rows - 1)) / rows)
        return tile_w, tile_h

    def _layout_valid(layout):
        cols, rows = layout
        tile_w, tile_h = _tile_size(layout)
        if cols > 1 and tile_w <= 2 * overlap:
            return False
        if rows > 1 and tile_h <= 2 * overlap:
            return False
        return True

    def _score(layout):
        tile_w, tile_h = _tile_size(layout)
        cols, rows = layout
        tile_aspect = tile_w / tile_h
        square_error = abs(math.log(tile_aspect))
        orientation_penalty = rows if clip_w >= clip_h else cols
        balance_penalty = abs(cols - rows)
        return (square_error, orientation_penalty, balance_penalty)

    valid_layouts = [layout for layout in layouts if _layout_valid(layout)]
    if not valid_layouts:
        raise ValueError("Clip dimensions are too small for current tile amount.")
    cols, rows = min(valid_layouts, key=_score)
    tile_w, tile_h = _tile_size((cols, rows))
    return tile_w, tile_h, cols, rows


def _overlap_for_size(clip_w: int, clip_h: int) -> int:
    return min(64, int(64 * ((clip_w * clip_h) / (1920 * 1080)) ** 0.5))


def _engine_name(model_name: str, engine_w: int, engine_h: int) -> str:
    return f"{model_name}_h{engine_h}_w{engine_w}_fp16.engine"


def _build_or_get_engine(root_dir: str, onnx_path: str, engine_dir: str, model_name: str, engine_w: int, engine_h: int, force_rebuild: bool = False) -> str:
    engine_path = os.path.join(engine_dir, _engine_name(model_name, engine_w, engine_h))
    if force_rebuild and os.path.isfile(engine_path):
        try:
            os.remove(engine_path)
        except Exception:
            pass

    if os.path.isfile(engine_path) and os.path.getsize(engine_path) >= 512:
        _log(f"using existing engine: {engine_path}")
        return engine_path

    trtexec_path = _get_trtexec(root_dir)
    opt_shapes = f"input:1x21x{engine_h}x{engine_w}"
    cmd = [
        str(trtexec_path),
        "--stronglyTyped",
        "--inputIOFormats=fp16:chw",
        "--outputIOFormats=fp16:chw",
        "--skipInference",
        "--memPoolSize=workspace:4096",
        f"--onnx={onnx_path}",
        f"--saveEngine={engine_path}",
        f"--optShapes={opt_shapes}",
        "--builderOptimizationLevel=3",
    ]
    _log(f"building engine: {os.path.basename(engine_path)}")
    result = subprocess.run(cmd, check=False, cwd=engine_dir)
    if result.returncode != 0:
        raise RuntimeError(f"trtexec failed while building the TensorRT engine. Return code: {result.returncode}")
    if not os.path.isfile(engine_path) or os.path.getsize(engine_path) < 512:
        raise RuntimeError("trtexec finished without producing a valid engine file.")
    return engine_path


def _to_temporalfix_input(src: vs.VideoNode) -> vs.VideoNode:
    if src.format is None:
        raise ValueError("Input clip must have constant format.")
    if src.format.id == vs.RGBH:
        clip = src
    elif src.format.color_family == vs.RGB:
        clip = core.resize.Point(src, format=vs.RGBH)
    else:
        clip = core.resize.Point(src, format=vs.RGBH, matrix_in_s=_guess_matrix_from_src(src))
    return core.std.Expr(clip, expr=["x 0 max 1 min"])


def _pad_for_inference(clip: vs.VideoNode) -> vs.VideoNode:
    if not any((PAD_LEFT, PAD_RIGHT, PAD_TOP, PAD_BOTTOM)):
        return clip
    return core.std.AddBorders(
        clip,
        left=PAD_LEFT,
        right=PAD_RIGHT,
        top=PAD_TOP,
        bottom=PAD_BOTTOM,
        color=[0.0, 0.0, 0.0],
    )


def _crop_back(clip: vs.VideoNode, width: int, height: int) -> vs.VideoNode:
    if clip.width == width and clip.height == height:
        return clip
    return core.std.CropAbs(clip, width=width, height=height, left=PAD_LEFT, top=PAD_TOP)


def pack_temporal_rgb_planes(rgb_clip: vs.VideoNode, temporal_frames: int = 7):
    packed = []
    for shifted in gen_shifts(rgb_clip, radius=temporal_frames // 2):
        packed.extend([core.std.ShufflePlanes(shifted, planes=p, colorfamily=vs.GRAY) for p in (0, 1, 2)])
    return packed


def _run_local_tensorrt(clip: vs.VideoNode, strength: int, tiles: int, num_streams: int, force_rebuild: bool):
    if clip.format.id != vs.RGBH:
        raise ValueError("TensorRT backend requires RGBH input.")

    root_dir, model_dir, engine_dir = _resolve_local_dirs()
    model_file, model_name = _model_info_for_strength(strength)
    onnx_path = os.path.join(model_dir, model_file)
    if not os.path.isfile(onnx_path):
        raise FileNotFoundError(f"Temporalfix ONNX not found: {onnx_path}")

    overlap = _overlap_for_size(clip.width, clip.height)
    tile_w, tile_h, _, _ = get_tiles(clip.width, clip.height, tiles=tiles, overlap=overlap)

    engine_path = _build_or_get_engine(
        root_dir=root_dir,
        onnx_path=onnx_path,
        engine_dir=engine_dir,
        model_name=model_name,
        engine_w=tile_w,
        engine_h=tile_h,
        force_rebuild=force_rebuild,
    )

    packed_planes = pack_temporal_rgb_planes(clip, temporal_frames=7)
    model_args = dict(engine_path=engine_path, num_streams=num_streams)
    if tiles > 1:
        model_args.update(dict(tilesize=(tile_w, tile_h), overlap=(overlap, overlap)))

    attempts = [
        ("packed-list-positional", lambda: core.trt.Model(packed_planes, **model_args)),
        ("packed-tuple-positional", lambda: core.trt.Model(tuple(packed_planes), **model_args)),
        ("packed-kw-clips", lambda: core.trt.Model(clips=packed_planes, **model_args)),
    ]

    out = None
    errors = []
    for name, fn in attempts:
        try:
            _log(f"inference attempt: {name}")
            out = fn()
            _log(f"inference success: {name}")
            break
        except Exception as e:
            errors.append(f"{name}: {e}")
            _log(f"inference failed: {name}: {e}")

    if out is None:
        raise RuntimeError("TensorRT inference setup failed.\n" + "\n".join(errors))

    out = core.std.CopyFrameProps(out, clip)
    return out


def _to_playback_output(clip: vs.VideoNode, src: vs.VideoNode) -> vs.VideoNode:
    if not OUTPUT_TO_SOURCE_FORMAT or src.format is None:
        return core.resize.Bilinear(clip, format=vs.YUV444P16, matrix_s=_guess_matrix_from_src(src))

    if src.format.color_family == vs.RGB:
        return core.resize.Bilinear(clip, format=src.format.id)

    return core.resize.Bilinear(
        clip,
        format=src.format.id,
        matrix_s=_guess_matrix_from_src(src),
    )


def apply_temporalfix(src: vs.VideoNode) -> vs.VideoNode:
    src_w = src.width
    src_h = src.height
    tf_in = _to_temporalfix_input(src)
    tf_in = _pad_for_inference(tf_in)
    fixed = _run_local_tensorrt(
        tf_in,
        strength=STRENGTH,
        tiles=TILES,
        num_streams=NUM_STREAMS,
        force_rebuild=FORCE_REBUILD_ENGINE,
    )
    fixed = _crop_back(fixed, src_w, src_h)
    fixed = _to_playback_output(fixed, src)
    return fixed


clip = globals().get("video_in")
if clip is None:
    raise RuntimeError("This script expects mpv's vapoursynth filter or a caller that defines video_in.")
clip = apply_temporalfix(clip)
if AUTO_SET_OUTPUT:
    clip.set_output()
