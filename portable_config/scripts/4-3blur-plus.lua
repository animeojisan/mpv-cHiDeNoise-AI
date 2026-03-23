-- オーバーレイフィルター付き映像処理スクリプト
-- DVD 4:3 特別処理 + 1920x1080超の縮小統合版
-- boxblur値とoverlay_w除数を script-opts/4-3blur-plus.conf から読み込む対応
--
-- 2025-12 改善点:
--  1) 16:9算出幅が奇数になるケースを偶数に丸め、
--     VapourSynthの "unaligned/cropped video sizes" を回避
--  2) stdin/rawvideo相当の入力では loadfile/seek/time-pos 復帰を避ける
--  3) ★軽量化: rawvideo/stdin キャプチャ時のみ
--       - format=rgba を避け format=yuv444p
--       - 左右ブラーを「縮小→boxblur→拡大」の安価ブラーに

local mp = require 'mp'

local overlay_active = false
local busy = false
local overlay_auto_enable_next = false

local script_id = "4-3blur-plus"
local lockfile_path = "portable_config/scripts/overlay_active_lock.txt"

local video_w, video_h = 0, 0
local video_sar = 1.0

mp.observe_property("video-params", "native", function(_, params)
    if params then
        video_w = params.width or 0
        video_h = params.height or 0
        video_sar = (params.sar and params.sar > 0) and params.sar or 1.0
    end
end)

-- confファイルからboxblurパラメータを読み込む
local function read_blur_conf()
    local conf_path = mp.command_native({"expand-path", "~~/script-opts/4-3blur-plus.conf"})
    local f = io.open(conf_path, "r")
    if not f then
        return "9:2" -- デフォルト値
    end
    local content = f:read("*all") or ""
    f:close()
    local val = content:match("boxblur%s*=%s*([%d%.]+:%d+)")
    if val then
        return val
    else
        return "9:2"
    end
end

-- confファイルからoverlay_wの除数を読み込む
local function read_overlay_w_divisor()
    local conf_path = mp.command_native({"expand-path", "~~/script-opts/4-3blur-plus.conf"})
    local f = io.open(conf_path, "r")
    if not f then
        return 7.2 -- デフォルト値
    end
    local content = f:read("*all") or ""
    f:close()
    local val = content:match("overlay_w%s*=%s*([%d%.]+)")
    if val then
        return tonumber(val) or 7.2
    else
        return 7.2
    end
end

local blur_params = read_blur_conf()
local overlay_w_divisor = read_overlay_w_divisor()

local function read_lockfile()
    local f = io.open(lockfile_path, "r")
    if not f then return nil end
    local content = f:read("*all")
    f:close()
    return content and content:match("^%s*(.-)%s*$")
end

local function lock_exists()
    local owner = read_lockfile()
    return owner and owner ~= script_id
end

local function create_lock()
    if lock_exists() then return false end
    local f = io.open(lockfile_path, "w")
    if not f then return false end
    f:write(script_id)
    f:close()
    return true
end

local function remove_lock()
    local owner = read_lockfile()
    if owner == script_id then
        os.remove(lockfile_path)
    end
end

-- ★ 偶数化ヘルパ（VS対策）
local function make_even(n)
    if not n then return n end
    n = math.floor(n + 0.5)
    if (n % 2) == 1 then
        return n + 1
    end
    return n
end

-- ★ stdin/ライブ系判定（安全側）
local function is_live_input()
    local path = mp.get_property("path") or ""
    if path == "-" then return true end

    local demuxer = mp.get_property("current-demuxer") or ""
    if demuxer == "rawvideo" then return true end

    demuxer = mp.get_property("demuxer") or ""
    if demuxer == "rawvideo" then return true end

    if path:match("^rawvideo://") then return true end
    return false
end

-- ★ rawvideo/キャプチャ判定（軽量モード対象）
local function is_rawvideo_capture()
    if is_live_input() then return true end
    return false
end

