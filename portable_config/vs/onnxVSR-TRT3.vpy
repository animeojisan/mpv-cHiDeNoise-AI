import os
import re
import subprocess
import vapoursynth as vs

try:
    import onnx
except Exception:
    onnx = None

core = vs.core
_cpu = os.cpu_count() or 8
core.num_threads = max(4, min(_cpu, 16))

# ===== user settings =====
NUM_STREAMS = 1
FORCE_REBUILD_ENGINE = False
SHOW_TRTEXEC_CONSOLE = True
SAVE_LOGS = False         # Trueでログ保存 / Falseで保存しない
KEEP_BUILD_TEMP = False
LOG_NAME = 'onnxvsr_playback.log'
BOOTSTRAP_LOG_NAME = 'onnxvsr_bootstrap.log'
MODEL_FILENAME = '2x_bndl_animefilm_tfdat_v2_sharp_1x15xHxW_dyn-HW_fp32_op20.onnx'
ENGINE_PREFIX = os.path.splitext(MODEL_FILENAME)[0]
INPUT_NAME = 'input'
TEMPORAL_FRAMES = None      # None=ONNX等から自動判定。3/5/7/9/11... の奇数想定
INPUT_CHANNELS = None       # None=自動判定。通常は TEMPORAL_FRAMES * 3
MIN_BUILD_W = 64
MIN_BUILD_H = 64
OUTPUT_FORMAT = vs.YUV420P10   # 16bit→10bitへ。最終変換コストと帯域を少し軽くする
MODEL_INPUT_FORMAT = vs.RGBS   # FP32 input to match this ONNX/engine
OUTPUT_TO_SOURCE_SIZE = False
TRIM_ENGINE_REUSE_TO_SOURCE_SIZE = True

# opt3 を最優先。安定用の最小限フォールバックだけ残す
TRY_CONFIGS = [
    {'fp16': True,  'builder_opt': 3, 'tag': 'fp16_opt3'},
    {'fp16': True,  'builder_opt': 1, 'tag': 'fp16_opt1'},
    {'fp16': False, 'builder_opt': 3, 'tag': 'fp32_opt3'},
]

script_dir = os.path.dirname(os.path.realpath(__file__))
bootstrap_log_path = os.path.join(script_dir, BOOTSTRAP_LOG_NAME)


def bootstrap_log(text: str):
    if not SAVE_LOGS:
        return
    try:
        with open(bootstrap_log_path, 'a', encoding='utf-8', errors='replace') as f:
            f.write(text)
            if not text.endswith('\n'):
                f.write('\n')
    except Exception:
        pass


def fail(msg: str):
    bootstrap_log('FAIL: ' + msg)
    raise RuntimeError(msg)


