### 2x_AnimeJaNai_SD_V1beta34_Compact_93k-fp16 / NeoRTX Backend Pack
import vapoursynth as vs
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
import neogpu_mpv as neo

clip = video_in

###################
# ユーザーオプション
###################
H_Pre = 1080
Model = "55ai/2x_AnimeJaNai_SD_V1beta34_Compact_93k-fp16.onnx"
Gpu = 0
H_Max = 2160
Lk_Fmt = False
Matrix = "709"

# Gpu: NVIDIA製GPU内の番号（0から）。NeoAMD用ではAMD製GPU内の番号。
# Model: vs-plugins/models 内のONNX。既存Packの同梱重みに対応するFP16版を使用。
# H_Pre / H_Max / Lk_Fmt: 既存vpyと同じ前処理・出力高さ制限。
# Matrix: 既存AI vpyと同じ709。必要に応じて470bg / 170mなどへ変更。
# この版は画像モデル用。RIFE / DRBA / TemporalFixには使用しないでください。

clip = k7f.FMT_CTRL(clip, h_max=H_Pre, fmt_pix=1 if Lk_Fmt else 0)
clip = neo.image(clip, backend="NeoRTX", model=Model, gpu=Gpu, matrix=Matrix)
clip = k7f.FMT_CTRL(clip, h_max=H_Max, fmt_pix=1 if Lk_Fmt else 0)
clip.set_output()
