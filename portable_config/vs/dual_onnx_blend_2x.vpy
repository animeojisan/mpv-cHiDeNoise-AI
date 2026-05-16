### liveaction_dual_onnx_blend_2x.vpy
###
### mpv-cHiDeNoise-AI / k7sfunc1 UAI_DML 用
###
### 通常2x拡大した元映像に、2つのONNX結果をミックスします。

import vapoursynth as vs
from vapoursynth import core
import k7sfunc1 as k7f

clip = video_in

##################
# ユーザーオプション
##################

# 参考DMLスクリプトと同じ基本設定
H_Pre = 1080
H_Max = 2160
Lk_Fmt = False

# 使用するONNXモデルを2つ指定します。
# 別モデルを使う場合は、この2行を書き換えてください。
ENGINE1_MODEL = "55ai/2x_AnimeJaNai_SD_V1beta34_Compact_93k-fp16.onnx"
ENGINE2_MODEL = "55ai/ToHeart_35mm_Compact_V1_1_fp16.onnx"

# ブレンド方式切り替え
# residual : 差分ブレンド（実写向けの本命）
# alpha    : 透過ブレンド（変化が強め）
BLEND_MODE = "alpha"

# 差分ブレンド量。
# 出力 = base + (AI - base) * amount
# 0.00 = 差分なし / 1.00 = AI差分を100%反映
ENGINE1_DIFF_AMOUNT = 0.35
ENGINE2_DIFF_AMOUNT = 0.35

# Trueにすると、YUV入力時は輝度(Y)だけにAI差分を足し、色差(U/V)は元拡大を維持します。
# 実写で色ズレ・赤紫寄り・彩度変化が気になる場合に有効です。
# RGB系フォーマットでは全プレーンに適用されます。
DIFF_LUMA_ONLY = False

# 透過ブレンド量。
# 比率正規化ではなく、各レイヤーをそのまま描画した上での透過量です。
ORIGINAL_OPACITY = 1.00
ENGINE1_OPACITY = 0.45
ENGINE2_OPACITY = 0.45

Fp16_Qnt = True
Gpu = 0
Gpu_T = 2

# DVDの表示比率補正
# mpv側や別フィルターで補正済みなら keep のまま。
# 必要な場合だけ ntsc_4_3 / ntsc_16_9 / pal_4_3 / pal_16_9 を指定。
SOURCE_ASPECT_MODE = "keep"

# 反応確認用
# simple2x       : 通常2x拡大のみ
# engine1_only   : AIエンジン1単体
# engine2_only   : AIエンジン2単体
# residual_blend : 差分ブレンド結果を表示
# alpha_blend    : 透過ブレンド結果を表示
# active_blend   : BLEND_MODE に従う現在の結果を表示
# half           : 左=通常2x / 右=現在のブレンド結果。確認用
# side_by_side   : 左=通常2x全体 / 右=現在のブレンド結果全体。比較用
# off            : 通常出力。文字なしで BLEND_MODE に従う結果
TEST_MODE = "off"

# half / side_by_side 用
SHOW_COMPARE_LABELS = True
SPLIT_LINE_WIDTH = 10

# TEST_MODE != off のとき左上に文字を出す
SHOW_TEXT = True

##################
# 内部処理
##################

def _even(x):
    return int(x) & ~1


def _clamp_0_1(v):
    v = float(v)
    if v < 0.0:
        return 0.0
    if v > 1.0:
        return 1.0
    return v


def _text(c, label, alignment=7):
    try:
        return core.text.Text(c, label, alignment=alignment)
    except Exception:
        try:
            return core.text.Text(c, label)
        except Exception:
            return c


def residual_label():
    return (
        f"mode=residual / base=simple2x / "
        f"AI1diff={_clamp_0_1(ENGINE1_DIFF_AMOUNT):.2f} / "
        f"AI2diff={_clamp_0_1(ENGINE2_DIFF_AMOUNT):.2f}"
    )


def alpha_label():
    return (
        f"mode=alpha / "
        f"original={_clamp_0_1(ORIGINAL_OPACITY):.2f} / "
        f"AI1={_clamp_0_1(ENGINE1_OPACITY):.2f} / "
        f"AI2={_clamp_0_1(ENGINE2_OPACITY):.2f}"
    )


def active_label():
    mode = BLEND_MODE.lower().strip()
    if mode == "alpha":
        return alpha_label()
    return residual_label()


def mark(c, label):
    if not SHOW_TEXT:
        return c
    return _text(c, "SWITCHABLE BLEND: " + label, alignment=7)


def label_clip(c, label, alignment=7):
    if not SHOW_COMPARE_LABELS:
        return c
    return _text(c, label, alignment=alignment)


