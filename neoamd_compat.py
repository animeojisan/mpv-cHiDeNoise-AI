"""Shared mpv entry point for the SHA-indexed NeoAMD compatibility catalog."""
import json
from fractions import Fraction
from pathlib import Path
import vapoursynth as vs
import neogpu_mpv_fast as neo
from neoamd_interpolation import source_passthrough

ROOT=Path(__file__).resolve().parent
CATALOG=json.loads((ROOT/"portable_config/neoamd-compatible-models.json").read_text(encoding="utf-8"))

def apply(clip, sha256, factor=2, container_fps=0):
    entry=CATALOG[sha256]
    model=ROOT/entry["model_path"]
    kind=entry["kind"]
    if kind==0:
        return neo.image(clip,backend="NeoAMD",model=str(model),mode=entry.get("mode",0))
    if kind==2:
        return neo.temporal(clip,backend="NeoAMD",model=str(model),mode=0)
    fps=clip.fps_num/clip.fps_den if clip.fps_num and clip.fps_den else container_fps
    if not fps or fps<=0:raise ValueError("Source FPS is unknown")
    rate=Fraction(str(fps)).limit_denominator(100000)
    clip=vs.core.std.AssumeFPS(clip,fpsnum=rate.numerator,fpsden=rate.denominator)
    if clip.num_frames>2147483647//factor:
        clip=vs.core.std.Trim(clip,last=2147483647//factor-1)
    source_clip=clip
    original=clip.format
    if original is None:raise ValueError("NeoAMD requires a constant pixel format")
    core=vs.core
    try:plugin=core.neogpuexp
    except AttributeError:
        core.std.LoadPlugin(path=str(ROOT/"vs-plugins/vs-neogpu-exp.dll"));plugin=core.neogpuexp
    rgb=core.resize.Bicubic(clip,format=vs.RGB24,**({"matrix_in_s":"709"} if original.color_family==vs.YUV else {}))
    result=plugin.NeoAMDInterpolate(rgb,model=str(model),backend=str(ROOT/"backends/neoamd"),factor=factor,scene_threshold=.15,mode=0)
    if original.color_family==vs.RGB:return source_passthrough(core.resize.Bicubic(result,format=original.id,dither_type="error_diffusion"),source_clip,factor)
    limited=core.resize.Bicubic(result,format=original.id,matrix_s="709",range_s="limited",dither_type="error_diffusion")
    full=core.resize.Bicubic(result,format=original.id,matrix_s="709",range_s="full",dither_type="error_diffusion")
    properties=core.std.Interleave([clip]*factor)
    return source_passthrough(core.std.FrameEval(limited,eval=lambda n,f:full if f.props.get("_ColorRange",1)==0 else limited,prop_src=properties),source_clip,factor)
