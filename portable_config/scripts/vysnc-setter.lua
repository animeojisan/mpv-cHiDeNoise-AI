local utils = require("mp.utils")
local conf_path = mp.find_config_file("mpv.conf")
local key_pattern = "^%s*video%-sync%s*=%s*"

local function write_value_to_mpv_conf(value)
    if not conf_path then
        mp.msg.error("mpv.conf not found")
        return
    end

    local lines = {}
    local replaced = false

    local f = io.open(conf_path, "r")
    if f then
        for line in f:lines() do
            -- video-sync の行で、= 前後に空白があっても OK
            if line:match(key_pattern) then
                table.insert(lines, "video-sync=" .. value) -- 空白なしで統一
                replaced = true
            else
                table.insert(lines, line)
            end
        end
        f:close()
    end

    if not replaced then
        table.insert(lines, "video-sync=" .. value)
    end

    local tmp = conf_path .. ".tmp"
    local wf = io.open(tmp, "w")
    if not wf then
        mp.msg.error("Failed to write to temporary mpv.conf")
        return
    end

    for _, line in ipairs(lines) do
        wf:write(line .. "\n")
    end
    wf:close()

    os.remove(conf_path)
    os.rename(tmp, conf_path)

    mp.osd_message("video-sync=" .. value .. " を保存しました（再起動で有効）")
end

mp.register_script_message("set-video-sync", function(value)
    if value then
        write_value_to_mpv_conf(value)
    else
        mp.msg.warn("set-video-sync に値がありません")
    end
end)