def _format_max_mid(c):
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
    return k7f.UAI_DML(
        c,
        clamp=False,
        model_pth=model_pth,
        fp16_qnt=Fp16_Qnt,
        gpu=Gpu,
        gpu_t=Gpu_T,
    )


def build_engine1(pre, simple):
    engine = run_uai_2x(pre, ENGINE1_MODEL)
    return resize_like(engine, simple)


def build_engine2(pre, simple):
    engine = run_uai_2x(pre, ENGINE2_MODEL)
    return resize_like(engine, simple)


def _expr_for_planes(ref, expr_all, expr_chroma_keep):
    if ref.format is None:
        return [expr_all]

    planes = ref.format.num_planes

    if DIFF_LUMA_ONLY and ref.format.color_family == vs.YUV and planes >= 3:
        return [expr_all] + [expr_chroma_keep] * (planes - 1)

    return [expr_all] * planes


def residual_blend(base, engine1, engine2, engine1_amount, engine2_amount):
    engine1 = resize_like(engine1, base)
    engine2 = resize_like(engine2, base)

    a1 = _clamp_0_1(engine1_amount)
    a2 = _clamp_0_1(engine2_amount)

    # x=base, y=engine1, z=engine2
    # base + (engine1 - base) * a1 + (engine2 - base) * a2
    expr = f"x y x - {a1:.8f} * + z x - {a2:.8f} * +"
    return core.std.Expr([base, engine1, engine2], expr=_expr_for_planes(base, expr, "x"))


def alpha_merge(base, overlay, opacity):
    overlay = resize_like(overlay, base)
    op = _clamp_0_1(opacity)
    if op <= 0.0:
        return base
    if op >= 1.0:
        return overlay
    return core.std.Merge(base, overlay, weight=op)


def alpha_blend(original, engine1, engine2, original_opacity, engine1_opacity, engine2_opacity):
    engine1 = resize_like(engine1, original)
    engine2 = resize_like(engine2, original)

    base = _blank_color(original, kind="black", width=original.width)
    out = alpha_merge(base, original, original_opacity)
    out = alpha_merge(out, engine1, engine1_opacity)
    out = alpha_merge(out, engine2, engine2_opacity)
    return out


def choose_active_blend(simple, engine1, engine2):
    mode = BLEND_MODE.lower().strip()
    if mode == "alpha":
        return alpha_blend(simple, engine1, engine2, ORIGINAL_OPACITY, ENGINE1_OPACITY, ENGINE2_OPACITY)
    return residual_blend(simple, engine1, engine2, ENGINE1_DIFF_AMOUNT, ENGINE2_DIFF_AMOUNT)


def half_compare(left, right, left_label="LEFT: simple 2x", right_label="RIGHT: active blend"):
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


def side_by_side_compare(left, right, left_label="LEFT: simple 2x", right_label="RIGHT: active blend"):
    right = resize_like(right, left)
    l = label_clip(left, left_label, alignment=7)
    r = label_clip(right, right_label, alignment=7)
    return core.std.StackHorizontal([l, separator(left), r])

##################
# Pipeline
##################

ret = k7f.FMT_CTRL(clip, h_max=1200, h_ret=True)

src = apply_source_aspect(clip)
pre = fmt_pre(src)
simple = simple_2x(pre)
engine1 = build_engine1(pre, simple)
engine2 = build_engine2(pre, simple)

out_residual = residual_blend(simple, engine1, engine2, ENGINE1_DIFF_AMOUNT, ENGINE2_DIFF_AMOUNT)
out_alpha = alpha_blend(simple, engine1, engine2, ORIGINAL_OPACITY, ENGINE1_OPACITY, ENGINE2_OPACITY)
out_active = choose_active_blend(simple, engine1, engine2)

test = TEST_MODE.lower().strip()
label = active_label()

if test == "simple2x":
    out = mark(simple, "simple2x")
elif test == "engine1_only":
    out = mark(engine1, "engine1_only")
elif test == "engine2_only":
    out = mark(engine2, "engine2_only")
elif test == "residual_blend":
    out = mark(out_residual, residual_label())
elif test == "alpha_blend":
    out = mark(out_alpha, alpha_label())
elif test == "active_blend":
    out = mark(out_active, label)
elif test == "half":
    out = mark(
        half_compare(simple, out_active, "LEFT: simple 2x", "RIGHT: " + label),
        "half / " + label,
    )
elif test == "side_by_side":
    out = mark(
        side_by_side_compare(simple, out_active, "LEFT: simple 2x", "RIGHT: " + label),
        "side_by_side / " + label,
    )
elif test == "off":
    out = out_active
else:
    out = mark(out_active, label)

out = fmt_post(out)
out.set_output()
