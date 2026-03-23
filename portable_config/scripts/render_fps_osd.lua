-- render_fps_osd.lua
-- 点滅しない Render FPS 表示（最小3行版）
-- stats.lua を一切トグル/呼び出ししない安全設計
--
-- 表示:
--   1) Render FPS (tick est)
--   2) Input FPS
--   3) Display FPS
--
-- 起動時は表示しない（input.conf から script-message-to で呼ぶ想定）

local mp = require "mp"

local enabled = false
local ov = mp.create_osd_overlay("ass-events")

-- tick 기반の簡易レンダFPS推定
local tick_count = 0
local last_tick = 0
local last_time = nil

local function pn(name)
    return mp.get_property_number(name)
end

local function reset_counters()
    tick_count = 0
    last_tick = 0
    last_time = nil
end

-- tick は「何かが更新されたレンダサイクル」に近い頻度で来る想定
mp.register_event("tick", function()
    if enabled then
        tick_count = tick_count + 1
    end
end)

local function build_text(rfps)
    local input_fps = pn("container-fps") or pn("fps") or 0
    local disp_fps  = pn("display-fps")

    local lines = {}

    if rfps and rfps > 0 then
        lines[#lines+1] = string.format("Render FPS (tick est): %.1f", rfps)
    else
        lines[#lines+1] = "Render FPS (tick est): --"
    end

    if input_fps and input_fps > 0 then
        lines[#lines+1] = string.format("Input FPS: %.3f", input_fps)
    else
        lines[#lines+1] = "Input FPS: --"
    end

    if disp_fps then
        lines[#lines+1] = string.format("Display FPS: %.1f", disp_fps)
    else
        lines[#lines+1] = "Display FPS: --"
    end

    return table.concat(lines, "\\N")
end

local function update_osd()
    if not enabled then
        ov:remove()
        return
    end

    local now = mp.get_time()
    if not last_time then
        last_time = now
        last_tick = tick_count
    end

    local dt = now - last_time
    local rfps = nil

    if dt and dt > 0.2 then
        local dframes = tick_count - last_tick
        rfps = dframes / dt
        last_time = now
        last_tick = tick_count
    end

    local text = build_text(rfps)

    -- 左上表示
    ov.data = string.format("{\\an7\\fs24\\bord2}%s", text)
    ov:update()
end

local timer = mp.add_periodic_timer(0.5, update_osd)
timer:stop()

local function enable()
    enabled = true
    reset_counters()
    timer:resume()
    update_osd()
end

local function disable()
    enabled = false
    timer:stop()
    ov:remove()
end

local function toggle()
    if enabled then disable() else enable() end
end

-- file-loaded でカウンタ初期化
mp.register_event("file-loaded", function()
    if enabled then
        reset_counters()
        update_osd()
    end
end)

-- 公開メッセージ
mp.register_script_message("toggle", toggle)
mp.register_script_message("on", enable)
mp.register_script_message("off", disable)

-- キーバインドは強制しない
-- input.conf で割り当て推奨:
-- F8 script-message-to render_fps_osd toggle
