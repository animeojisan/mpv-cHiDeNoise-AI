"""Portable mpv/VapourSynth helpers. Image output uses ordered dithering to avoid serial error diffusion."""
from pathlib import Path
from datetime import datetime
from uuid import uuid4
import vapoursynth as vs

ROOT = Path(__file__).resolve().parent


def _process(clip, *, backend="NeoRTX", model, gpu=0, matrix="709", mode=0, log=False, log_interval=60, log_warmup=30, temporal=False, output_dither=None, output_resizer="bicubic"):
    """Run a single-frame image model; return the original pixel format.

    model: path relative to vs-plugins/models, or an absolute ONNX path.
    gpu: zero-based ordinal among DXGI adapters of the selected vendor.
    matrix: YUV/RGB conversion fallback, matching the existing AI scripts.
    """
    if backend not in ("NeoRTX", "NeoAMD"):
        raise ValueError("backend must be NeoRTX or NeoAMD")
    if clip.format is None or clip.width <= 0 or clip.height <= 0:
        raise ValueError("Neo requires a constant pixel format and frame size")
    core = vs.core
    try:
        plugin = core.neogpuexp
    except AttributeError:
        core.std.LoadPlugin(path=str(ROOT / "vs-plugins" / "vs-neogpu-exp.dll"))
        plugin = core.neogpuexp
    model_path = Path(model)
    if not model_path.is_absolute():
        model_path = ROOT / "vs-plugins" / "models" / model_path
    backend_path = ROOT / "backends" / backend.lower()
    if not model_path.is_file():
        raise FileNotFoundError("Neo ONNX model is missing from vs-plugins/models")
    if not (backend_path / "backend.json").is_file():
        raise FileNotFoundError("Extract the complete Neo Backend Pack into backends/" + backend.lower())
    original = clip.format
    if output_dither is None:
        output_dither = "ordered" if not temporal else "error_diffusion"
    if output_dither not in ("none", "ordered", "random", "error_diffusion"):
        raise ValueError("Unsupported output_dither")
    if output_resizer not in ("bicubic", "bilinear"):
        raise ValueError("Unsupported output_resizer")
    if original.color_family == vs.RGB:
        rgb = core.resize.Bicubic(clip, format=vs.RGB24, dither_type="error_diffusion")
    elif original.color_family == vs.YUV:
        rgb = core.resize.Bicubic(clip, format=vs.RGB24, matrix_in_s=matrix,
                                 dither_type="error_diffusion")
    else:
        raise ValueError("Neo mpv helper supports YUV and RGB clips")
    timing = {}
    if log:
        stamp = datetime.now().strftime("%Y%m%d-%H%M%S-%f")
        route = ("Temporal-" if temporal else "") + ("Shared" if mode else "SIMD")
        path = ROOT / "portable_config" / "logs" / (backend + "-" + route + "-" + stamp + "-" + uuid4().hex[:8] + ".csv")
        timing = dict(log_path=str(path), log_interval=log_interval, log_warmup=log_warmup)
    result = getattr(plugin, backend + ("Temporal" if temporal else ""))(rgb, model=str(model_path), backend=str(backend_path), gpu=gpu, mode=mode, **timing)
    resize_out = core.resize.Bilinear if output_resizer == "bilinear" else core.resize.Bicubic
    if original.color_family == vs.RGB:
        return resize_out(result, format=original.id, dither_type=output_dither)
    # Preserve YUV full/limited range without pulling a frame during graph creation.
    limited = resize_out(result, format=original.id, matrix_s=matrix,
                         range_s="limited", dither_type=output_dither)
    full = resize_out(result, format=original.id, matrix_s=matrix,
                      range_s="full", dither_type=output_dither)
    def select_range(n, f):
        return full if f.props.get("_ColorRange", 1) == 0 else limited
    return core.std.FrameEval(limited, eval=select_range, prop_src=clip)


def image(clip, *, backend="NeoRTX", model, gpu=0, matrix="709", mode=0, log=False, log_interval=60, log_warmup=30, output_dither=None, output_resizer="bicubic"):
    """Run a single-frame image model."""
    return _process(clip, backend=backend, model=model, gpu=gpu, matrix=matrix, mode=mode, log=log, log_interval=log_interval, log_warmup=log_warmup, output_dither=output_dither, output_resizer=output_resizer)


def temporal(clip, *, backend="NeoRTX", model, gpu=0, matrix="709", mode=0, log=False, log_interval=60, log_warmup=30, output_dither=None):
    """Run TemporalFix with a centered seven-frame window; preserve FPS and size."""
    return _process(clip, backend=backend, model=model, gpu=gpu, matrix=matrix, mode=mode, log=log, log_interval=log_interval, log_warmup=log_warmup, temporal=True, output_dither=output_dither)
