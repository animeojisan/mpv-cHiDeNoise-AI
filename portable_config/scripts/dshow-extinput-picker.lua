-- dshow-extinput-picker.lua
-- External input picker for mpv (Windows / DirectShow)
-- ✅ オーディオデバイスの検出力を強化した修正版＋低遅延チューニング

local mp = require "mp"
local options = require "mp.options"
local utils = require "mp.utils"

local o = {
    video_regex = "",
    audio_regex = "",
    strict_filter = false,
    show_audio_menu = true,
    font_size =40,
    max_items = 25,
    allow_system_ffmpeg_fallback = true,
}
options.read_options(o)

local ov = mp.create_osd_overlay("ass-events")

local state = {
    active = false,
    stage = "video",
    video_all = {},
    audio_all = {},
    video_list = {},
    audio_list = {},
    list = {},
    idx = 1,
    video_sel = nil,
    audio_sel = nil,
}

-- ■■■ 低遅延設定 ■■■
local latency_props = {
    ["demuxer"]                      = "lavf",
    ["demuxer-lavf-probe-info"]      = "nostreams",
    ["demuxer-lavf-analyzeduration"] = "0.1",
    ["cache-pause"]                  = "no",
    cache                            = "no",
    ["demuxer-readahead-secs"]       = "0",
    ["video-sync"]                   = "audio",
    ["video-sync-max-audio-change"]  = "0.01",
    ["audio-buffer"]                 = "0",
    ["vd-lavc-threads"]              = "0",
    ["video-latency-hacks"]          = "yes", -- VO側のバッファを1〜2フレーム分詰める
    interpolation                    = "no",
    untimed                          = "yes",
}

local function enable_low_latency()
    -- mpv 側の各種バッファ削減
    for name, val in pairs(latency_props) do
        mp.set_property(name, tostring(val))
    end

    -- libavformat(dshow) 側のリアルタイム系設定
    -- ・fflags=+nobuffer      : デマックスレイヤのバッファを削って遅延を抑制
    -- ・rtbufsize=150M        : ドロップを抑えつつ、そこまで深いバッファにしない
    -- ・audio_buffer_size=64  : DirectShow オーディオのデフォルト 500ms → 約64ms まで縮小
    mp.commandv("change-list", "demuxer-lavf-o", "add", "fflags=+nobuffer")
    mp.commandv("change-list", "demuxer-lavf-o", "add", "rtbufsize=100M")
    mp.commandv("change-list", "demuxer-lavf-o", "add", "audio_buffer_size=72")
end

-- ■■■ ユーティリティ ■■■

local function join(p1, p2)
    return utils.join_path(p1, p2)
end

local function ass_escape(s)
    s = s:gsub("\\", "\\\\"):gsub("{", "\\{"):gsub("}", "\\}")
    return s
end

local function clamp(v, lo, hi)
    return math.max(lo, math.min(v, hi))
end

local function file_exists(p)
    return p and p ~= "" and utils.file_info(p) ~= nil
end

local function expand_path(p)
    return p and mp.command_native({ "expand-path", p }) or p
end

local function resolve_bundled_ffmpeg()
    local cand1 = expand_path("~~exe_dir/ffmpeg.exe")
    if file_exists(cand1) then return cand1 end

    local cfg = mp.get_property("config-dir")
    if cfg then
        local parent = cfg:match("^(.*)[/\\][^/\\]+$")
        if parent then
            local candp = join(parent, "ffmpeg.exe")
            if file_exists(candp) then return candp end
        end
    end

    if o.allow_system_ffmpeg_fallback then return "ffmpeg" end
    return nil
end

