import math
import os
import re

import vapoursynth as vs

core = vs.core

# Based on vs_temporalfix by pifroggi.
# Special thanks and respect to the original author for the design and implementation.
# This script is a concise mpv-oriented DirectML adaptation of the temporalfix 2.0 ONNX workflow.
# It is designed to run as a single self-contained .vpy file.

STRENGTH = 3
GPU_ID = 0
NUM_STREAMS = 1
FP16_QNT = True
TILES = 2
AUTO_SET_OUTPUT = True
OUTPUT_TO_SOURCE_FORMAT = True
LOG = False

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


class Version:
    def __init__(self, vstring=None):
        if vstring:
            self.parse(vstring)

    def __eq__(self, other):
        c = self._cmp(other)
        if c is NotImplemented:
            return c
        return c == 0

    def __lt__(self, other):
        c = self._cmp(other)
        if c is NotImplemented:
            return c
        return c < 0

    def __le__(self, other):
        c = self._cmp(other)
        if c is NotImplemented:
            return c
        return c <= 0

    def __gt__(self, other):
        c = self._cmp(other)
        if c is NotImplemented:
            return c
        return c > 0

    def __ge__(self, other):
        c = self._cmp(other)
        if c is NotImplemented:
            return c
        return c >= 0


class LooseVersion(Version):
    component_re = re.compile(r"(\d+ | [a-z]+ | \.)", re.VERBOSE)

    def __init__(self, vstring=None):
        if vstring:
            self.parse(vstring)

    def parse(self, vstring):
        self.vstring = vstring
        components = [x for x in self.component_re.split(vstring) if x and x != "."]
        for i, obj in enumerate(components):
            try:
                components[i] = int(obj)
            except ValueError:
                pass
        self.version = components

    def _cmp(self, other):
        if isinstance(other, str):
            other = LooseVersion(other)
        elif not isinstance(other, LooseVersion):
            return NotImplemented
        if self.version == other.version:
            return 0
        if self.version < other.version:
            return -1
        if self.version > other.version:
            return 1
        return 0


vs_thd_init = os.cpu_count() or 8
if vs_thd_init > 8 and vs_thd_init <= 16:
    vs_thd_dft = 8
elif vs_thd_init > 16:
    if vs_thd_init <= 32:
        vs_thd_dft = vs_thd_init // 2
        if vs_thd_dft % 2 != 0:
            vs_thd_dft -= 1
    else:
        vs_thd_dft = 16
else:
    vs_thd_dft = vs_thd_init

if vs.__api_version__.api_major < 4:
    raise ImportError("VapourSynth version too old; R57+/API4 required")

vsmlrt = None
onnx = None


def _log(msg: str) -> None:
    if LOG:
        print(f"[temporalfix_dml] {msg}")


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
    try:
        script_dir = os.path.dirname(os.path.realpath(__file__))
    except Exception:
        script_dir = os.getcwd()

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
    return checked[0] if checked else os.getcwd()


def _resolve_local_dirs():
    root_dir = _find_root_dir()
    model_dir = os.path.join(root_dir, MODEL_SUBDIR)
    os.makedirs(model_dir, exist_ok=True)
    return root_dir, model_dir


def _model_info_for_strength(strength: int):
    try:
        model_file = MODEL_FILENAME_MAP[int(strength)]
    except Exception as e:
        raise ValueError("Strength must be 1, 2, or 3.") from e
    model_name = os.path.splitext(model_file)[0].replace("_op18_fp16", "")
    return model_file, model_name


def _load_onnx():
    global onnx
    if onnx is None:
        try:
            import onnx as _onnx
        except ImportError as exc:
            raise ImportError("temporalfix2_DML1: missing Python package 'onnx'") from exc
        onnx = _onnx
    return onnx


def _load_vsmlrt():
    global vsmlrt
    if vsmlrt is None:
        try:
            import vsmlrt as _vsmlrt
        except ImportError as exc:
            raise ImportError("temporalfix2_DML1: missing script 'vsmlrt'") from exc
        vsmlrt = _vsmlrt

    if LooseVersion(vsmlrt.__version__) < LooseVersion("3.15.25"):
        raise ImportError("temporalfix2_DML1: vsmlrt is too old; need >= 3.15.25")

    return vsmlrt


