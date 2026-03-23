-- disable_subs.lua
local mp = require 'mp'

mp.register_event("file-loaded", function()
    mp.set_property("sid", "no") -- 字幕IDをなしに設定
end)