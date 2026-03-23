
"""
onnxVSR.py - DirectML / TensorRT ONNX wrapper for mpv/VapourSynth use.

This revision adds:
- explicit DML / TRT model-path resolution
- file logging to portable_config/cache/onnxvsr_debug.log
- temporal packed-input VSR_DML / VSR_TRT for TSPAN-like ONNX exports
- clear backend-entry diagnostics so silent "no engine build" cases are easier to debug
"""

import os
import re
import typing

import vapoursynth as vs

core = vs.core

__version__ = "0.3.0"
__all__ = [
    "UAI_DML", "UAI_TRT",
    "VSR_DML", "VSR_TRT",
    "UAI", "VSR",
    "ONNX_INFO", "ONNX_ANZ",
    "resolve_model_path", "resolve_model_path_ort", "resolve_model_path_trt",
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
        return (self.version > other.version) - (self.version < other.version)


vs_thd_init = os.cpu_count() or 8
if vs_thd_init > 8 and vs_thd_init <= 16:
    vs_thd_dft = 8
elif vs_thd_init > 16:
    if vs_thd_init <= 32:
        vs_thd_dft = vs_thd_init // 2
        if vs_thd_dft % 2 != 0:
            vs_thd_dft = vs_thd_dft - 1
    else:
        vs_thd_dft = 16
else:
    vs_thd_dft = vs_thd_init

vs_api = vs.__api_version__.api_major
if vs_api < 4:
    raise ImportError("VapourSynth version too old; R57+/API4 required")

vsmlrt = None
onnx = None


def _debug_log_path() -> str:
    candidates = []
    try:
        cwd = os.getcwd()
        candidates.append(os.path.join(cwd, "portable_config", "cache", "onnxvsr_debug.log"))
        candidates.append(os.path.join(cwd, "cache", "onnxvsr_debug.log"))
    except Exception:
        pass

    try:
        here = os.path.dirname(os.path.abspath(__file__))
        candidates.append(os.path.join(here, "portable_config", "cache", "onnxvsr_debug.log"))
        candidates.append(os.path.join(os.path.dirname(here), "portable_config", "cache", "onnxvsr_debug.log"))
        candidates.append(os.path.join(here, "cache", "onnxvsr_debug.log"))
    except Exception:
        pass

    for path in candidates:
        try:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            return path
        except Exception:
            continue
    return os.path.abspath("onnxvsr_debug.log")


def _log(msg: str, enabled: bool = True) -> None:
    if not enabled:
        return
    line = f"[onnxVSR] {msg}"
    try:
        with open(_debug_log_path(), "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except Exception:
        pass


def _require_ort_plugin(func_name: str) -> None:
    if not hasattr(core, "ort"):
        raise ModuleNotFoundError(f"{func_name}: missing plugin 'ort' (vs-onnxruntime / DirectML plugin)")


def _require_trt_plugin(func_name: str) -> str:
    if hasattr(core, "trt"):
        return "trt"
    raise ModuleNotFoundError(f"{func_name}: missing plugin 'trt' (vsmlrt-cuda / TensorRT plugin)")


def _require_akarin(func_name: str) -> None:
    if not hasattr(core, "akarin"):
        raise ModuleNotFoundError(f"{func_name}: clamp=True requires plugin 'akarin'")


def _plugin_dir_from_attr(attr: str) -> str:
    obj = getattr(core, attr)
    return os.path.dirname(obj.Version()["path"]).decode()


def resolve_model_path_ort(model_pth: str) -> str:
    if not isinstance(model_pth, str) or len(model_pth) <= 3:
        raise vs.Error("resolve_model_path_ort: invalid model_pth")
    _require_ort_plugin("resolve_model_path_ort")
    plg_dir = _plugin_dir_from_attr("ort")
    mdl_pth_rel = os.path.join(plg_dir, "models", model_pth)
    if os.path.exists(mdl_pth_rel):
        return mdl_pth_rel
    if os.path.exists(model_pth):
        return model_pth
    raise vs.Error(f"resolve_model_path_ort: model not found: {model_pth}")


def resolve_model_path_trt(model_pth: str) -> str:
    if not isinstance(model_pth, str) or len(model_pth) <= 3:
        raise vs.Error("resolve_model_path_trt: invalid model_pth")
    _require_trt_plugin("resolve_model_path_trt")
    plg_dir = _plugin_dir_from_attr("trt")
    mdl_pth_rel = os.path.join(plg_dir, "models", model_pth)
    if os.path.exists(mdl_pth_rel):
        return mdl_pth_rel
    if os.path.exists(model_pth):
        return model_pth
    raise vs.Error(f"resolve_model_path_trt: model not found: {model_pth}")


def resolve_model_path(model_pth: str, backend: str = "dml") -> str:
    backend = (backend or "dml").lower()
    if backend == "dml":
        return resolve_model_path_ort(model_pth)
    if backend == "trt":
        return resolve_model_path_trt(model_pth)
    raise vs.Error(f"resolve_model_path: unsupported backend={backend}")


def _load_onnx():
    global onnx
    if onnx is None:
        import onnx as _onnx
        onnx = _onnx
    return onnx


def ONNX_INFO(input: str = "", check_mdl: bool = False) -> dict:
    func_name = "ONNX_INFO"
    if not isinstance(input, str) or not input:
        raise vs.Error(f"{func_name}: invalid input path")

    _onnx = _load_onnx()

    if check_mdl:
        from onnx.checker import ValidationError
        try:
            _onnx.checker.check_model(input)
        except ValidationError as e:
            _log(f"{func_name}: invalid model warning: {e}", True)
        except Exception as e:
            _log(f"{func_name}: check warning: {e}", True)

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

    return {
        "elem_type": elem_type,
        "rank": len(dims),
        "dims": dims,
        "input_name": graph_input.name,
    }


ONNX_ANZ = ONNX_INFO


def supports_temporal_input(model_pth: str, backend: str = "dml") -> bool:
    info = ONNX_INFO(resolve_model_path(model_pth, backend=backend))
    dims = info["dims"]
    return info["rank"] >= 5 or (info["rank"] == 4 and isinstance(dims[1], int) and dims[1] > 3 and dims[1] % 3 == 0)


def _load_vsmlrt(func_name: str, need_trt: bool = False):
    global vsmlrt
    if vsmlrt is None:
        import vsmlrt as _vsmlrt
        vsmlrt = _vsmlrt

    min_ver = "3.18.1" if need_trt else "3.15.25"
    if LooseVersion(vsmlrt.__version__) < LooseVersion(min_ver):
        raise ImportError(f"{func_name}: vsmlrt is too old; need >= {min_ver}")
    return vsmlrt


def _shift_clip_edge(clip: vs.VideoNode, offset: int) -> vs.VideoNode:
    if offset == 0:
        return clip
    n = clip.num_frames
    if n <= 1:
        return clip
    if offset < 0:
        k = min(-offset, n - 1)
        head = core.std.Trim(clip, 0, 0) * k
        body = core.std.Trim(clip, 0, n - k - 1)
        return head + body
    k = min(offset, n - 1)
    body = core.std.Trim(clip, k, n - 1)
    tail = core.std.Trim(clip, n - 1, n - 1) * k
    return body + tail


def _split_rgb_planes(clip: vs.VideoNode) -> typing.List[vs.VideoNode]:
    return [
        core.std.ShufflePlanes(clip, 0, vs.GRAY),
        core.std.ShufflePlanes(clip, 1, vs.GRAY),
        core.std.ShufflePlanes(clip, 2, vs.GRAY),
    ]


def _validate_common(func_name: str, input, clamp, model_pth, fp16_qnt, gpu, gpu_t, vs_t):
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


def _input_precision_to_bool(elem_type: int, func_name: str) -> bool:
    if elem_type == 1:
        return False
    if elem_type == 10:
        return True
    raise vs.Error(f"{func_name}: unsupported model input precision ({elem_type})")


def _prepare_rgb(input: vs.VideoNode, fp16_qnt: bool, clamp: bool) -> vs.VideoNode:
    clip = core.resize.Bilinear(
        clip=input,
        format=vs.RGBH if fp16_qnt else vs.RGBS,
        matrix_in_s="709",
    )
    if clamp:
        clip = core.akarin.Expr(clips=clip, expr="x 0 1 clamp")
    return clip


def _make_dml_backend(_vsmlrt, gpu: int, gpu_t: int, fp16_qnt: bool):
    return _vsmlrt.BackendV2.ORT_DML(
        device_id=gpu,
        num_streams=gpu_t,
        fp16=fp16_qnt,
    )


def _make_trt_backend(
    _vsmlrt,
    gpu: int,
    gpu_t: int,
    fp16_qnt: bool,
    opt_lv: int,
    cuda_opt: typing.List[int],
    int8_qnt: bool,
    st_eng: bool,
    res_opt: typing.Optional[typing.List[int]],
    res_max: typing.Optional[typing.List[int]],
    ws_size: int,
):
    if opt_lv not in [0, 1, 2, 3, 4, 5]:
        raise vs.Error("TRT: invalid opt_lv")
    if not (isinstance(cuda_opt, list) and len(cuda_opt) == 3 and all(isinstance(num, int) and num in [0, 1] for num in cuda_opt)):
        raise vs.Error("TRT: invalid cuda_opt")
    if not isinstance(int8_qnt, bool):
        raise vs.Error("TRT: invalid int8_qnt")
    if not isinstance(st_eng, bool):
        raise vs.Error("TRT: invalid st_eng")
    if not st_eng:
        if not (isinstance(res_opt, list) and len(res_opt) == 2 and all(isinstance(i, int) for i in res_opt)):
            raise vs.Error("TRT: invalid res_opt")
        if not (isinstance(res_max, list) and len(res_max) == 2 and all(isinstance(i, int) for i in res_max)):
            raise vs.Error("TRT: invalid res_max")
    if not isinstance(ws_size, int) or ws_size < 0:
        raise vs.Error("TRT: invalid ws_size")

    nv1, nv2, nv3 = [bool(num) for num in cuda_opt]
    if int8_qnt:
        fp16_qnt = True

    return _vsmlrt.BackendV2.TRT(
        builder_optimization_level=opt_lv,
        short_path=True,
        device_id=gpu,
        num_streams=gpu_t,
        use_cuda_graph=nv1,
        use_cublas=nv2,
        use_cudnn=nv3,
        int8=int8_qnt,
        fp16=fp16_qnt,
        tf32=False if fp16_qnt else True,
        output_format=1 if fp16_qnt else 0,
        workspace=None if ws_size < 128 else (ws_size if st_eng else ws_size * 2),
        static_shape=st_eng,
        min_shapes=[0, 0] if st_eng else [384, 384],
        opt_shapes=None if st_eng else res_opt,
        max_shapes=None if st_eng else res_max,
    )


def _restore_output(infer: vs.VideoNode, fmt_in: int, colorlv: int) -> vs.VideoNode:
    return core.resize.Bilinear(
        clip=infer,
        format=fmt_in,
        matrix_s="709",
        range=1 if colorlv == 0 else None,
    )


def UAI_DML(
    input: vs.VideoNode,
    clamp: bool = False,
    model_pth: str = "",
    fp16_qnt: bool = True,
    gpu: int = 0,
    gpu_t: int = 2,
    vs_t: int = vs_thd_dft,
    log: bool = True,
) -> vs.VideoNode:
    func_name = "UAI_DML"
    _validate_common(func_name, input, clamp, model_pth, fp16_qnt, gpu, gpu_t, vs_t)
    _require_ort_plugin(func_name)
    if clamp:
        _require_akarin(func_name)

    mdl_pth = resolve_model_path_ort(model_pth)
    model_info = ONNX_INFO(mdl_pth)
    _log(f"{func_name}: model={mdl_pth}", log)
    _log(f"{func_name}: input_name={model_info['input_name']} dims={model_info['dims']} elem_type={model_info['elem_type']}", log)

    if model_info["rank"] >= 5 or (model_info["rank"] == 4 and isinstance(model_info["dims"][1], int) and model_info["dims"][1] > 3):
        raise vs.Error(f"{func_name}: temporal/VSR model detected; use VSR_DML() instead")

    _vsmlrt = _load_vsmlrt(func_name, need_trt=False)

    core.num_threads = vs_t
    fmt_in = input.format.id
    colorlv = getattr(input.get_frame(0).props, "_ColorRange", 0)
    fp16_mdl = _input_precision_to_bool(model_info["elem_type"], func_name)
    if fp16_mdl:
        fp16_qnt = True

    clip = _prepare_rgb(input, fp16_qnt, clamp)
    be_param = _make_dml_backend(_vsmlrt, gpu, gpu_t, fp16_qnt)
    _log(f"{func_name}: backend=ORT_DML gpu={gpu} streams={gpu_t} vs_threads={vs_t}", log)

    infer = _vsmlrt.inference(clips=clip, network_path=mdl_pth, backend=be_param)
    _log(f"{func_name}: inference submitted", log)
    output = _restore_output(infer, fmt_in, colorlv)
    _log(f"{func_name}: output={output.width}x{output.height}", log)
    return output


def UAI_TRT(
    input: vs.VideoNode,
    clamp: bool = False,
    model_pth: str = "",
    opt_lv: int = 3,
    cuda_opt: typing.List[int] = [0, 0, 0],
    int8_qnt: bool = False,
    fp16_qnt: bool = True,
    gpu: int = 0,
    gpu_t: int = 2,
    st_eng: bool = False,
    res_opt: typing.Optional[typing.List[int]] = None,
    res_max: typing.Optional[typing.List[int]] = None,
    ws_size: int = 0,
    vs_t: int = vs_thd_dft,
    log: bool = True,
) -> vs.VideoNode:
    func_name = "UAI_TRT"
    _validate_common(func_name, input, clamp, model_pth, fp16_qnt, gpu, gpu_t, vs_t)
    _require_trt_plugin(func_name)
    if clamp:
        _require_akarin(func_name)

    mdl_pth = resolve_model_path_trt(model_pth)
    model_info = ONNX_INFO(mdl_pth)
    _log(f"{func_name}: ENTER", log)
    _log(f"{func_name}: model={mdl_pth}", log)
    _log(f"{func_name}: input_name={model_info['input_name']} dims={model_info['dims']} elem_type={model_info['elem_type']}", log)

    if model_info["rank"] >= 5 or (model_info["rank"] == 4 and isinstance(model_info["dims"][1], int) and model_info["dims"][1] > 3):
        raise vs.Error(f"{func_name}: temporal/VSR model detected; use VSR_TRT() instead")

    _vsmlrt = _load_vsmlrt(func_name, need_trt=True)
    _log(f"{func_name}: vsmlrt={_vsmlrt.__version__}", log)
    _log(f"{func_name}: core.trt present=yes", log)

    core.num_threads = vs_t
    fmt_in = input.format.id
    colorlv = getattr(input.get_frame(0).props, "_ColorRange", 0)
    fp16_mdl = _input_precision_to_bool(model_info["elem_type"], func_name)
    if fp16_mdl:
        fp16_qnt = True

    clip = _prepare_rgb(input, fp16_qnt, clamp)
    be_param = _make_trt_backend(_vsmlrt, gpu, gpu_t, fp16_qnt, opt_lv, cuda_opt, int8_qnt, st_eng, res_opt, res_max, ws_size)
    _log(f"{func_name}: backend=TRT gpu={gpu} streams={gpu_t} static_shape={st_eng} res_opt={res_opt} res_max={res_max}", log)

    infer = _vsmlrt.inference(clips=clip, network_path=mdl_pth, backend=be_param)
    _log(f"{func_name}: inference submitted", log)
    output = _restore_output(infer, fmt_in, colorlv)
    _log(f"{func_name}: output={output.width}x{output.height}", log)
    return output


def _vsr_impl(
    func_name: str,
    backend: str,
    input: vs.VideoNode,
    clamp: bool,
    model_pth: str,
    fp16_qnt: bool,
    gpu: int,
    gpu_t: int,
    vs_t: int,
    log: bool,
    opt_lv: int = 3,
    cuda_opt: typing.List[int] = [0, 0, 0],
    int8_qnt: bool = False,
    st_eng: bool = False,
    res_opt: typing.Optional[typing.List[int]] = None,
    res_max: typing.Optional[typing.List[int]] = None,
    ws_size: int = 0,
) -> vs.VideoNode:
    _validate_common(func_name, input, clamp, model_pth, fp16_qnt, gpu, gpu_t, vs_t)
    if backend == "dml":
        _require_ort_plugin(func_name)
        need_trt = False
        mdl_pth = resolve_model_path_ort(model_pth)
    else:
        _require_trt_plugin(func_name)
        need_trt = True
        mdl_pth = resolve_model_path_trt(model_pth)
    if clamp:
        _require_akarin(func_name)

    model_info = ONNX_INFO(mdl_pth)
    _log(f"{func_name}: ENTER backend={backend}", log)
    _log(f"{func_name}: model={mdl_pth}", log)
    _log(f"{func_name}: input_name={model_info['input_name']} dims={model_info['dims']} elem_type={model_info['elem_type']}", log)

    dims = model_info["dims"]
    if model_info["rank"] != 4 or not isinstance(dims[1], int) or dims[1] <= 3 or dims[1] % 3 != 0:
        raise vs.Error(f"{func_name}: unsupported temporal ONNX input shape {dims}; expected packed 4D with channels = frames*3")

    num_frames = dims[1] // 3
    radius = num_frames // 2
    _log(f"{func_name}: inferred temporal frames={num_frames} radius={radius}", log)

    _vsmlrt = _load_vsmlrt(func_name, need_trt=need_trt)
    core.num_threads = vs_t
    fmt_in = input.format.id
    colorlv = getattr(input.get_frame(0).props, "_ColorRange", 0)
    fp16_mdl = _input_precision_to_bool(model_info["elem_type"], func_name)
    if fp16_mdl:
        fp16_qnt = True

    clip = _prepare_rgb(input, fp16_qnt, clamp)
    _log(f"{func_name}: clip_in={input.width}x{input.height} frames={input.num_frames}", log)

    packed_inputs = []
    for off in range(-radius, radius + 1):
        shifted = _shift_clip_edge(clip, off)
        packed_inputs.extend(_split_rgb_planes(shifted))
    _log(f"{func_name}: packed_inputs={len(packed_inputs)} (expected={num_frames * 3})", log)

    if backend == "dml":
        be_param = _make_dml_backend(_vsmlrt, gpu, gpu_t, fp16_qnt)
        _log(f"{func_name}: backend=ORT_DML gpu={gpu} streams={gpu_t} vs_threads={vs_t}", log)
    else:
        be_param = _make_trt_backend(_vsmlrt, gpu, gpu_t, fp16_qnt, opt_lv, cuda_opt, int8_qnt, st_eng, res_opt, res_max, ws_size)
        _log(f"{func_name}: backend=TRT gpu={gpu} streams={gpu_t} static_shape={st_eng} res_opt={res_opt} res_max={res_max} vs_threads={vs_t}", log)

    infer = _vsmlrt.inference(clips=packed_inputs, network_path=mdl_pth, backend=be_param)
    _log(f"{func_name}: inference submitted", log)
    output = _restore_output(infer, fmt_in, colorlv)
    _log(f"{func_name}: output={output.width}x{output.height}", log)
    return output


def VSR_DML(
    input: vs.VideoNode,
    clamp: bool = False,
    model_pth: str = "",
    fp16_qnt: bool = True,
    gpu: int = 0,
    gpu_t: int = 2,
    vs_t: int = vs_thd_dft,
    log: bool = True,
) -> vs.VideoNode:
    return _vsr_impl("VSR_DML", "dml", input, clamp, model_pth, fp16_qnt, gpu, gpu_t, vs_t, log)


def VSR_TRT(
    input: vs.VideoNode,
    clamp: bool = False,
    model_pth: str = "",
    opt_lv: int = 3,
    cuda_opt: typing.List[int] = [0, 0, 0],
    int8_qnt: bool = False,
    fp16_qnt: bool = True,
    gpu: int = 0,
    gpu_t: int = 2,
    st_eng: bool = False,
    res_opt: typing.Optional[typing.List[int]] = None,
    res_max: typing.Optional[typing.List[int]] = None,
    ws_size: int = 0,
    vs_t: int = vs_thd_dft,
    log: bool = True,
) -> vs.VideoNode:
    return _vsr_impl("VSR_TRT", "trt", input, clamp, model_pth, fp16_qnt, gpu, gpu_t, vs_t, log,
                     opt_lv=opt_lv, cuda_opt=cuda_opt, int8_qnt=int8_qnt, st_eng=st_eng,
                     res_opt=res_opt, res_max=res_max, ws_size=ws_size)


def UAI(input: vs.VideoNode, backend: str = "dml", **kwargs) -> vs.VideoNode:
    backend = (backend or "dml").lower()
    if backend == "dml":
        return UAI_DML(input, **kwargs)
    if backend == "trt":
        return UAI_TRT(input, **kwargs)
    raise vs.Error(f"UAI: unsupported backend={backend}")


def VSR(input: vs.VideoNode, backend: str = "dml", **kwargs) -> vs.VideoNode:
    backend = (backend or "dml").lower()
    if backend == "dml":
        return VSR_DML(input, **kwargs)
    if backend == "trt":
        return VSR_TRT(input, **kwargs)
    raise vs.Error(f"VSR: unsupported backend={backend}")
