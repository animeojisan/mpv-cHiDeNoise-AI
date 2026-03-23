local log_path = mp.command_native({"expand-path", "~~/_cache/history.log"})

mp.register_script_message("clear-history", function()
    local f = io.open(log_path, "w")  -- 「w」で空の状態に上書き
    if f then
        f:close()
        mp.osd_message("視聴履歴をクリアしました")
    else
        mp.osd_message("history.log のクリアに失敗しました")
    end
end)
