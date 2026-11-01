"""Keep original interpolation phases in their original format and pixels."""
import vapoursynth as vs

def source_passthrough(filtered, source, factor):
    if not isinstance(factor, int) or not 2 <= factor <= 8:
        return filtered
    if source.format is None or filtered.format is None:
        return filtered
    if (filtered.width, filtered.height, filtered.format.id) != (source.width, source.height, source.format.id):
        return filtered
    if filtered.num_frames != source.num_frames * factor:
        return filtered
    if source.fps_num <= 0 or source.fps_den <= 0 or filtered.fps_num * source.fps_den != source.fps_num * factor * filtered.fps_den:
        return filtered
    repeated = vs.core.std.Interleave([source] * factor)
    repeated = vs.core.std.SetFrameProps(repeated, _DurationNum=filtered.fps_den, _DurationDen=filtered.fps_num)
    return vs.core.std.FrameEval(filtered, eval=lambda n: repeated if n % factor == 0 else filtered)
