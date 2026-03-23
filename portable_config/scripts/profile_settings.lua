local config_path = mp.command_native({"expand-path", "~~/selected_profile.conf"})

-- 設定を保存する関数（OFF は保存しない）
local function save_profile(profile)
    if profile == "OFF" then
        -- 空にして保存しない扱い
        local file = io.open(config_path, "w")
        if file then
            file:write("")
            file:close()
        end
        return
    end
    local file = io.open(config_path, "w")
    if file then
        file:write(profile)
        file:close()
    end
end

-- 設定を読み込む関数
local function load_profile()
    local file = io.open(config_path, "r")
    if file then
        local profile = file:read("*all")
        file:close()
        if profile == "" or profile == "OFF" then
            return nil
        end
        return profile
    end
    return nil
end

-- プロファイルを適用する関数
local function apply_profile(profile)
    mp.command("apply-profile " .. profile)
    mp.osd_message("プロファイル適用: " .. profile)
    save_profile(profile)
end

-- input.conf からプロファイル適用時に保存する
mp.register_script_message("set-profile", apply_profile)

-- 起動時に保存されたプロファイルを適用
local saved_profile = load_profile()
if saved_profile then
    apply_profile(saved_profile)
end
