# onnxVSR.py
#
# 主な参考元・ベースの一部:
# hooke007/mpv_PlayKit
# https://github.com/hooke007/mpv_PlayKit
#
# 関連するツール、スクリプト、設定を公開・共有してくださった
# hooke007 さんおよび各制作者・貢献者の方々に敬意と感謝を表します。
#
# This file contains project-specific adjustments and modifications.
# See NOTICE.txt for credits, references, and provenance notes.

import os
import re
import vapoursynth as vs

core = vs.core

__version__ = "0.4.0-filelog"
__all__ = [
    "UAI_DML",
    "VSR_DML",
    "ONNX_INFO",
    "ONNX_ANZ",
    "resolve_model_path",
    "supports_temporal_input",
]


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

vs_api = vs.__api_version__.api_major
if vs_api < 4:
    raise ImportError("VapourSynth version too old; R57+/API4 required")

vsmlrt = None
onnx = None


def _append_log_line(path: str, line: str) -> None:
    try:
        log_dir = os.path.dirname(path)
        if log_dir:
            os.makedirs(log_dir, exist_ok=True)
        with open(path, "a", encoding="utf-8", errors="ignore") as f:
            f.write(line + "\n")
    except Exception:
        pass


def _default_log_path() -> str:
    try:
        cwd = os.getcwd()
    except Exception:
        cwd = "."

    candidates = []
    try:
        candidates.append(os.path.join(cwd, "portable_config", "cache", "onnxvsr_debug.log"))
        candidates.append(os.path.join(cwd, "cache", "onnxvsr_debug.log"))
        candidates.append(os.path.join(cwd, "onnxvsr_debug.log"))
    except Exception:
        pass

    for cand in candidates:
        try:
            parent = os.path.dirname(cand)
            if parent:
                os.makedirs(parent, exist_ok=True)
            return cand
        except Exception:
            continue

    return "onnxvsr_debug.log"


def _log(enabled: bool, msg: str, log_file: str = None) -> None:
    if not enabled:
        return
    line = f"[onnxVSR] {msg}"
    print(line)
    _append_log_line(log_file or _default_log_path(), line)



def _require_ort_plugin(func_name: str) -> None:
    if not hasattr(core, "ort"):
        raise ModuleNotFoundError(f"{func_name}: missing plugin 'ort' (vs-onnxruntime / DirectML plugin)")


def _require_akarin(func_name: str) -> None:
    if not hasattr(core, "akarin"):
        raise ModuleNotFoundError(f"{func_name}: clamp=True requires plugin 'akarin'")


def resolve_model_path(model_pth: str) -> str:
    if not isinstance(model_pth, str) or len(model_pth) <= 3:
        raise vs.Error("resolve_model_path: invalid model_pth")

    _require_ort_plugin("resolve_model_path")
    plg_dir = os.path.dirname(core.ort.Version()["path"]).decode()
    mdl_pth_rel = os.path.join(plg_dir, "models", model_pth)

    if os.path.exists(mdl_pth_rel):
        return mdl_pth_rel
    if os.path.exists(model_pth):
        return model_pth

    raise vs.Error(f"resolve_model_path: model not found: {model_pth}")


def _load_onnx():
    global onnx
    if onnx is None:
        try:
            import onnx as _onnx
        except ImportError as exc:
            raise ImportError("ONNX_INFO: missing Python package 'onnx'") from exc
        onnx = _onnx
    return onnx


def ONNX_INFO(input: str = "", check_mdl: bool = True) -> dict:
    func_name = "ONNX_INFO"
    if not isinstance(input, str) or not input:
        raise vs.Error(f"{func_name}: invalid input path")

    _onnx = _load_onnx()

    if check_mdl:
        from onnx.checker import ValidationError
        try:
            _onnx.checker.check_model(input)
        except ValidationError as e:
            print(f"{func_name}: invalid model warning: {e}")
        except Exception as e:
            print(f"{func_name}: check warning: {e}")

    model = _onnx.load(input)
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


ONNX_ANZ = ONNX_INFO


def supports_temporal_input(model_pth: str) -> bool:
    info = ONNX_INFO(model_pth)
    if info["rank"] >= 5:
        return True
    if info["rank"] == 4:
        cdim = info["dims"][1]
        if isinstance(cdim, int) and cdim >= 6 and cdim % 3 == 0:
            return True
    return False