-- ■■■ 【修正】強化版デバイス名抽出ロジック ■■■
local function extract_names(text)
    local video, audio = {}, {}
    local current_mode = nil

    for line in text:gmatch("[^\r\n]+") do
        -- モード判定 (ヘッダー行による判定)
        if line:find("DirectShow video devices", 1, true) then
            current_mode = "video"
        elseif line:find("DirectShow audio devices", 1, true) then
            current_mode = "audio"
        end

        -- デバイス名（引用符の中身）を抽出
        local name = line:match('"%s*([^"]+)%s*"')
        
        -- Alternative name は無視
        if name and not name:find("Alternative name", 1, true) then
            -- 1. ヘッダーによってモードが確定している場合
            if current_mode == "video" then
                table.insert(video, name)
            elseif current_mode == "audio" then
                table.insert(audio, name)
            end

            -- 2. ヘッダーがない、または一行に (video) 等が含まれる形式の場合
            local kind_raw = line:match('%((.-)%)')
            if kind_raw then
                local kl = kind_raw:lower()
                if kl:find("video") or kind_raw:find("ビデオ") or kind_raw:find("映像") then
                    table.insert(video, name)
                elseif kl:find("audio") or kind_raw:find("オーディオ") or kind_raw:find("音声") then
                    table.insert(audio, name)
                end
            end
        end
    end

    -- 重複を削除する関数
    local function make_unique(list)
        local hash, res = {}, {}
        for _, v in ipairs(list) do
            if not hash[v] then
                res[#res + 1] = v
                hash[v] = true
            end
        end
        return res
    end

    return make_unique(video), make_unique(audio)
end

-- ■■■ メインロジック ■■■

local function get_dshow_devices()
    local ff = resolve_bundled_ffmpeg()
    if not ff then
        mp.osd_message("ffmpegが見つかりません", 5.0)
        return nil, nil, "err"
    end

    local res = mp.command_native({
        name = "subprocess",
        playback_only = false,
        capture_stdout = true,
        capture_stderr = true,
        args = { ff, "-hide_banner", "-list_devices", "true", "-f", "dshow", "-i", "dummy" }
    })

    local text = (res.stderr or "") .. "\n" .. (res.stdout or "")
    return extract_names(text)
end

local function build_lists()
    local function filter(list, reg)
        if not reg or reg == "" then return list end
        local out = {}
        for _, v in ipairs(list) do if v:match(reg) then table.insert(out, v) end end
        return out
    end
    state.video_list = filter(state.video_all, o.video_regex)
    state.audio_list = filter(state.audio_all, o.audio_regex)
    
    if not o.strict_filter then
        if #state.video_list == 0 then state.video_list = state.video_all end
        if #state.audio_list == 0 then state.audio_list = state.audio_all end
    end
end

local function render_menu()
    if not state.active then ov.data = "" ov:remove() return end
    
    local title = (state.stage == "video") and "【ビデオ】デバイスを選択" or "【オーディオ】デバイスを選択"
    local header = string.format("{\\fs%d\\b1}%s{\\b0}\\N", o.font_size + 4, ass_escape(title))
    local hint = "{\\fs18}↑↓:移動 / Enter:決定 / Esc:閉じる{\\fs0}\\N\\N"
    
    local start = clamp(state.idx - math.floor(o.max_items/2), 1, math.max(1, #state.list - o.max_items + 1))
    local body = ""
    for i = start, math.min(start + o.max_items - 1, #state.list) do
        local prefix = (i == state.idx) and "▶ " or "  "
        body = body .. string.format("{\\fs%d}%s%s{\\fs0}\\N", o.font_size, prefix, ass_escape(state.list[i] or ""))
    end
    ov.data = header .. hint .. body
    ov:update()
end

local function close_menu()
    state.active = false
    mp.remove_key_binding("ext_up")
    mp.remove_key_binding("ext_down")
    mp.remove_key_binding("ext_enter")
    mp.remove_key_binding("ext_esc")
    render_menu()
end

local function apply_selection()
    local url = "av://dshow:video=" .. state.video_sel
    if state.audio_sel then
        url = url .. ":audio=" .. state.audio_sel
    end
    enable_low_latency()
    mp.commandv("loadfile", url, "replace")
    close_menu()
end

local function enter_action()
    local sel = state.list[state.idx]
    if not sel then return end

    if state.stage == "video" then
        state.video_sel = sel
        -- オーディオデバイスが存在し、設定で有効ならオーディオ選択へ
        if o.show_audio_menu and #state.audio_list > 0 then
            state.stage = "audio"
            state.list = { "<no audio>" }
            for _, a in ipairs(state.audio_list) do table.insert(state.list, a) end
            state.idx = 1
            render_menu()
        else
            apply_selection()
        end
    else
        state.audio_sel = (sel ~= "<no audio>") and sel or nil
        apply_selection()
    end
end

local function show_menu()
    local v, a, err = get_dshow_devices()
    if err or not v or #v == 0 then
        mp.osd_message("デバイスが見つかりません", 5.0)
        return
    end

    state.video_all, state.audio_all = v, a
    build_lists()
    
    state.active = true
    state.stage = "video"
    state.list = state.video_list
    state.idx = 1
    
    mp.add_forced_key_binding("UP", "ext_up", function() state.idx = clamp(state.idx-1, 1, #state.list) render_menu() end, {repeatable=true})
    mp.add_forced_key_binding("DOWN", "ext_down", function() state.idx = clamp(state.idx+1, 1, #state.list) render_menu() end, {repeatable=true})
    mp.add_forced_key_binding("ENTER", "ext_enter", enter_action)
    mp.add_forced_key_binding("ESC", "ext_esc", close_menu)
    render_menu()
end

mp.register_script_message("show", show_menu)
