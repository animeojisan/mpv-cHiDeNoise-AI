### カスタムAIモデルでスケーリング（DX12グラフィックスカード用）
### ファイル_ https://github.com/hooke007/MPV_lazy/discussions/329
### パラメトリック_ https://github.com/hooke007/MPV_lazy/wiki/3_K7sfunc#uai_dml

import vapoursynth as vs
from vapoursynth import core
import k7sfunc1 as k7f

clip = video_in

##################
# ユーザーオプション#
##################

H_Pre = 1080
Model = "55ai/W2xEX_AnimeVideo_MiniV1d5_x2_fp16_op17.onnx"
Fp16_Qnt = True
Gpu = 0
Gpu_T = 2
H_Max = 2160
Lk_Fmt = False

## 整数：処理前の高さ制限
## 使用するモデルを指定する/mpv-lazy/vs-plugins/models/55ai/です
## <True|False> fp32モデルに対してfp16量子化を使用するかどうか
## 使用するグラフィックカードのシリアル番号
## <1|2|3> 使用するグラフィックカードのスレッド数
## 整数：出力の高さ制限 (モニターの高さを記入)
## <True|False> ピクセルフォーマットを yuv420p8 に固定するかどうか

ret = k7f.FMT_CTRL(clip, h_max=1200, h_ret=True)
clip = k7f.FMT_CTRL(clip, h_max=H_Pre, fmt_pix=1 if Lk_Fmt else 0)
clip = k7f.UAI_DML(clip, clamp=False, model_pth=Model, fp16_qnt=Fp16_Qnt, gpu=Gpu, gpu_t=Gpu_T)
clip = k7f.FMT_CTRL(clip, h_max=H_Max, fmt_pix=1 if Lk_Fmt else 0)

clip.set_output()