local function build_lavfi()
    local src_w = video_w
    local src_h = video_h
    local sar = video_sar

    if src_w == 0 or src_h == 0 then
        src_w = mp.get_property_number("width", 1280)
        src_h = mp.get_property_number("height", 720)
        sar = mp.get_property_number("video-params/sar", 1.0)
    end

    local scale_filter = ""

    -- ★ キャプチャ時のみ軽量化モードON
    local fast_mode = is_rawvideo_capture()
    local format_tag = fast_mode and "format=yuv444p" or "format=rgba"

    -- ★ 左右ブラー生成：fast_mode は縮小→blur→拡大
    local function blur_chain(in_label, out_label, crop_x, blur_w, blur_h)
        if fast_mode then
            -- 横方向を強めに縮小してコスト削減
            local small_w = math.max(32, math.floor(blur_w / 3 + 0.5))
            return string.format(
                "[%s]crop=%d:%d:%d:0,scale=%d:%d:flags=bilinear,boxblur=%s,scale=%d:%d:flags=bilinear[%s];",
                in_label,
                blur_w, blur_h, crop_x,
                small_w, blur_h,
                blur_params,
                blur_w, blur_h,
                out_label
            )
        else
            return string.format(
                "[%s]crop=%d:%d:%d:0,boxblur=%s[%s];",
                in_label,
                blur_w, blur_h, crop_x,
                blur_params,
                out_label
            )
        end
    end

    -- ★ DVD PAL 720x576 特別処理
    if src_w == 720 and src_h == 576 then
        local overlay_w = 1024
        local overlay_h = 576
        local blur_w = math.floor(overlay_w / overlay_w_divisor + 0.5)
        local blur_h = overlay_h

        local center44_w = math.floor(overlay_w * 0.41 + 0.5)
        local center44_start_x = math.floor((overlay_w - center44_w) / 2 + 0.5)

        local left_crop_x = math.max(0, center44_start_x - blur_w)
        local right_crop_x = math.min(overlay_w - blur_w, center44_start_x + center44_w)

        local blurL = blur_chain("blurL", "blurLout", left_crop_x, blur_w, blur_h)
        local blurR = blur_chain("blurR", "blurRout", right_crop_x, blur_w, blur_h)

        return string.format([[
[vid1]scale=768:576,setsar=1,pad=%d:%d:(%d - iw)/2:(%d - ih)/2:color=black,%s,split=3[main][blurL][blurR];
%s
%s
[main]copy[centerout];
[centerout][blurLout]overlay=x=0:y=0[tmp];
[tmp][blurRout]overlay=x=%d:y=0,setdar=16/9[vo]
]],
        overlay_w, overlay_h, overlay_w, overlay_h, format_tag,
        blurL, blurR,
        overlay_w - blur_w
        )
    end

    -- ★ DVD NTSC 720x480 特別処理
    if src_w == 720 and src_h == 480 then
        local overlay_w = 854
        local overlay_h = 480
        local blur_w = math.floor(overlay_w / overlay_w_divisor + 0.5)
        local blur_h = overlay_h

        local center44_w = math.floor(overlay_w * 0.41 + 0.5)
        local center44_start_x = math.floor((overlay_w - center44_w) / 2 + 0.5)

        local left_crop_x = math.max(0, center44_start_x - blur_w)
        local right_crop_x = math.min(overlay_w - blur_w, center44_start_x + center44_w)

        local blurL = blur_chain("blurL", "blurLout", left_crop_x, blur_w, blur_h)
        local blurR = blur_chain("blurR", "blurRout", right_crop_x, blur_w, blur_h)

        return string.format([[
[vid1]scale=640:480,setsar=1,pad=%d:%d:(%d - iw)/2:(%d - ih)/2:color=black,%s,split=3[main][blurL][blurR];
%s
%s
[main]copy[centerout];
[centerout][blurLout]overlay=x=0:y=0[tmp];
[tmp][blurRout]overlay=x=%d:y=0,setdar=16/9[vo]
]],
        overlay_w, overlay_h, overlay_w, overlay_h, format_tag,
        blurL, blurR,
        overlay_w - blur_w
        )
    end

    -- ★ 1920x1080超の縮小
    if src_w > 1920 or src_h > 1080 then
        local scale_w = 1920 / src_w
        local scale_h = 1080 / src_h
        local scale = math.min(scale_w, scale_h)
        src_w = math.floor(src_w * scale + 0.5)
        src_h = math.floor(src_h * scale + 0.5)
        scale_filter = string.format("scale=%d:%d,", src_w, src_h)
    end

    -- ★ 低解像度分岐
    if src_h < 480 then
        local original_ar = (src_w / src_h) * sar
        local dist_16_9 = math.abs(original_ar - (16/9))
        local dist_4_3 = math.abs(original_ar - (4/3))

        if dist_4_3 < dist_16_9 then
            local overlay_w = 854
            local overlay_h = 480
            local blur_w = math.floor(overlay_w / overlay_w_divisor + 0.5)
            local blur_h = overlay_h

            local center44_w = math.floor(overlay_w * 0.41 + 0.5)
            local center44_start_x = math.floor((overlay_w - center44_w) / 2 + 0.5)

            local left_crop_x = math.max(0, center44_start_x - blur_w)
            local right_crop_x = math.min(overlay_w - blur_w, center44_start_x + center44_w)

            local blurL = blur_chain("blurL", "blurLout", left_crop_x, blur_w, blur_h)
            local blurR = blur_chain("blurR", "blurRout", right_crop_x, blur_w, blur_h)

            return string.format([[
[vid1]scale=640:480,pad=%d:%d:(%d - iw)/2:(%d - ih)/2:color=black,%s,setsar=1,split=3[main][blurL][blurR];
%s
%s
[main]copy[centerout];
[centerout][blurLout]overlay=x=0:y=0[tmp];
[tmp][blurRout]overlay=x=%d:y=0,setdar=16/9[vo]
]],
            overlay_w, overlay_h, overlay_w, overlay_h, format_tag,
            blurL, blurR,
            overlay_w - blur_w
            )
        else
            local scaled_w = math.floor(src_w * (480 / src_h) + 0.5)
            if scaled_w > 854 then scaled_w = 854 end

            local overlay_w = 854
            local overlay_h = 480
            local blur_w = math.floor(overlay_w / overlay_w_divisor + 0.5)
            local blur_h = overlay_h

            local center44_w = math.floor(overlay_w * 0.41 + 0.5)
            local center44_start_x = math.floor((overlay_w - center44_w) / 2 + 0.5)

            local left_crop_x = math.max(0, center44_start_x - blur_w)
            local right_crop_x = math.min(overlay_w - blur_w, center44_start_x + center44_w)

            local blurL = blur_chain("blurL", "blurLout", left_crop_x, blur_w, blur_h)
            local blurR = blur_chain("blurR", "blurRout", right_crop_x, blur_w, blur_h)

            return string.format([[
[vid1]scale=%d:480,pad=%d:%d:(%d - iw)/2:(%d - ih)/2:color=black,%s,setsar=1,split=3[main][blurL][blurR];
%s
%s
[main]copy[centerout];
[centerout][blurLout]overlay=x=0:y=0[tmp];
[tmp][blurRout]overlay=x=%d:y=0,setdar=16/9[vo]
]],
            scaled_w, overlay_w, overlay_h, overlay_w, overlay_h, format_tag,
            blurL, blurR,
            overlay_w - blur_w
            )
        end
    end

    -- ★ 720pっぽい中途半端幅の特別分岐
    if src_h == 720 and src_w >= 1130 and src_w <= 1150 then
        local target_w = 1280
        local pad_x = target_w - src_w
        local pad_left = math.floor(pad_x / 2 + 0.5)

        local blur_w = math.floor(target_w / 7 + 0.5) -- ここは7固定のまま
        local blur_h = src_h

        local center44_w = math.floor(target_w * 0.41 + 0.5)
        local center44_start_x = math.floor((target_w - center44_w) / 2 + 0.5)

        local left_crop_x = math.max(0, center44_start_x - blur_w)
        local right_crop_x = math.min(target_w - blur_w, center44_start_x + center44_w)

        local blurL = blur_chain("blurL", "blurLout", left_crop_x, blur_w, blur_h)
        local blurR = blur_chain("blurR", "blurRout", right_crop_x, blur_w, blur_h)

        return string.format([[
[vid1]%spad=%d:%d:%d:0:color=black,%s,split=3[main][blurL][blurR];
%s
%s
[main]copy[centerout];
[centerout][blurLout]overlay=x=0:y=0[tmp];
[tmp][blurRout]overlay=x=%d:y=0[vo]
]],
        scale_filter, target_w, src_h, pad_left, format_tag,
        blurL, blurR,
        target_w - blur_w
        )
    end

    local ar = (src_w / src_h) * sar
    local force_scale = (ar < 1.65 or ar > 1.9)

    if force_scale then
        local scaled_h = src_h
        local scaled_w = math.floor(scaled_h * 16 / 9 + 0.5)
        scaled_w = make_even(scaled_w) -- VS対策

        local blur_w = math.floor(scaled_w / overlay_w_divisor + 0.5)
        local blur_h = scaled_h

        local center44_w = math.floor(scaled_w * 0.41 + 0.5)
        local center44_start_x = math.floor((scaled_w - center44_w) / 2 + 0.5)

        local left_crop_x = math.max(0, center44_start_x - blur_w)
        local right_crop_x = math.min(scaled_w - blur_w, center44_start_x + center44_w)

        local blurL = blur_chain("blurL", "blurLout", left_crop_x, blur_w, blur_h)
        local blurR = blur_chain("blurR", "blurRout", right_crop_x, blur_w, blur_h)

        return string.format([[
[vid1]%sscale=%d:%d,%s,split=3[main][blurL][blurR];
%s
%s
[main]copy[centerout];
[centerout][blurLout]overlay=x=0:y=0[tmp];
[tmp][blurRout]overlay=x=%d:y=0[vo]
]],
        scale_filter, scaled_w, scaled_h, format_tag,
        blurL, blurR,
        scaled_w - blur_w
        )
    end

    local target_ar = 16 / 9
    local out_w = math.floor(src_h * target_ar + 0.5)
    out_w = make_even(out_w) -- VS対策

    local pad_x = math.max(0, out_w - src_w)
    local pad_left = math.floor(pad_x / 2 + 0.5)

    local scaled_w = out_w
    local scaled_h = src_h

    local blur_w = math.floor(scaled_w / overlay_w_divisor + 0.5)
    local blur_h = scaled_h

    local center44_w = math.floor(scaled_w * 0.41 + 0.5)
    local center44_start_x = math.floor((scaled_w - center44_w) / 2 + 0.5)

    local left_crop_x = math.max(0, center44_start_x - blur_w)
    local right_crop_x = math.min(scaled_w - blur_w, center44_start_x + center44_w)

    local blurL = blur_chain("blurL", "blurLout", left_crop_x, blur_w, blur_h)
    local blurR = blur_chain("blurR", "blurRout", right_crop_x, blur_w, blur_h)

    return string.format([[
[vid1]%spad=%d:%d:%d:0:color=black,%s,split=3[main][blurL][blurR];
%s
%s
[main]copy[centerout];
[centerout][blurLout]overlay=x=0:y=0[tmp];
[tmp][blurRout]overlay=x=%d:y=0[vo]
]],
    scale_filter, out_w, src_h, pad_left, format_tag,
    blurL, blurR,
    scaled_w - blur_w
    )
