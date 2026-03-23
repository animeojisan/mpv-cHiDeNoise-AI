local ffi = require("ffi")
local utils = require("mp.utils")
local msg = require("mp.msg")

-- ファイルパス
local config_path = mp.command_native({"expand-path", "~~/mpv.conf"})
local state_path = mp.command_native({"expand-path", "~~/script-opts/pos_save_enabled.json"})

local autofit_prefix = "autofit="
local geometry_prefix = "geometry="

-- Win32 API 定義
ffi.cdef[[
typedef void* HWND;
typedef struct { long left; long top; long right; long bottom; } RECT;
HWND GetForegroundWindow(void);
int GetWindowRect(HWND hWnd, RECT* lpRect);
int GetSystemMetrics(int nIndex);
]]

local SM_CXSCREEN = 0
local SM_CYSCREEN = 1

-- ウィンドウ位置取得
local function get_window_position()
    local hwnd = ffi.C.GetForegroundWindow()
    if hwnd == nil then return nil, nil end
    local rect = ffi.new("RECT[1]")
    if ffi.C.GetWindowRect(hwnd, rect) == 0 then return nil, nil end
    return rect[0].left, rect[0].top
end

-- mpv.conf の autofit と geometry を更新
local function update_config(width, height, x, y)
    local autofit_line = autofit_prefix .. width .. "x" .. height
    local geometry_line = geometry_prefix .. "+" .. x .. "+" .. y

    local lines = {}
    local autofit_found = false
    local geometry_found = false

    local file = io.open(config_path, "r")
    if file then
        for line in file:lines() do
            local trimmed = line:match("^%s*(.-)%s*$")
            if trimmed:find("^" .. autofit_prefix) then
                table.insert(lines, autofit_line)
                autofit_found = true
            elseif trimmed:find("^" .. geometry_prefix) then
                table.insert(lines, geometry_line)
                geometry_found = true
            else
                table.insert(lines, line)
            end
        end
        file:close()
    end

    if not autofit_found then table.insert(lines, autofit_line) end
    if not geometry_found then table.insert(lines, geometry_line) end

    local out = io.open(config_path, "w")
    if out then
        for _, line in ipairs(lines) do
            out:write(line .. "\n")
        end
        out:close()
        msg.info("mpv.conf 更新完了: " .. autofit_line .. " / " .. geometry_line)
    else
        msg.error("mpv.conf の書き込みに失敗")
    end
end

-- ON/OFF状態管理
local enabled = false

local function save_enabled()
    local f = io.open(state_path, "w+")
    if f then
        f:write(utils.format_json({enabled = enabled}))
        f:close()
        msg.info("自動保存状態を保存: " .. tostring(enabled))
    else
        msg.error("状態ファイル書き込み失敗")
    end
end

local function load_enabled()
    local f = io.open(state_path, "r")
    if f then
        local content = f:read("*all")
        f:close()
        local data = utils.parse_json(content)
        if data and data.enabled ~= nil then
            enabled = data.enabled
            msg.info("自動保存状態を読み込み: " .. tostring(enabled))
            return
        end
    end
    -- ファイルなしまたは読み込み失敗なら初回起動とみなしONに設定
    enabled = true
    msg.info("状態ファイルなしのため初回起動として enabled = true をセット")
    save_enabled()
end

-- shutdown時の処理
local function shutdown_handler()
    if not enabled then
        msg.info("自動保存無効のためスキップ")
        return
    end
    local w = mp.get_property_number("osd-width")
    local h = mp.get_property_number("osd-height")
    local x, y = get_window_position()
    if w and h and x and y then
        update_config(w, h, x, y)
    else
        msg.warn("ウィンドウ情報取得失敗。保存スキップ")
    end
end

-- 画面中央座標を計算してmpv.confに書き込む
local function move_window_center_in_config()
    local screen_w = ffi.C.GetSystemMetrics(SM_CXSCREEN)
    local screen_h = ffi.C.GetSystemMetrics(SM_CYSCREEN)
    local win_w = mp.get_property_number("osd-width") or 800
    local win_h = mp.get_property_number("osd-height") or 600

    local center_x = math.floor((screen_w - win_w) / 2)
    local center_y = math.floor((screen_h - win_h) / 2)

    update_config(win_w, win_h, center_x, center_y)
    msg.info(string.format("自動保存OFF：mpv.confのgeometryを中央位置に更新: +%d+%d", center_x, center_y))
end

-- 起動時に状態読み込み、shutdownイベント登録制御
load_enabled()
if enabled then
    mp.register_event("shutdown", shutdown_handler)
else
    msg.info("自動保存はOFFのためshutdown登録なし")
end

-- ON/OFF切替用 script-message 登録
mp.register_script_message("toggle-pos-save", function()
    enabled = not enabled
    save_enabled()
    mp.osd_message(enabled and "💾 ウィンドウ位置/サイズ状態を記憶: ON" or "❌ ウィンドウ位置/サイズ状態を記憶: OFF", 2)

    if enabled then
        mp.register_event("shutdown", shutdown_handler)
    else
        mp.unregister_event("shutdown", shutdown_handler)
        move_window_center_in_config()  -- OFF時は中央位置に書き換え
    end
end)