def _load_vsmlrt(func_name: str):
    global vsmlrt
    if vsmlrt is None:
        try:
            import vsmlrt as _vsmlrt
        except ImportError as exc:
            raise ImportError(f"{func_name}: missing script 'vsmlrt'") from exc
        vsmlrt = _vsmlrt

    if LooseVersion(vsmlrt.__version__) < LooseVersion("3.15.25"):
        raise ImportError(f"{func_name}: vsmlrt is too old; need >= 3.15.25")

    return vsmlrt


def _repeat_frame(clip: vs.VideoNode, index: int, count: int) -> vs.VideoNode:
    if count <= 0:
        return None
    frame = core.std.Trim(clip, first=index, last=index)
    return core.std.Loop(frame, times=count)


def _shift_with_edge_pad(clip: vs.VideoNode, offset: int) -> vs.VideoNode:
    if offset == 0:
        return clip
    if clip.num_frames <= 1:
        return clip
    if offset < 0:
        pad = -offset
        head = _repeat_frame(clip, 0, pad)
        body = core.std.Trim(clip, first=0, last=clip.num_frames - pad - 1)
        return head + body
    pad = offset
    body = core.std.Trim(clip, first=pad, last=clip.num_frames - 1)
    tail = _repeat_frame(clip, clip.num_frames - 1, pad)
    return body + tail


def _split_rgb_planes(clip: vs.VideoNode):
    return [core.std.ShufflePlanes(clip, planes=p, colorfamily=vs.GRAY) for p in (0, 1, 2)]




def _crop_abs(clip: vs.VideoNode, width: int, height: int, left: int, top: int) -> vs.VideoNode:
    return core.std.CropAbs(clip, width=width, height=height, left=left, top=top)


def _reflect_pad_axis(clip: vs.VideoNode, before: int, after: int, horizontal: bool = True) -> vs.VideoNode:
    if before < 0 or after < 0:
        raise vs.Error("_reflect_pad_axis: before/after must be >= 0")
    if before == 0 and after == 0:
        return clip

    w = clip.width
    h = clip.height

    def take_edge(amount: int, from_start: bool):
        if amount <= 0:
            return None
        parts = []
        remaining = amount
        while remaining > 0:
            chunk = min(remaining, w if horizontal else h)
            if horizontal:
                left = 0 if from_start else w - chunk
                part = _crop_abs(clip, chunk, h, left, 0)
                part = core.std.FlipHorizontal(part)
            else:
                top = 0 if from_start else h - chunk
                part = _crop_abs(clip, w, chunk, 0, top)
                part = core.std.FlipVertical(part)
            parts.append(part)
            remaining -= chunk
        if horizontal:
            return parts[0] if len(parts) == 1 else core.std.StackHorizontal(parts)
        return parts[0] if len(parts) == 1 else core.std.StackVertical(parts)

    left_or_top = take_edge(before, True)
    right_or_bottom = take_edge(after, False)

    if horizontal:
        parts = [x for x in (left_or_top, clip, right_or_bottom) if x is not None]
        return parts[0] if len(parts) == 1 else core.std.StackHorizontal(parts)
    parts = [x for x in (left_or_top, clip, right_or_bottom) if x is not None]
    return parts[0] if len(parts) == 1 else core.std.StackVertical(parts)


def _reflect_pad(clip: vs.VideoNode, left: int, right: int, top: int, bottom: int) -> vs.VideoNode:
    padded = _reflect_pad_axis(clip, left, right, horizontal=True)
    padded = _reflect_pad_axis(padded, top, bottom, horizontal=False)
    return padded