end

local function remove_overlay()
    if busy then
        mp.osd_message("処理中のためオーバーレイ切替不可")
        return
    end
    busy = true
    mp.set_property("lavfi-complex", "")
    overlay_active = false
    overlay_auto_enable_next = true

    -- ★ stdin/rawvideo相当では loadfile/seek をしない
    if is_live_input() then
        remove_lock()
        busy = false
        mp.osd_message("オーバーレイ表示：OFF")
        return
    end

    local path = mp.get_property("path")
    local timepos = mp.get_property_number("time-pos", 0)

    if path then
        mp.commandv("loadfile", path, "replace")
        mp.add_timeout(0.1, function()
            remove_lock()
            busy = false
            mp.osd_message("オーバーレイ表示：OFF")
        end)
        mp.add_timeout(0.3, function()
            mp.set_property_number("time-pos", timepos)
        end)
    else
        remove_lock()
        busy = false
        mp.osd_message("オーバーレイ表示：OFF")
    end
end

local function apply_overlay()
    if busy then
        mp.osd_message("処理中のため切替不可")
        return
    end
    if lock_exists() then
        mp.osd_message("他のオーバーレイ (" .. (read_lockfile() or "不明") .. ") が有効中")
        return
    end
    if not create_lock() then
        mp.osd_message("ロック取得失敗")
        return
    end

    busy = true
    overlay_active = true

    mp.set_property("lavfi-complex", build_lavfi())
    mp.osd_message("オーバーレイ表示：ON")
    busy = false
end

mp.add_key_binding("Alt+O", "4-3blur-plus", function()
    if overlay_active then
        remove_overlay()
    else
        apply_overlay()
    end
end)

mp.register_event("file-loaded", function()
    if overlay_active then
        remove_overlay()
    end
end)

mp.observe_property("idle-active", "bool", function(_, idle)
    if not idle and overlay_auto_enable_next then
        overlay_auto_enable_next = false
        mp.add_timeout(1.5, function()
            apply_overlay()
        end)
    end
end)

mp.register_event("end-file", function()
    if overlay_active then
        remove_lock()
    end
end)

mp.register_event("shutdown", function()
    if overlay_active then
        remove_lock()
    end
end)
