local utils = require("mp.utils")
local conf_path = mp.find_config_file("mpv.conf")

-- mpv.conf の指定 key=value を安全に更新
local function update_conf_value(path, key, value)
    if not path then
        mp.msg.error("mpv.conf not found")
        return
    end

    local lines = {}
    local found = false

    local f = io.open(path, "r")
    if f then
        for line in f:lines() do
            if line:match("^%s*" .. key .. "%s*=") then
                table.insert(lines, key .. "=" .. value)
                found = true
            else
                table.insert(lines, line)
            end
        end
        f:close()
    end

    if not found then
        table.insert(lines, key .. "=" .. value)
    end

    local wf = io.open(path, "w")
    if not wf then
        mp.msg.error("Failed to write to mpv.conf")
        return
    end
    for _, line in ipairs(lines) do
        wf:write(line .. "\n")
    end
    wf:close()
end

-- script-message 経由で hwdec を変更・保存
mp.register_script_message("set-hwdec", function(mode)
    if not mode or mode == "" then
        mp.msg.warn("No hwdec mode provided.")
        return
    end
    mp.set_property("hwdec", mode)
    update_conf_value(conf_path, "hwdec", mode)
    mp.osd_message("hwdec set to: " .. mode)
end)
