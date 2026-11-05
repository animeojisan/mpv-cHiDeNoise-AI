### cHiDeNoise_Anime_Compact_V_1 / NeoRTX Backend Pack
# Saved speed evidence is recorded in menu-selection.json.
from vapoursynth import core
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
import neogpu_mpv_fast as neo

core.num_threads = 8
H_Pre = 1080
H_Max = 1440
Lk_Fmt = False
Model = '55ai/cHiDeNoise_Anime_Compact_V_1.onnx'
Gpu = 0
Matrix = '709'
Log = False

clip = k7f.FMT_CTRL(video_in, h_max=H_Pre, fmt_pix=1 if Lk_Fmt else 0, vs_t=core.num_threads)
clip = neo.image(clip, backend='NeoRTX', model=Model, gpu=Gpu, matrix=Matrix, log=Log)
clip = k7f.FMT_CTRL(clip, h_max=H_Max, fmt_pix=1 if Lk_Fmt else 0, vs_t=core.num_threads)
clip.set_output()
