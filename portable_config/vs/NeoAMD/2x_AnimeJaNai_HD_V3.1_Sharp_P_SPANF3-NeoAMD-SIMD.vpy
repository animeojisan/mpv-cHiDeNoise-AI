### 2x_AnimeJaNai_HD_V3.1Sharp1_Performance_SPANF3_fp16 / NeoAMD Backend Pack
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
import neogpu_mpv_fast as neo

clip = video_in

###################
# ユーザーオプション
###################
H_Pre = 1080
Model = "55ai/2x_AnimeJaNai_HD_V3.1Sharp1_Performance_SPANF3_fp16.onnx"
Gpu = 0
H_Max = 2160
Lk_Fmt = False
Matrix = "709"

# 専用ログ（portable_config/logs/*.csv）
# Trueで記録、Falseで計測・ファイル出力とも停止。
Log = False
LogInterval = 60  # この処理要求数ごとの平均ms・最小/最大を出力。1なら毎要求。
LogWarmup = 30    # 初回の処理要求を平均から除外。0で全要求を記録。

# Gpu: NVIDIA製GPU内の番号（0から）。NeoAMD用ではAMD製GPU内の番号。
# Model: vs-plugins/models 内のONNX。既存Packの同梱重みに対応するFP16版を使用。
# H_Pre / H_Max / Lk_Fmt: 既存vpyと同じ前処理・出力高さ制限。
# Matrix: 既存AI vpyと同じ709。必要に応じて470bg / 170mなどへ変更。
# この版は画像モデル用。RIFE / DRBA / TemporalFixには使用しないでください。

clip = k7f.FMT_CTRL(clip, h_max=H_Pre, fmt_pix=1 if Lk_Fmt else 0)
clip = neo.image(clip, backend="NeoAMD", model=Model, gpu=Gpu, matrix=Matrix, mode=0, log=Log, log_interval=LogInterval, log_warmup=LogWarmup)
clip = k7f.FMT_CTRL(clip, h_max=H_Max, fmt_pix=1 if Lk_Fmt else 0)
clip.set_output()