def _compute_balanced_padding(size: int, multiple: int):
    if multiple <= 0:
        return 0, 0
    target = ((size + multiple - 1) // multiple) * multiple
    pad = max(0, target - size)
    before = pad // 2
    after = pad - before
    return before, after


def _center_crop(clip: vs.VideoNode, width: int, height: int, left: int, top: int) -> vs.VideoNode:
    return core.std.CropAbs(clip, width=width, height=height, left=left, top=top)

def UAI_DML(
    input: vs.VideoNode,
    clamp: bool = False,
    model_pth: str = "",
    fp16_qnt: bool = True,
    gpu: int = 0,
    gpu_t: int = 2,
    vs_t: int = vs_thd_dft,
    log: bool = False,
    log_file: str = None,
) -> vs.VideoNode:
    func_name = "UAI_DML"

    if not isinstance(input, vs.VideoNode):
        raise vs.Error(f"{func_name}: invalid input")
    if not isinstance(clamp, bool):
        raise vs.Error(f"{func_name}: invalid clamp")
    if not isinstance(model_pth, str) or len(model_pth) <= 3:
        raise vs.Error(f"{func_name}: invalid model_pth")
    if not isinstance(fp16_qnt, bool):
        raise vs.Error(f"{func_name}: invalid fp16_qnt")
    if not isinstance(gpu, int) or gpu < 0:
        raise vs.Error(f"{func_name}: invalid gpu")
    if not isinstance(gpu_t, int) or gpu_t <= 0:
        raise vs.Error(f"{func_name}: invalid gpu_t")
    if not isinstance(vs_t, int) or vs_t <= 0 or vs_t > vs_thd_init:
        raise vs.Error(f"{func_name}: invalid vs_t")

    _require_ort_plugin(func_name)
    if clamp:
        _require_akarin(func_name)

    mdl_pth = resolve_model_path(model_pth)
    model_info = ONNX_INFO(mdl_pth)
    _log(log, f"{func_name}: model={mdl_pth}", log_file)
    _log(log, f"{func_name}: input_name={model_info['input_name']} dims={model_info['dims']} elem_type={model_info['elem_type']}", log_file)
    _log(log, f"{func_name}: backend=ORT_DML gpu={gpu} streams={gpu_t} vs_threads={vs_t}", log_file)
    _log(log, f"{func_name}: clip_in={input.width}x{input.height} frames={input.num_frames}", log_file)

    if model_info["rank"] >= 5:
        raise vs.Error(
            f"{func_name}: temporal/VSR model detected (rank={model_info['rank']}). Use VSR_DML() instead."
        )

    _vsmlrt = _load_vsmlrt(func_name)

    core.num_threads = vs_t
    fmt_in = input.format.id
    colorlv = getattr(input.get_frame(0).props, "_ColorRange", 0)

    elem_type = model_info["elem_type"]
    if elem_type == 1:
        fp16_mdl = False
    elif elem_type == 10:
        fp16_mdl = True
    else:
        raise vs.Error(f"{func_name}: unsupported model input precision ({elem_type})")

    if fp16_mdl:
        fp16_qnt = True

    clip = core.resize.Bilinear(input, format=vs.RGBH if fp16_qnt else vs.RGBS, matrix_in_s="709")

    if clamp:
        clip = core.akarin.Expr(clips=clip, expr="x 0 1 clamp")

    be_param = _vsmlrt.BackendV2.ORT_DML(device_id=gpu, num_streams=gpu_t, fp16=fp16_qnt)
    infer = _vsmlrt.inference(clips=clip, network_path=mdl_pth, backend=be_param, input_name=model_info["input_name"])
    _log(log, f"{func_name}: inference submitted", log_file)

    output = core.resize.Bilinear(infer, format=fmt_in, matrix_s="709", range=1 if colorlv == 0 else None)
    _log(log, f"{func_name}: output={output.width}x{output.height}", log_file)
    return output


def VSR_DML(
    input: vs.VideoNode,
    clamp: bool = False,
    model_pth: str = "",
    fp16_qnt: bool = True,
    gpu: int = 0,
    gpu_t: int = 2,
    vs_t: int = vs_thd_dft,
    log: bool = False,
    log_file: str = None,
    pad_mode: str = "none",
    pad_multiple: int = 0,
    crop_output: bool = True,
) -> vs.VideoNode:
    func_name = "VSR_DML"

    if not isinstance(input, vs.VideoNode):
        raise vs.Error(f"{func_name}: invalid input")
    if not isinstance(model_pth, str) or len(model_pth) <= 3:
        raise vs.Error(f"{func_name}: invalid model_pth")
    if not isinstance(pad_mode, str) or pad_mode.lower() not in ("none", "reflect"):
        raise vs.Error(f"{func_name}: pad_mode must be 'none' or 'reflect'")
    if not isinstance(pad_multiple, int) or pad_multiple < 0:
        raise vs.Error(f"{func_name}: pad_multiple must be >= 0")
    if not isinstance(crop_output, bool):
        raise vs.Error(f"{func_name}: crop_output must be bool")
    pad_mode = pad_mode.lower()

    _require_ort_plugin(func_name)
    if clamp:
        _require_akarin(func_name)

    mdl_pth = resolve_model_path(model_pth)
    model_info = ONNX_INFO(mdl_pth)
    _vsmlrt = _load_vsmlrt(func_name)

    _log(log, f"{func_name}: model={mdl_pth}", log_file)
    _log(log, f"{func_name}: input_name={model_info['input_name']} dims={model_info['dims']} elem_type={model_info['elem_type']}", log_file)
    _log(log, f"{func_name}: backend=ORT_DML gpu={gpu} streams={gpu_t} vs_threads={vs_t}", log_file)
    _log(log, f"{func_name}: clip_in={input.width}x{input.height} frames={input.num_frames}", log_file)

    dims = model_info["dims"]
    if model_info["rank"] != 4:
        raise vs.Error(f"{func_name}: expected packed 4D ONNX input like [N, T*C, H, W], got rank={model_info['rank']}")

    cdim = dims[1]
    if not isinstance(cdim, int) or cdim < 6 or cdim % 3 != 0:
        raise vs.Error(f"{func_name}: unsupported packed channel count: {cdim}")

    num_frames = cdim // 3
    radius = num_frames // 2
    _log(log, f"{func_name}: inferred temporal frames={num_frames} radius={radius}", log_file)
    _log(log, f"{func_name}: pad_mode={pad_mode} pad_multiple={pad_multiple} crop_output={crop_output}", log_file)

    core.num_threads = vs_t
    fmt_in = input.format.id
    colorlv = getattr(input.get_frame(0).props, "_ColorRange", 0)

    elem_type = model_info["elem_type"]
    if elem_type == 1:
        fp16_mdl = False
    elif elem_type == 10:
        fp16_mdl = True
    else:
        raise vs.Error(f"{func_name}: unsupported model input precision ({elem_type})")

    if fp16_mdl:
        fp16_qnt = True

    clip = core.resize.Bilinear(input, format=vs.RGBH if fp16_qnt else vs.RGBS, matrix_in_s="709")
    if clamp:
        clip = core.akarin.Expr(clips=clip, expr="x 0 1 clamp")

    pad_left = pad_right = pad_top = pad_bottom = 0
    if pad_mode == "reflect" and pad_multiple > 1:
        pad_left, pad_right = _compute_balanced_padding(clip.width, pad_multiple)
        pad_top, pad_bottom = _compute_balanced_padding(clip.height, pad_multiple)
        if pad_left or pad_right or pad_top or pad_bottom:
            _log(log, f"{func_name}: applying reflect padding left={pad_left} right={pad_right} top={pad_top} bottom={pad_bottom}", log_file)
            clip = _reflect_pad(clip, pad_left, pad_right, pad_top, pad_bottom)
            _log(log, f"{func_name}: padded_clip={clip.width}x{clip.height}", log_file)

    packed = []
    for offset in range(-radius, radius + 1):
        shifted = _shift_with_edge_pad(clip, offset)
        packed.extend(_split_rgb_planes(shifted))

    _log(log, f"{func_name}: packed_inputs={len(packed)} (expected={cdim})", log_file)

    be_param = _vsmlrt.BackendV2.ORT_DML(device_id=gpu, num_streams=gpu_t, fp16=fp16_qnt)
    infer = _vsmlrt.inference(
        clips=packed,
        network_path=mdl_pth,
        backend=be_param,
        input_name=model_info["input_name"],
    )
    _log(log, f"{func_name}: inference submitted", log_file)

    if crop_output and (pad_left or pad_right or pad_top or pad_bottom):
        scale_num_w = infer.width
        scale_den_w = clip.width
        scale_num_h = infer.height
        scale_den_h = clip.height
        crop_left = (pad_left * scale_num_w) // scale_den_w
        crop_top = (pad_top * scale_num_h) // scale_den_h
        out_w = (input.width * scale_num_w) // scale_den_w
        out_h = (input.height * scale_num_h) // scale_den_h
        _log(log, f"{func_name}: crop_output left={crop_left} top={crop_top} width={out_w} height={out_h}", log_file)
        infer = _center_crop(infer, width=out_w, height=out_h, left=crop_left, top=crop_top)

    output = core.resize.Bilinear(infer, format=fmt_in, matrix_s="709", range=1 if colorlv == 0 else None)
    _log(log, f"{func_name}: output={output.width}x{output.height}", log_file)
    return output