def _require_ort_plugin() -> None:
    if not hasattr(core, "ort"):
        raise ModuleNotFoundError("temporalfix2_DML1: missing plugin 'ort' (vs-onnxruntime / DirectML plugin)")


def _onnx_info(path: str) -> dict:
    _onnx = _load_onnx()
    model = _onnx.load(path)
    graph_input = model.graph.input[0]
    tensor_type = graph_input.type.tensor_type
    elem_type = tensor_type.elem_type

    dims = []
    for dim in tensor_type.shape.dim:
        if dim.HasField("dim_value"):
            dims.append(dim.dim_value)
        elif dim.HasField("dim_param"):
            dims.append(dim.dim_param)
        else:
            dims.append(None)

    outputs = []
    for out in model.graph.output:
        odims = []
        for dim in out.type.tensor_type.shape.dim:
            if dim.HasField("dim_value"):
                odims.append(dim.dim_value)
            elif dim.HasField("dim_param"):
                odims.append(dim.dim_param)
            else:
                odims.append(None)
        outputs.append({"name": out.name, "dims": odims})

    return {
        "elem_type": elem_type,
        "rank": len(dims),
        "dims": dims,
        "input_name": graph_input.name,
        "outputs": outputs,
    }


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


def _resolve_model_path(strength: int) -> str:
    _, model_dir = _resolve_local_dirs()
    model_file, _ = _model_info_for_strength(strength)
    model_path = os.path.join(model_dir, model_file)
    if not os.path.isfile(model_path):
        raise FileNotFoundError(f"Temporalfix ONNX not found: {model_path}")
    return model_path


def _run_local_dml(clip: vs.VideoNode, strength: int, tiles: int, num_streams: int, gpu_id: int, fp16_qnt: bool):
    if clip.format.id not in (vs.RGBH, vs.RGBS):
        raise ValueError("DirectML backend requires RGBH/RGBS input.")

    _require_ort_plugin()
    _vsmlrt = _load_vsmlrt()
    model_path = _resolve_model_path(strength)
    model_info = _onnx_info(model_path)

    if model_info["rank"] != 4:
        raise RuntimeError(f"Expected 4D packed temporal ONNX input, got rank={model_info['rank']}")

    cdim = model_info["dims"][1]
    if not isinstance(cdim, int) or cdim < 6 or cdim % 3 != 0:
        raise RuntimeError(f"Unsupported packed temporal channel count: {cdim}")

    temporal_frames = cdim // 3
    packed_planes = pack_temporal_rgb_planes(clip, temporal_frames=temporal_frames)

    elem_type = model_info["elem_type"]
    if elem_type == 10:
        fp16_qnt = True
    elif elem_type != 1:
        raise RuntimeError(f"Unsupported model input precision elem_type={elem_type}")

    overlap = _overlap_for_size(clip.width, clip.height) if tiles > 1 else 0
    infer_kwargs = {
        "clips": packed_planes,
        "network_path": model_path,
        "backend": _vsmlrt.BackendV2.ORT_DML(device_id=gpu_id, num_streams=num_streams, fp16=fp16_qnt),
        "input_name": model_info["input_name"],
    }

    if tiles > 1:
        tile_w, tile_h, _, _ = get_tiles(clip.width, clip.height, tiles=tiles, overlap=overlap)
        infer_kwargs.update({
            "tilesize": (tile_w, tile_h),
            "overlap": (overlap, overlap),
        })
        _log(f"tiling enabled: {tile_w}x{tile_h}, overlap={overlap}")

    _log(f"model={os.path.basename(model_path)} input_name={model_info['input_name']} dims={model_info['dims']}")
    _log(f"gpu={gpu_id} streams={num_streams} fp16_qnt={fp16_qnt} temporal_frames={temporal_frames}")

    out = _vsmlrt.inference(**infer_kwargs)
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
    fixed = _run_local_dml(
        tf_in,
        strength=STRENGTH,
        tiles=TILES,
        num_streams=NUM_STREAMS,
        gpu_id=GPU_ID,
        fp16_qnt=FP16_QNT,
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