def align_up(x: int, n: int) -> int:
    return ((x + n - 1) // n) * n


def shape_candidates(src_w: int, src_h: int):
    seen = set()
    # 候補数を絞る。素の解像度を優先し、次に 16/32 アラインだけ試す
    base = [
        (src_w, src_h),
        (align_up(src_w, 16), align_up(src_h, 16)),
        (align_up(src_w, 32), align_up(src_h, 32)),
    ]
    for w, h in base:
        w = max(MIN_BUILD_W, w)
        h = max(MIN_BUILD_H, h)
        if (w, h) not in seen:
            seen.add((w, h))
            yield w, h


def infer_temporal_frames_from_onnx(onnx_model_path: str):
    if onnx is None:
        return None
    try:
        model = onnx.load(onnx_model_path, load_external_data=False)
        for inp in model.graph.input:
            if inp.name != INPUT_NAME:
                continue
            tensor_type = inp.type.tensor_type
            shape = tensor_type.shape.dim
            if len(shape) < 2:
                continue
            ch_dim = shape[1]
            if ch_dim.HasField('dim_value'):
                ch = int(ch_dim.dim_value)
                if ch > 0 and ch % 3 == 0:
                    return ch // 3
        for inp in model.graph.input:
            tensor_type = inp.type.tensor_type
            shape = tensor_type.shape.dim
            if len(shape) < 2:
                continue
            ch_dim = shape[1]
            if ch_dim.HasField('dim_value'):
                ch = int(ch_dim.dim_value)
                if ch > 0 and ch % 3 == 0:
                    return ch // 3
    except Exception:
        return None
    return None


def infer_temporal_frames_from_filename(name: str):
    m = re.search(r'(?<!\d)(\d+)f(?!\d)', name, flags=re.IGNORECASE)
    if not m:
        return None
    try:
        val = int(m.group(1))
        return val if val > 0 else None
    except Exception:
        return None


def resolve_temporal_frames_and_channels():
    frames = TEMPORAL_FRAMES
    if frames is None:
        frames = infer_temporal_frames_from_onnx(onnx_path)
    if frames is None:
        frames = infer_temporal_frames_from_filename(MODEL_FILENAME)
    if frames is None and INPUT_CHANNELS is not None and INPUT_CHANNELS > 0 and INPUT_CHANNELS % 3 == 0:
        frames = INPUT_CHANNELS // 3
    if frames is None:
        fail('Could not determine temporal frame count from ONNX / filename / INPUT_CHANNELS.\n'
             'Please set TEMPORAL_FRAMES manually in user settings.')
    if frames <= 0 or (frames % 2) == 0:
        fail(f'Invalid temporal frame count: {frames}. Expected odd value like 3/5/7/9/11.')

    channels = INPUT_CHANNELS
    expected_channels = frames * 3
    if channels is None:
        channels = expected_channels
    if channels != expected_channels:
        fail(f'INPUT_CHANNELS mismatch. temporal_frames={frames}, expected_channels={expected_channels}, actual={channels}')
    return frames, channels


def engine_name_for(src_w: int, src_h: int, build_w: int, build_h: int, fp16: bool):
    prec = 'fp16' if fp16 else 'fp32'
    return f'{ENGINE_PREFIX}_{prec}_{src_w}x{src_h}.engine'


def engine_name_legacy(src_w: int, src_h: int, fp16: bool):
    prec = 'fp16' if fp16 else 'fp32'
    return f'{ENGINE_PREFIX}_{prec}_{src_w}x{src_h}.engine'


def find_root_dir():
    candidates = [
        os.path.normpath(os.path.join(script_dir, '..', '..')),
        os.getcwd(),
        os.path.normpath(os.path.join(os.getcwd(), '..')),
    ]
    checked = []
    for c in candidates:
        if not c:
            continue
        c = os.path.normpath(c)
        if c in checked:
            continue
        checked.append(c)
        mpv_exe = os.path.join(c, 'mpv.exe')
        portable_cfg = os.path.join(c, 'portable_config')
        trtexec = os.path.join(c, 'vs-plugins', 'vsmlrt-cuda', 'trtexec.exe')
        if os.path.isfile(trtexec) and (os.path.isfile(mpv_exe) or os.path.isdir(portable_cfg)):
            return c
    return candidates[0]


root_dir = find_root_dir()
portable_config_dir = os.path.join(root_dir, 'portable_config')
cache_dir = os.path.join(portable_config_dir, 'cache')
log_path = os.path.join(cache_dir, LOG_NAME)
trtexec_path = os.path.join(root_dir, 'vs-plugins', 'vsmlrt-cuda', 'trtexec.exe')
model_dir = os.path.join(root_dir, 'vs-plugins', 'models', '55ai')
onnx_path = os.path.join(model_dir, MODEL_FILENAME)

bootstrap_log('============================================================')
bootstrap_log('Portable VSR playback bootstrap start')
bootstrap_log('script_dir=' + script_dir)
bootstrap_log('cwd=' + os.getcwd())
bootstrap_log('root_dir=' + root_dir)
bootstrap_log('trtexec_path=' + trtexec_path)
bootstrap_log('onnx_path=' + onnx_path)

for d in [portable_config_dir, cache_dir, model_dir]:
    try:
        os.makedirs(d, exist_ok=True)
    except Exception as e:
        fail('Failed to create directory: ' + d + '\n' + str(e))


def log_write(text: str):
    if SAVE_LOGS:
        with open(log_path, 'a', encoding='utf-8', errors='replace') as f:
            f.write(text)
            if not text.endswith('\n'):
                f.write('\n')
    bootstrap_log(text)


def reset_log():
    if not SAVE_LOGS:
        return
    with open(log_path, 'w', encoding='utf-8', errors='replace') as f:
        f.write('')


def run_build(onnx_input_path: str, final_engine_path: str, build_w: int, build_h: int, cfg: dict, cwd: str):
    command = [
        trtexec_path,
        '--onnx=' + onnx_input_path,
        '--saveEngine=' + final_engine_path,
        f'--minShapes={INPUT_NAME}:1x{INPUT_CHANNELS_RESOLVED}x{MIN_BUILD_H}x{MIN_BUILD_W}',
        f'--optShapes={INPUT_NAME}:1x{INPUT_CHANNELS_RESOLVED}x{build_h}x{build_w}',
        f'--maxShapes={INPUT_NAME}:1x{INPUT_CHANNELS_RESOLVED}x{build_h}x{build_w}',
        f'--builderOptimizationLevel={cfg["builder_opt"]}',
        '--skipInference',
    ]
    if cfg['fp16']:
        command.append('--fp16')

    log_write('----------------------------------------')
    log_write('try=' + cfg['tag'])
    log_write('command=' + subprocess.list2cmdline(command))
    log_write('cwd=' + cwd)

    creationflags = 0
    if SHOW_TRTEXEC_CONSOLE and os.name == 'nt':
        creationflags = getattr(subprocess, 'CREATE_NEW_CONSOLE', 0)

    if SHOW_TRTEXEC_CONSOLE and os.name == 'nt':
        cmdline = subprocess.list2cmdline(command)
        console_script = (
            '@echo off & '
            f'echo [TensorRT] build start: {cfg["tag"]} {build_w}x{build_h} & '
            f'echo [TensorRT] onnx: {os.path.basename(onnx_input_path)} & '
            f'echo [TensorRT] engine: {os.path.basename(final_engine_path)} & '
            'echo [TensorRT] running trtexec... & '
            f'{cmdline} & '
            'echo. & '
            'echo [TensorRT] trtexec finished with exit code: %errorlevel%'
        )
        result = subprocess.run(
            ['cmd.exe', '/c', console_script],
            cwd=cwd,
            creationflags=creationflags,
        )
        log_write('returncode=' + str(result.returncode))
        log_write('---- trtexec output begin ----')
        log_write('(console passthrough mode; output shown in spawned console)')
        log_write('---- trtexec output end ----')
        return result.returncode

    result = subprocess.run(
        command,
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding='utf-8',
        errors='replace',
        creationflags=creationflags,
    )
    log_write('returncode=' + str(result.returncode))
    log_write('---- trtexec output begin ----')
    log_write(result.stdout or '')
    log_write('---- trtexec output end ----')
    return result.returncode


def repeat_first_frame(c: vs.VideoNode, count: int) -> vs.VideoNode:
    if count <= 0:
        return None
    one = core.std.Trim(c, first=0, last=0)
    out = one
    for _ in range(count - 1):
        out += one
    return out


def repeat_last_frame(c: vs.VideoNode, count: int) -> vs.VideoNode:
    if count <= 0:
        return None
    one = core.std.Trim(c, first=c.num_frames - 1, last=c.num_frames - 1)
    out = one
    for _ in range(count - 1):
        out += one
    return out


def temporal_shift(c: vs.VideoNode, offset: int) -> vs.VideoNode:
    if offset == 0:
        return c
    if offset < 0:
        pad = -offset
        head = repeat_first_frame(c, pad)
        body = core.std.Trim(c, first=0, last=c.num_frames - pad - 1)
        return head + body
    pad = offset
    body = core.std.Trim(c, first=pad, last=c.num_frames - 1)
    tail = repeat_last_frame(c, pad)
    return body + tail


def pack_temporal_rgb_planes(rgb_clip: vs.VideoNode, temporal_frames: int):
    radius = temporal_frames // 2
    packed = []
    for offset in range(-radius, radius + 1):
        shifted = temporal_shift(rgb_clip, offset)
        packed.extend([core.std.ShufflePlanes(shifted, planes=p, colorfamily=vs.GRAY) for p in (0, 1, 2)])
    return packed


def resize_to_model_rgb(c: vs.VideoNode, width: int, height: int) -> vs.VideoNode:
    if c.width != width or c.height != height:
        return core.resize.Bilinear(c, width=width, height=height, format=MODEL_INPUT_FORMAT, matrix_in_s='709')
    return core.resize.Bilinear(c, format=MODEL_INPUT_FORMAT, matrix_in_s='709')


reset_log()
log_write('============================================================')
log_write('Portable VSR playback session')
log_write('root_dir=' + root_dir)
log_write('script_dir=' + script_dir)
log_write('cache_dir=' + cache_dir)
log_write('model_dir=' + model_dir)
log_write('trtexec_path=' + trtexec_path)
log_write('onnx_path=' + onnx_path)

if not os.path.isfile(trtexec_path):
    fail('trtexec.exe not found:\n' + trtexec_path)
if not os.path.isfile(onnx_path):
    fail('ONNX model not found:\n' + onnx_path)
if 'video_in' not in globals():
    fail('video_in is not defined. This VPY must be launched from mpv/vssource pipeline.')

TEMPORAL_FRAMES_RESOLVED, INPUT_CHANNELS_RESOLVED = resolve_temporal_frames_and_channels()
log_write(f'temporal_frames={TEMPORAL_FRAMES_RESOLVED}')
log_write(f'input_channels={INPUT_CHANNELS_RESOLVED}')
log_write(f'core.num_threads={core.num_threads}')

clip = video_in
src_w = clip.width
src_h = clip.height
log_write(f'src_size={src_w}x{src_h}')

final_engine_path = None
selected_build_w = src_w
selected_build_h = src_h
selected_cfg = None
attempt_errors = []

for build_w, build_h in shape_candidates(src_w, src_h):
    log_write(f'candidate_build_size={build_w}x{build_h}')
    for cfg in TRY_CONFIGS:
        target_engine_name = engine_name_for(src_w, src_h, build_w, build_h, cfg['fp16'])
        target_engine_path = os.path.join(model_dir, target_engine_name)
        legacy_engine_path = os.path.join(model_dir, engine_name_legacy(src_w, src_h, cfg['fp16']))

        if FORCE_REBUILD_ENGINE:
            for p in [target_engine_path, legacy_engine_path]:
                if os.path.exists(p):
                    try:
                        os.remove(p)
                    except Exception:
                        pass

        if os.path.isfile(target_engine_path) and os.path.getsize(target_engine_path) > 0:
            final_engine_path = target_engine_path
            selected_build_w = build_w
            selected_build_h = build_h
            selected_cfg = cfg
            log_write('reuse_engine=' + final_engine_path)
            break
        if os.path.isfile(legacy_engine_path) and os.path.getsize(legacy_engine_path) > 0:
            final_engine_path = legacy_engine_path
            selected_build_w = src_w
            selected_build_h = src_h
            selected_cfg = cfg
            log_write('reuse_legacy_engine=' + final_engine_path)
            break

        if os.path.exists(target_engine_path):
            try:
                os.remove(target_engine_path)
            except Exception:
                pass

        rc = run_build(onnx_path, target_engine_path, build_w, build_h, cfg, model_dir)
        engine_exists = os.path.isfile(target_engine_path)
        engine_size = os.path.getsize(target_engine_path) if engine_exists else -1
        log_write('engine_exists=' + str(engine_exists))
        log_write('engine_size=' + str(engine_size))

        if rc == 0 and engine_exists and engine_size > 0:
            final_engine_path = target_engine_path
            selected_build_w = build_w
            selected_build_h = build_h
            selected_cfg = cfg
            log_write('final_engine_path=' + final_engine_path)
            break

        attempt_errors.append(f'{cfg["tag"]}@{build_w}x{build_h}: rc={rc}, engine_size={engine_size}')

    if final_engine_path:
        break

if not final_engine_path:
    tail = '\n'.join(attempt_errors[-12:])
    fail('All TensorRT build attempts failed. See log:\n' + log_path + '\n\nLast attempts:\n' + tail)

log_write('inference_engine=' + final_engine_path)
log_write(f'inference_build_size={selected_build_w}x{selected_build_h}')
log_write('inference_try=' + (selected_cfg['tag'] if selected_cfg else 'unknown'))

# 未使用だった clips 生成はやめ、packed_planes のみを準備する
proc = resize_to_model_rgb(clip, selected_build_w, selected_build_h)
packed_planes = pack_temporal_rgb_planes(proc, TEMPORAL_FRAMES_RESOLVED)
log_write(f'prepared_packed_planes={len(packed_planes)}')
log_write('model_input_format=' + ('RGBS' if MODEL_INPUT_FORMAT == vs.RGBS else 'RGBH'))

if not hasattr(core, 'trt'):
    fail('core.trt is not available. The vstrt plugin may not be loaded.\n'
         'Expected plugin path under portable distribution: vs-plugins\\vstrt*.dll\n'
         'log=' + log_path)

inference_errors = []
out = None

# 安定していた packed 系だけに絞る
inference_attempts = [
    ('packed-list-positional', lambda: core.trt.Model(packed_planes, engine_path=final_engine_path, num_streams=NUM_STREAMS)),
    ('packed-tuple-positional', lambda: core.trt.Model(tuple(packed_planes), engine_path=final_engine_path, num_streams=NUM_STREAMS)),
    ('packed-kw-clips', lambda: core.trt.Model(clips=packed_planes, engine_path=final_engine_path, num_streams=NUM_STREAMS)),
]

for attempt_name, attempt_fn in inference_attempts:
    try:
        log_write('inference_attempt=' + attempt_name)
        out = attempt_fn()
        log_write('inference_attempt_success=' + attempt_name)
        break
    except Exception as e:
        msg = str(e)
        inference_errors.append(attempt_name + ': ' + msg)
        log_write('inference_attempt_failed=' + attempt_name)
        log_write('inference_error=' + msg)

if out is None:
    fail('TensorRT inference setup failed after packed call styles. The engine was built, but playback init failed.\n'
         'engine=' + final_engine_path + '\n'
         'log=' + log_path + '\n\n'
         'attempts:\n' + '\n'.join(inference_errors))

# モデル出力解像度を保持したまま、最終変換だけ軽量寄りにする
log_write(f'output_size_before_format={out.width}x{out.height}')
out = core.resize.Bilinear(out, format=OUTPUT_FORMAT, matrix_s='709')
log_write(f'final_output_size={out.width}x{out.height}')
out.set_output()
