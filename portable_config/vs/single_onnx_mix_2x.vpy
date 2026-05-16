### single_onnx_mix_2x.vpy
###
### mpv-cHiDeNoise-AI / k7sfunc1 UAI_DML 用
### オリジナル2x拡大映像に、指定したONNXのAIアプコン結果をミックスします。

import vapoursynth as vs
from vapoursynth import core
import k7sfunc1 as k7f

clip = video_in

##################
# ユーザー設定
##################

H_Pre = 1080
H_Max = 2160
Lk_Fmt = False

# 使用するONNXモデル。別モデルを使う場合は、この1行だけ差し替えてください。
ENGINE_MODEL = "55ai/2x_AnimeJaNai_SD_V1beta34_Compact_93k-fp16.onnx"

# AI側の混合量
# 0.00 = 通常2x拡大のみ / 1.00 = AI結果のみ
ENGINE_BLEND = 0.55

Fp16_Qnt = True
Gpu = 0
Gpu_T = 2

# DVDの表示比率補正。通常は keep のままでOK。
# 必要な場合だけ ntsc_4_3 / ntsc_16_9 / pal_4_3 / pal_16_9 を指定。
SOURCE_ASPECT_MODE = "keep"

# 動作確認用。通常は off。
# simple2x / engine_only / blend / half / side_by_side / off
TEST_MODE = "off"

SHOW_COMPARE_LABELS = True
SPLIT_LINE_WIDTH = 10
SHOW_TEXT = True

##################
# 内部処理
##################
def _even(x):
    return int(x) & ~1


def _clamp01(v):
    return max(0.0, min(1.0, float(v)))


def _text(c, label, alignment=7):
    try:
        return core.text.Text(c, label, alignment=alignment)
    except Exception:
        try:
            return core.text.Text(c, label)
        except Exception:
            return c


def mark(c, label):
    if not SHOW_TEXT:
        return c
    return _text(c, "SINGLE ENGINE BLEND: " + label, alignment=7)


def label_clip(c, label, alignment=7):
    if not SHOW_COMPARE_LABELS:
        return c
    return _text(c, label, alignment=alignment)


def _format_max_mid(c):
    # BlankClip用の白/黒/中性色。YUVではUVを中立にします。
    if c.format is None:
        return 1.0, 0.5
    if c.format.sample_type == vs.FLOAT:
        return 1.0, 0.5
    maxv = (1 << c.format.bits_per_sample) - 1
    midv = 1 << (c.format.bits_per_sample - 1)
    return maxv, midv


def _blank_color(c, kind="white", width=8):
    maxv, midv = _format_max_mid(c)
    if c.format is not None and c.format.color_family == vs.YUV and c.format.num_planes >= 3:
        if kind == "black":
            color = [0, midv, midv]
        else:
            color = [maxv, midv, midv]
    else:
        v = 0 if kind == "black" else maxv
        planes = c.format.num_planes if c.format is not None else 3
        color = [v] * planes
    return core.std.BlankClip(c, width=int(width), height=c.height, color=color)


def separator(c):
    # 黒細線 + 白太線 + 黒細線。境界が一目で分かるようにします。
    w = max(4, int(SPLIT_LINE_WIDTH))
    side = 2
    mid = max(2, w - side * 2)
    return core.std.StackHorizontal([
        _blank_color(c, "black", side),
        _blank_color(c, "white", mid),
        _blank_color(c, "black", side),
    ])


def apply_source_aspect(c):
    mode = SOURCE_ASPECT_MODE.lower().strip()
    if mode in ("", "keep", "none", "no"):
        return c

    if mode == "ntsc_4_3":
        w, h = 640, 480
    elif mode == "ntsc_16_9":
        w, h = 854, 480
    elif mode == "pal_4_3":
        w, h = 768, 576
    elif mode == "pal_16_9":
        w, h = 1024, 576
    else:
        raise ValueError("Unknown SOURCE_ASPECT_MODE: " + SOURCE_ASPECT_MODE)

    return core.resize.Spline36(c, width=w, height=h)


def fmt_pre(c):
    return k7f.FMT_CTRL(c, h_max=H_Pre, fmt_pix=1 if Lk_Fmt else 0)


def fmt_post(c):
    return k7f.FMT_CTRL(c, h_max=H_Max, fmt_pix=1 if Lk_Fmt else 0)


def simple_2x(c):
    return core.resize.Spline36(c, width=_even(c.width * 2), height=_even(c.height * 2))


def resize_like(c, ref):
    if c.width == ref.width and c.height == ref.height:
        return c
    return core.resize.Spline36(c, width=ref.width, height=ref.height)


def run_uai_2x(c, model_pth):
    # 単体確認版と同じ呼び方を維持
    return k7f.UAI_DML(
        c,
        clamp=False,
        model_pth=model_pth,
        fp16_qnt=Fp16_Qnt,
        gpu=Gpu,
        gpu_t=Gpu_T,
    )


def merge_simple(base, other, amount):
    other = resize_like(other, base)
    return core.std.Merge(base, other, weight=_clamp01(amount))


def build_engine(pre, simple):
    engine = run_uai_2x(pre, ENGINE_MODEL)
    return resize_like(engine, simple)


def half_compare(left, right, left_label="LEFT: simple 2x", right_label="RIGHT: blend"):
    # 画面サイズを大きく変えずに、左半分と右半分だけを差し替える比較。
    right = resize_like(right, left)
    w = left.width
    line_w = max(4, int(SPLIT_LINE_WIDTH))
    remain = max(16, w - line_w)
    lw = _even(remain // 2)
    rw = remain - lw
    if rw % 2 != 0:
        rw -= 1
    l = core.std.Crop(left, right=w - lw)
    r = core.std.Crop(right, left=w - rw)
    l = label_clip(l, left_label, alignment=7)
    r = label_clip(r, right_label, alignment=9)
    return core.std.StackHorizontal([l, separator(left), r])


def side_by_side_compare(left, right, left_label="LEFT: simple 2x", right_label="RIGHT: blend"):
    # 全画面同士を横に並べるので、halfより違いが分かりやすい確認用。
    right = resize_like(right, left)
    l = label_clip(left, left_label, alignment=7)
    r = label_clip(right, right_label, alignment=7)
    return core.std.StackHorizontal([l, separator(left), r])

##################
# Pipeline
##################

# 参考DMLスクリプトと同じret行。戻り値は使いませんが構造を揃えています。
ret = k7f.FMT_CTRL(clip, h_max=1200, h_ret=True)

src = apply_source_aspect(clip)
pre = fmt_pre(src)
simple = simple_2x(pre)
engine = build_engine(pre, simple)
blend = merge_simple(simple, engine, ENGINE_BLEND)

test = TEST_MODE.lower().strip()

if test == "simple2x":
    out = mark(simple, "simple2x")
elif test == "engine_only":
    out = mark(engine, "engine_only")
elif test == "half":
    out = mark(
        half_compare(simple, blend, "LEFT: simple 2x", f"RIGHT: AI blend {ENGINE_BLEND:.2f}"),
        f"half / blend={ENGINE_BLEND:.2f}",
    )
elif test == "side_by_side":
    out = mark(
        side_by_side_compare(simple, blend, "LEFT: simple 2x", f"RIGHT: AI blend {ENGINE_BLEND:.2f}"),
        f"side_by_side / blend={ENGINE_BLEND:.2f}",
    )
elif test == "blend":
    out = mark(blend, f"blend={ENGINE_BLEND:.2f}")
elif test == "off":
    out = blend
else:
    out = mark(blend, f"blend={ENGINE_BLEND:.2f}")

out = fmt_post(out)
out.set_output()
