local mp = require "mp"
local utils = require "mp.utils"

mp.register_script_message("open-config-folder", function()
    -- open_folder.exe が portable_config の直下にある場合
    -- "~~/" は portable_config フォルダのパスを表します
    local exe_path = mp.command_native({ "expand-path", "~~/open_folder.exe" })

    -- 以前のデバッグメッセージがあればそのまま残しておくと便利です
    -- mp.osd_message("スクリプトがトリガーされました！")

    if utils.file_info(exe_path) then
        -- mp.osd_message("open_folder.exeが見つかりました: " .. exe_path)

        local config_folder_path = mp.command_native({ "expand-path", "~~/" }) -- portable_config フォルダのパス

        mp.command_native_async({
            name = "subprocess",
            args = { exe_path, config_folder_path },
            playback_only = false,
        }, function(success, result, error)
            if success then
                -- mp.osd_message("設定フォルダを開くコマンドが実行されました。")
            else
                mp.osd_message("設定フォルダを開けませんでした: " .. (error or "unknown error"))
            end
        end)
    else
        mp.osd_message("open_folder.exe が見つかりません: " .. exe_path)
    end
end)