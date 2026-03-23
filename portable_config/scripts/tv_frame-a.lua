-- overlay-toggle.lua（安定版 + DVD 4:3 & DVD 16:9 NTSC & PAL 分岐修正）

local image_path = "portable_config/images/TOSHIBA-85per.png"
local overlay_active = false
local busy = false

local latest_video_w = 0
local latest_video_h = 0
local latest_sar = 1.0
local latest_dar = 1.7777
local latest_disp_w = 0
local latest_disp_h = 0

local script_id = "overlay-toggle"
local lockfile_path = "portable_config/scripts/overlay_active_lock.txt"

local function read_lockfile()
    local f = io.open(lockfile_path, "r")
    if not f then return nil end
    local content = f:read("*all")
    f:close()
    if content then
        content = content:match("^%s*(.-)%s*$")
    end
    return content
end

local function lock_exists()
    local owner = read_lockfile()
    return owner ~= nil and owner ~= script_id
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
        local success, err = os.remove(lockfile_path)
        if not success then
            mp.msg.warn("lockファイル削除失敗: " .. (err or "不明なエラー"))
        end
    end
end

mp.observe_property("video-params", "native", function(_, params)
    if params then
        latest_video_w = params.width or 0
        latest_video_h = params.height or 0
        latest_sar = (params.sar and params.sar > 0) and params.sar or 1.0
        latest_dar = (params.display_aspect and params.display_aspect > 0) and params.display_aspect or (latest_video_w / latest_video_h)
        latest_disp_w = params.dwidth or latest_video_w
        latest_disp_h = params.dheight or latest_video_h
    end
end)

local function build_lavfi()
    local src_w = latest_video_w
    local src_h = latest_video_h
    local sar = latest_sar
    local dar = latest_dar
    local dwidth = latest_disp_w
    local dheight = latest_disp_h

    if src_w == 0 or src_h == 0 then
        src_w = mp.get_property_number("width", 1280)
        src_h = mp.get_property_number("height", 720)
        sar = mp.get_property_number("video-params/sar", 1.0)
        dar = mp.get_property_number("video-params/display-aspect", src_w / src_h)
        dwidth = mp.get_property_number("dwidth", src_w)
        dheight = mp.get_property_number("dheight", src_h)
    end

    local target_output_aspect_ratio = 16 / 9
    local video_filter_chain
    local overlay_w, overlay_h

    local is_ntsc_dvd = (src_w == 720 and src_h == 480)
    local is_pal_dvd = (src_w == 720 and src_h == 576)
    local display_ar = dwidth / dheight

    if is_ntsc_dvd and math.abs(display_ar - (4 / 3)) < 0.05 then
        -- NTSC 4:3 DVD
        overlay_w = 854
        overlay_h = 480
        video_filter_chain = table.concat({
            "[vid1]scale=640:480",
            "setsar=1",
            "pad=854:480:(854 - iw)/2:(480 - ih)/2:color=black",
            string.format("setdar=%f[video_padded]", target_output_aspect_ratio)
        }, ",")

    elseif is_ntsc_dvd and sar > 1.05 then
        -- NTSC 16:9 DVD
        overlay_w = math.floor(src_w * sar + 0.5)
        overlay_h = src_h
        video_filter_chain = string.format(
            "[vid1]scale=%d:%d,setsar=1,setdar=%f[video_padded]",
            overlay_w, overlay_h, target_output_aspect_ratio
        )

    elseif is_pal_dvd and math.abs(display_ar - (4 / 3)) < 0.05 then
        -- PAL 4:3 DVD
        -- PAL 720x576で4:3表示は縦解像度を480に縮小して640x480にスケールし、854x480に黒帯パッド
        overlay_w = 854
        overlay_h = 480
        video_filter_chain = table.concat({
            "[vid1]scale=640:480",
            "setsar=1",
            "pad=854:480:(854 - iw)/2:(480 - ih)/2:color=black",
            string.format("setdar=%f[video_padded]", target_output_aspect_ratio)
        }, ",")

    elseif is_pal_dvd and sar > 1.05 then
        -- PAL 16:9 DVD
        -- 縦解像度はそのまま576で、横幅をSARで補正し、16:9のDARに設定
        overlay_w = math.floor(src_w * sar + 0.5)
        overlay_h = src_h
        video_filter_chain = string.format(
            "[vid1]scale=%d:%d,setsar=1,setdar=%f[video_padded]",
            overlay_w, overlay_h, target_output_aspect_ratio
        )

    else
        -- その他一般処理
        if math.abs(dar - target_output_aspect_ratio) < 0.01 then
            overlay_w = src_w
            overlay_h = src_h
            video_filter_chain = string.format("[vid1]setdar=%f,setsar=1[video_padded]", target_output_aspect_ratio)
        else
            overlay_h = src_h
            local target_w = math.floor(overlay_h * target_output_aspect_ratio + 0.5)
            local actual_w = math.floor(overlay_h * dar + 0.5)
            local pad_x = math.max(0, target_w - actual_w)
            local pad_left = math.floor(pad_x / 2 + 0.5)
            overlay_w = target_w

            video_filter_chain = table.concat({
                string.format("[vid1]setdar=%f", dar),
                string.format("scale=%d:%d", actual_w, overlay_h),
                "setsar=1",
                string.format("pad=%d:%d:%d:0:color=black[video_padded]", overlay_w, overlay_h, pad_left)
            }, ",")
        end
    end

    local lavfi = table.concat({
        video_filter_chain .. ";",
        string.format("movie=%s,scale=%d:%d,format=rgba[image_scaled];", image_path:gsub("\\", "/"), overlay_w, overlay_h),
        "[video_padded][image_scaled]overlay=0:0:format=auto[vo]"
    })

    return lavfi
end

local function apply_overlay()
    if busy then
        mp.osd_message("処理中のためオーバーレイ切替不可")
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
    local last_path = mp.get_property("path")
    local last_time = mp.get_property_number("time-pos", 0)

    if mp.get_property_bool("pause") then
        mp.set_property_bool("pause", false)
    end

    mp.set_property("lavfi-complex", build_lavfi())
    overlay_active = true
    mp.osd_message("オーバーレイ表示：ON")
    busy = false
end

local function remove_overlay()
    if busy then
        mp.osd_message("処理中のためオーバーレイ切替不可")
        return
    end

    busy = true
    mp.set_property("lavfi-complex", "")
    overlay_active = false

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

mp.add_key_binding("Alt+I", "overlay-toggle", function()
    if overlay_active then
        remove_overlay()
    else
        apply_overlay()
    end
end)

mp.register_event("file-loaded", function()
    if overlay_active and not busy then
        remove_overlay()
    end
end)

mp.register_event("shutdown", function()
    if overlay_active then
        remove_lock()
    end
end)

mp.register_event("end-file", function()
    if overlay_active then
        remove_lock()
    end
end)
