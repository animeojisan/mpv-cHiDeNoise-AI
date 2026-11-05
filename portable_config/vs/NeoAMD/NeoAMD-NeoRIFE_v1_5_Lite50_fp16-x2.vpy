### NeoRIFE_v1_5_Lite50_fp16 x2 / NeoAMD Backend Pack
from pathlib import Path
from datetime import datetime
from uuid import uuid4
from fractions import Fraction
import vapoursynth as vs
# Find the application root even when this VPY is in a backend subfolder.
from pathlib import Path as _NeoPath
import sys as _neo_sys
_neo_app_root = next((_p for _p in _NeoPath(__file__).resolve().parents
                      if (_p / "backends").is_dir() and (_p / "vs-plugins" / "models").is_dir()), None)
if _neo_app_root is None:
    raise RuntimeError("mpv application root was not found above this VPY")
for _neo_module_dir in reversed((_neo_app_root, _neo_app_root / "vs-scripts")):
    if str(_neo_module_dir) not in _neo_sys.path:
        _neo_sys.path.insert(0, str(_neo_module_dir))

import k7sfunc1 as k7f

core = vs.core
core.num_threads = 8

# ----- ユーザー設定 -----
Backend = 'NeoAMD'
Model = '55ai/NeoRIFE_v1_5_Lite50_fp16.onnx'
Gpu = 0
Factor = 2
SourceFPS = 0.0     # 0でmpvから自動取得。自動取得できない場合だけ元動画のfpsを指定。
SceneThreshold = 0.15  # シーン切替では補間を回避。-1で判定を無効化。
H_Pre = 1080
H_Max = 2160
Lk_Fmt = False
Matrix = "709"
Log = False
LogInterval = 60
LogWarmup = 30
# ------------------------

# このファイルをmpvの portable_config/vs に置いて使用してください。
# RIFE専用。TemporalFixやDRBAのモデルには流用しないでください。
ROOT = _neo_app_root
if Backend not in ("NeoRTX", "NeoAMD"):
    raise ValueError('Backend must be "NeoRTX" or "NeoAMD"')
if not isinstance(Factor, int) or not 2 <= Factor <= 8:
    raise ValueError("Factor must be an integer from 2 to 8")
model = Path(Model)
if not model.is_absolute():
    model = ROOT / "vs-plugins" / "models" / model
backend = ROOT / "backends" / Backend.lower()
if not model.is_file():
    raise FileNotFoundError(str(model))
if not (backend / "backend.json").is_file():
    raise FileNotFoundError("Backend Pack is missing: " + str(backend))
try:
    plugin = core.neogpuexp
except AttributeError:
    core.std.LoadPlugin(path=str(ROOT / "vs-plugins" / "vs-neogpu-exp.dll"))
    plugin = core.neogpuexp

clip = video_in
fps = SourceFPS or (float(clip.fps_num) / clip.fps_den if clip.fps_num and clip.fps_den else globals().get("container_fps", 0))
if not fps or fps <= 0:
    raise ValueError("Source FPS is unknown; set SourceFPS to the input video FPS")
rate = Fraction(str(fps)).limit_denominator(100000)
clip = core.std.AssumeFPS(clip, fpsnum=rate.numerator, fpsden=rate.denominator)
# mpvが与える仮想フレーム数も、補間後にVSの整数上限を超えないようにする。
if clip.num_frames > 2147483647 // Factor:
    clip = core.std.Trim(clip, last=2147483647 // Factor - 1)
clip = k7f.FMT_CTRL(clip, h_max=H_Pre, fmt_pix=1 if Lk_Fmt else 0, vs_t=core.num_threads)
original = clip
if clip.format.color_family == vs.YUV:
    rgb = core.resize.Bicubic(clip, format=vs.RGB24, matrix_in_s=Matrix,
                              dither_type="error_diffusion")
elif clip.format.color_family == vs.RGB:
    rgb = core.resize.Bicubic(clip, format=vs.RGB24, dither_type="error_diffusion")
else:
    raise ValueError("RIFE requires a YUV or RGB clip")

timing = {}
if Log:
    name = Backend + "-RIFE-SIMD-" + datetime.now().strftime("%Y%m%d-%H%M%S-%f") + "-" + uuid4().hex[:8] + ".csv"
    timing = dict(log_path=str(ROOT / "portable_config" / "logs" / name),
                  log_interval=LogInterval, log_warmup=LogWarmup)

clip = getattr(plugin, Backend + "Interpolate")(
    rgb, model=str(model), backend=str(backend), gpu=Gpu,
    factor=Factor, scene_threshold=SceneThreshold, mode=0, **timing)

if original.format.color_family == vs.RGB:
    clip = core.resize.Bicubic(clip, format=original.format.id, dither_type="error_diffusion")
else:
    limited = core.resize.Bicubic(clip, format=original.format.id, matrix_s=Matrix,
                                  range_s="limited", dither_type="error_diffusion")
    full = core.resize.Bicubic(clip, format=original.format.id, matrix_s=Matrix,
                               range_s="full", dither_type="error_diffusion")
    # FrameEvalの参照用クリップも補間後と同じフレーム数へ揃える。
    range_reference = core.std.Interleave([original] * Factor)
    def select_range(n, f):
        return full if f.props.get("_ColorRange", 1) == 0 else limited
    clip = core.std.FrameEval(limited, eval=select_range, prop_src=range_reference)

clip = k7f.FMT_CTRL(clip, h_max=H_Max, fmt_pix=1 if Lk_Fmt else 0, vs_t=core.num_threads)
clip.set_output()
