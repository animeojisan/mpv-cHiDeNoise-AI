"""Portable mpv/VapourSynth helpers for the unchanged Neo Backend Packs."""
from pathlib import Path
import vapoursynth as vs

ROOT = Path(__file__).resolve().parent


def image(clip, *, backend="NeoRTX", model, gpu=0, matrix="709", output_dither=None):
    """Run a single-frame image model; return the original pixel format.

    model: path relative to vs-plugins/models, or an absolute ONNX path.
    gpu: zero-based ordinal among DXGI adapters of the selected vendor.
    matrix: YUV/RGB conversion fallback, matching the existing AI scripts.
    """
    if backend not in ("NeoRTX", "NeoAMD"):
        raise ValueError("backend must be NeoRTX or NeoAMD")
    if clip.format is None or clip.width <= 0 or clip.height <= 0:
        raise ValueError("Neo requires a constant pixel format and frame size")
    if backend in ("NeoRTX", "NeoAMD") and (ROOT / "vs-plugins" / "vs-neogpu-exp.dll").is_file():
        import neogpu_mpv_fast as fast
        fast.ROOT = ROOT
        return fast.image(clip, backend=backend, model=model, gpu=gpu, matrix=matrix, output_dither=output_dither)
    core = vs.core
    try:
        plugin = core.neogpu
    except AttributeError:
        core.std.LoadPlugin(path=str(ROOT / "vs-plugins" / "vs-neogpu.dll"))
        plugin = core.neogpu
    model_path = Path(model)
    if not model_path.is_absolute():
        model_path = ROOT / "vs-plugins" / "models" / model_path
    backend_path = ROOT / "backends" / backend.lower()
    if not model_path.is_file():
        raise FileNotFoundError("Neo ONNX model is missing from vs-plugins/models")
    if not (backend_path / "backend.json").is_file():
        raise FileNotFoundError("Extract the complete Neo Backend Pack into backends/" + backend.lower())
    original = clip.format
    if original.color_family == vs.RGB:
        rgb = core.resize.Bicubic(clip, format=vs.RGB24, dither_type="error_diffusion")
    elif original.color_family == vs.YUV:
        rgb = core.resize.Bicubic(clip, format=vs.RGB24, matrix_in_s=matrix,
                                 dither_type="error_diffusion")
    else:
        raise ValueError("Neo mpv helper supports YUV and RGB clips")
    result = getattr(plugin, backend)(rgb, model=str(model_path), backend=str(backend_path), gpu=gpu)
    if original.color_family == vs.RGB:
        return core.resize.Bicubic(result, format=original.id, dither_type="error_diffusion")
    # Preserve YUV full/limited range without pulling a frame during graph creation.
    limited = core.resize.Bicubic(result, format=original.id, matrix_s=matrix,
                                  range_s="limited", dither_type="error_diffusion")
    full = core.resize.Bicubic(result, format=original.id, matrix_s=matrix,
                               range_s="full", dither_type="error_diffusion")
    def select_range(n, f):
        return full if f.props.get("_ColorRange", 1) == 0 else limited
    return core.std.FrameEval(limited, eval=select_range, prop_src=clip)
