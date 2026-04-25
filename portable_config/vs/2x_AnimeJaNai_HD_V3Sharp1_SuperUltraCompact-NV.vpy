### カスタムAIモデルでスケーリング（Nvidia RTXグラフィックスカード用）
### ファイル_ https://github.com/hooke007/MPV_lazy/discussions/329
### パラメトリック_ https://github.com/hooke007/MPV_lazy/wiki/3_K7sfunc#uai_nv_trt

import vapoursynth as vs
from vapoursynth import core
import k7sfunc1 as k7f

clip = video_in

###################
# ユーザーオプション#
###################

H_Pre = 1080
Model = "55ai/2x_AnimeJaNai_HD_V3Sharp1_SuperUltraCompact_25k-fp16.onnx"
Fp16_Qnt = True
Gpu = 0
Gpu_T = 2
St_Eng = False
Res_Opt = [1280, 720]
Res_Max = [1920, 1080]
Ws_Size = 0
H_Max = 2160
Lk_Fmt = False


## 整数：処理前の高さ制限
## 使用するモデルを指定する/mpv-lazy/vs-plugins/models/55ai/です
## <True|False> fp32モデルに対してfp16量子化を使用するかどうか
## 使用するグラフィックカードのシリアル番号
##<1|2|3> 使用するカードのスレッド数
## <True|False> 静的エンジンを使うかどうか, そうでなければ動的エンジンを使うかどうか
## 動的エンジン用に最適化されたソース解像度 (高さは H_Pre 以内)
## 動的エンジンでサポートされるソース解像度の最大値
##<0~1024> 整数：ディスプレイメモリ(MiB)の制約, 最小値は128, この数値より小さい値を設定すると制限なし
##<0~1024> integer, 出力高さの制限 (モニターの高さを記入) ## <True||1024> integer, 出力高さの制限 (モニターの高さを記入)
## <True|False> ピクセルフォーマットを yuv420p8 に固定するかどうか

ret = k7f.FMT_CTRL(clip, h_max=1200, h_ret=True)
clip = k7f.FMT_CTRL(clip, h_max=H_Pre, fmt_pix=1 if Lk_Fmt else 0)
clip = k7f.UAI_NV_TRT(clip, clamp=False, model_pth=Model, opt_lv=3, cuda_opt=[1, 1, 1], int8_qnt=False, fp16_qnt=Fp16_Qnt, gpu=Gpu, gpu_t=Gpu_T, st_eng=St_Eng, res_opt=Res_Opt, res_max=Res_Max, ws_size=Ws_Size)
clip = k7f.FMT_CTRL(clip, h_max=H_Max, fmt_pix=1 if Lk_Fmt else 0)

clip.set_output()
