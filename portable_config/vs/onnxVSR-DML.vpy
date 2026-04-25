### カスタムAIモデルでスケーリング（DX12グラフィックスカード用）
### ファイル_ https://github.com/hooke007/MPV_lazy/discussions/329
### パラメトリック_ https://github.com/hooke007/MPV_lazy/wiki/3_K7sfunc#uai_dml
### TSPAN / packed temporal ONNX 用

import vapoursynth as vs
from vapoursynth import core
import k7sfunc1 as k7f
import onnxVSR as onnxVSR

clip = video_in

##################
# ユーザーオプション#
##################

H_Pre = 1080
Model = "55ai/2x-AniRemaster_TSPAN_fp16.onnx"
Fp16_Qnt = True
Gpu = 0
Gpu_T = 2
H_Max = 1440
Lk_Fmt = False
Pad_Mode = "reflect"
Pad_Multiple = 64
Crop_Output = True


## 整数：処理前の高さ制限
## 使用するモデルを指定する/mpv-lazy/vs-plugins/models/55ai/です
## <True|False> fp32モデルに対してfp16量子化を使用するかどうか
## 使用するグラフィックカードのシリアル番号
## <1|2|3> 使用するグラフィックカードのスレッド数
## 整数：出力の高さ制限 (モニターの高さを記入)
## <True|False> ピクセルフォーマットを yuv420p8 に固定するかどうか
## <"none"|"reflect"> VSR入力の周囲へ付ける余白モード
## 整数：高さ/幅をこの倍数へ揃える (0 で無効)
## <True|False> padding後の推論結果を中央cropして元の有効範囲へ戻す


ret = k7f.FMT_CTRL(clip, h_max=1200, h_ret=True)
clip = k7f.FMT_CTRL(clip, h_max=H_Pre, fmt_pix=1 if Lk_Fmt else 0)

clip = onnxVSR.VSR_DML(
    clip,
    clamp=False,
    model_pth=Model,
    fp16_qnt=Fp16_Qnt,
    gpu=Gpu,
    gpu_t=Gpu_T,
    pad_mode=Pad_Mode,
    pad_multiple=Pad_Multiple,
    crop_output=Crop_Output,
)

clip = k7f.FMT_CTRL(clip, h_max=H_Max, fmt_pix=1 if Lk_Fmt else 0)
clip.set_output()

