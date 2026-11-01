"""Shared NeoAMD interpolation adapter for existing mpv filter settings."""
from pathlib import Path
from fractions import Fraction
import vapoursynth as vs

ROOT = Path(__file__).resolve().parent


def interpolate(clip, *, model, fps_num=2, fps_den=1, fps_in=0,
                sc_mode=0, gpu=0, gpu_t=2, matrix="709", backend=None):
    if not isinstance(fps_num, int) or not isinstance(fps_den, int):
        raise ValueError("Interpolation rate must use integer numerator/denominator")
    if fps_den <= 0 or fps_num <= fps_den:
        raise ValueError("Interpolation rate must exceed 1")
    factor = Fraction(fps_num, fps_den)
    numerator, denominator = factor.numerator, factor.denominator
    if not 2 <= numerator <= 8:
        raise ValueError("NeoAMD interpolation numerator must be 2..8")
    if clip.format is None or clip.format.color_family not in (vs.RGB, vs.YUV):
        raise ValueError("Interpolation needs a constant RGB/YUV format")
    if sc_mode not in (0, 1, 2) or not isinstance(gpu_t, int) or gpu_t < 1:
        raise ValueError("Invalid scene detection or stream count")
    source_rate = Fraction(str(fps_in)).limit_denominator(100000) if fps_in else Fraction(clip.fps_num, clip.fps_den)
    if source_rate <= 0:
        raise ValueError("Source FPS is unknown")
    if clip.num_frames > 2147483647 // numerator:
        raise ValueError("Interpolation frame count exceeds the supported limit")
    core = vs.core
    source = core.std.AssumeFPS(clip, fpsnum=source_rate.numerator, fpsden=source_rate.denominator)
    marked = source
    if sc_mode == 1:
        marked = core.misc.SCDetect(marked, threshold=0.15)
    elif sc_mode == 2:
        sup = core.mv.Super(marked, pel=1)
        vectors = core.mv.Analyse(sup, isb=True)
        marked = core.mv.SCDetection(marked, vectors=vectors, thscd1=240, thscd2=130)
    original = source.format
    rgb = core.resize.Bilinear(marked, format=vs.RGB24,
                               **({"matrix_in_s": matrix} if original.color_family == vs.YUV else {}))
    try:
        plugin = core.neogpuexp
    except AttributeError:
        core.std.LoadPlugin(path=str(ROOT / "vs-plugins/vs-neogpu-exp.dll"))
        plugin = core.neogpuexp
    path = Path(model)
    if not path.is_absolute():
        path = ROOT / "vs-plugins/models" / path
    result = plugin.NeoAMDInterpolate(rgb, model=str(path), backend=str(backend or ROOT / "backends/neoamd"),
                                      gpu=gpu, factor=numerator, interp_streams=min(2, gpu_t),
                                      scene_threshold=-1, mode=0)
    reference = core.std.Interleave([marked] * numerator)
    if original.color_family == vs.RGB:
        result = core.resize.Bilinear(result, format=original.id)
    else:
        limited = core.resize.Bilinear(result, format=original.id, matrix_s=matrix, range_s="limited")
        full = core.resize.Bilinear(result, format=original.id, matrix_s=matrix, range_s="full")
        result = core.std.FrameEval(limited, eval=lambda n, f: full if f.props.get("_ColorRange", 1) == 0 else limited,
                                    prop_src=reference)
    # Scene cuts and the last source frame must bypass the RGB round trip too.
    phase_rate = source_rate * numerator
    reference = core.std.SetFrameProps(reference, _DurationNum=phase_rate.denominator,
                                       _DurationDen=phase_rate.numerator)
    converted = result
    result = core.std.FrameEval(converted,
        eval=lambda n, f: reference if (n % numerator == 0 or n // numerator == source.num_frames - 1
                                       or f.props.get("_SceneChangeNext", 0) == 1) else converted,
        prop_src=reference)
    if denominator != 1:
        result = core.std.SelectEvery(result, cycle=denominator, offsets=[0])
    target = source_rate * factor
    result = core.std.AssumeFPS(result, fpsnum=target.numerator, fpsden=target.denominator)
    return core.std.SetFrameProps(result, _DurationNum=target.denominator, _DurationDen=target.numerator,
                                  _NeoAMDInterpolationBackend="NeoAMD")


def rife(clip, *, fallback, model=4251, turbo=True, **kwargs):
    """Keep unsupported model/setting combinations on the existing implementation."""
    if model not in (4251, 426) or not isinstance(turbo, bool):
        return fallback(clip, model=model, turbo=turbo, **kwargs)
    native_kwargs = {k: v for k, v in kwargs.items() if k != "vs_t"}
    try:
        if "vs_t" in kwargs:
            vs.core.num_threads = kwargs["vs_t"]
        network = ("rife_v2/" if turbo else "rife/") + ("rife_v4.25_lite.onnx" if model == 4251 else "rife_v4.26.onnx")
        return interpolate(clip, model=network, **native_kwargs)
    except (vs.Error, ValueError, OSError):
        return fallback(clip, model=model, turbo=turbo, **kwargs)
