local o = {
    -- 自動でログ保存（falseなら save_bind で手動保存）
    auto_save = true,
    save_bind = "",
    -- このパーセント以上再生したら「見終わった」とみなし、auto_save のとき delete=true にする境界
    auto_save_skip_past = 100,
    -- 同一フォルダの最新だけ表示するか（メニュー側用）
    hide_same_dir = false,
    -- --idle のとき自動で履歴表示（OSD用・不要なら false にしてOK）
    auto_run_idle = true,
    -- 再生中ファイルを切り替える前に watch-later を書くか
    write_watch_later = true,
    -- もともと履歴表示に使っていたキー（今回はバインドしない）
    display_bind = "`",
    -- マウス操作（今回キーは張らないので実質無効）
    mouse_controls = true,
    -- 履歴ファイル名（mpv config dir 配下）
    log_path = "history.log",
    -- 日付フォーマット
    date_format = "%d/%m/%y %X",
    -- true なら media-title ではなくパスをメニューに出す
    show_paths = false,
    -- 長いファイル名をカットするか
    slice_longfilenames = false,
    slice_longfilenames_amount = 100,
    -- パスの最後のファイル名だけ表示するか
    split_paths = true,
    -- OSD用フォント設定
    font_scale = 50,
    border_size = 0.7,
    -- ハイライト色（BGR hex）
    hi_color = "H46CFFF",
    -- 省略記号の表示
    ellipsis = false,
    -- mpv-menu / uosc のサブメニューに出す最大件数
    list_show_amount = 20,
}
(require "mp.options").read_options(o, _, function() end)
local utils = require("mp.utils")
o.log_path = utils.join_path(mp.find_config_file("."), o.log_path)

local cur_title, cur_path
local uosc_available = false
local is_windows = package.config:sub(1,1) == "\\"

local function esc_string(str)
    return str:gsub("([%p])", "%%%1")
end

local function is_protocol(path)
    return type(path) == 'string' and path:match('^%a[%a%d-_]+://') ~= nil
end

-- normalize-path を使ってフルパスにする（ただし "-" はそのまま）
local function normalize(path)
    if normalize_path ~= nil then
        if normalize_path then
            path = mp.command_native({"normalize-path", path})
        else
            local directory = mp.get_property("working-directory", "")
            path = utils.join_path(directory, path:gsub('^%.[\\/]',''))
            if is_windows then path = path:gsub("\\", "/") end
        end
        return path
    end

    normalize_path = false

    local commands = mp.get_property_native("command-list", {})
    for _, command in ipairs(commands) do
        if command.name == "normalize-path" then
            normalize_path = true
            break
        end
    end
    return normalize(path)
end

-- from http://lua-users.org/wiki/LuaUnicode
local UTF8_PATTERN = '[%z\1-\127\194-\244][\128-\191]*'

local function utf8_sub(s, i, j)
    local t = {}
    local idx = 1
    for match in s:gmatch(UTF8_PATTERN) do
        if j and idx > j then break end
        if idx >= i then t[#t + 1] = match end
        idx = idx + 1
    end
    return table.concat(t)
end

local function split_ext(filename)
    local idx = filename:match(".+()%.%w+$")
    if idx then
        filename = filename:sub(1, idx - 1)
    end
    return filename
end

local function strip_title(str)
    if o.slice_longfilenames and str:len() > o.slice_longfilenames_amount + 5 then
        str = utf8_sub(str, 1, o.slice_longfilenames_amount) .. "..."
    end
    return str
end

-- 拡張子が無くても安全
local function get_ext(path)
    if is_protocol(path) then
        local proto = path:match("^(%a[%w.+-]-)://")
        return proto and proto:upper() or ""
    else
        local ext = path:match(".+%.(%w+)$")
        return ext and ext:upper() or ""
    end
end

local function get_dir(path)
    if is_protocol(path) then
        return path
    end
    local dir, filename = utils.split_path(path)
    return dir
end

local function get_filename(item)
    if is_protocol(item.path) then
        return item.title
    end
    local dir, filename = utils.split_path(item.path)
    return filename
end

-- path=="-" のときは normalize せずそのまま "-" を返す
local function get_path()
    local path = mp.get_property("path")
    local title = (mp.get_property("media-title") or ""):gsub("\"", "")
    if not path then return end
    if path == "-" then
        return title, "-"
    end
    if is_protocol(path) then
        return title, path
    else
        local npath = normalize(path)
        return title, npath
    end
end

-- ==== ログ読み書き ====

-- 安全版: func(line) が nil を返した行は捨てる / ファイルなしなら {} を返す
local function read_log(func)
    local f = io.open(o.log_path, "r")
    if not f then return {} end
    local list = {}
    for line in f:lines() do
        if not line:match("^%s*$") then
            if func then
                local v = func(line)
                if v ~= nil then
                    table.insert(list, v)
                end
            else
                table.insert(list, line)
            end
        end
    end
    f:close()
    return list
end

-- 「履歴として意味のない '-' エントリ」を判定して弾く
local function is_capture_dummy(title, path)
    if not title or not path then return false end
    if title ~= "-" then return false end
    -- パスが "-" そのもの、または末尾が "/-" or "\-"
    if path == "-" then return true end
    if path:match("[\\/]%-$") then return true end
    return false
end

local function read_log_table()
    return read_log(function(line)
        local t, p = line:match('^.-"(.-)" | (.*)$')
        if not t or not p then return nil end
        -- ★ 既存の '-' ダミー行はここで捨てる（メニューに出さない）
        if is_capture_dummy(t, p) then
            return nil
        end
        return {title = t, path = p}
    end)
end

local function table_reverse(tbl)
    local reversed = {}
    for i = 1, #tbl do
        reversed[#tbl - i + 1] = tbl[i]
    end
    return reversed
end

local function hide_same_dir(content)
    local lists = {}
    local dir_cache = {}
    for i = 1, #content do
        local dirname = get_dir(content[#content-i+1].path)
        if not dir_cache[dirname] then
            table.insert(lists, content[#content-i+1])
        end
        if dirname ~= "." then
            dir_cache[dirname] = true
        end
    end
    return table_reverse(lists)
end

-- ==== mpv-menu / dyn_menu 連携 ====

local dyn_menu = {
    ready = false,
    type = 'submenu',
    submenu = {}
}

local function update_dyn_menu_items()
    local menu = {}
    local lists = read_log_table()
    if not lists or not lists[1] then
        return
    end
    if o.hide_same_dir then
        lists = hide_same_dir(lists)
    end
    local length = math.min(#lists, o.list_show_amount)
    for i = 1, length do
        local item = lists[#lists - i + 1]
        menu[#menu + 1] = {
            title = string.format('%s\t%s',
                o.show_paths and strip_title(split_ext(get_filename(item)))
                    or strip_title(split_ext(item.title)),
                get_ext(item.path)
            ),
            cmd = string.format("loadfile '%s'", item.path),
        }
    end
    dyn_menu.submenu = menu
    mp.commandv('script-message-to', 'dyn_menu', 'update', 'recent', utils.format_json(dyn_menu))
end

-- ==== ログ書き込み ====

local function write_log(delete)
    -- DVD VIDEO_TS フォルダ / ドライブルートはスキップ
    if cur_path and (
        cur_path:match("^[A-Za-z]:[\\/][Vv][Ii][Dd][Ee][Oo]_[Tt][Ss][\\/]?$") or
        cur_path:match("^[A-Za-z]:[\\/]?$")
    ) then
        mp.msg.info("Skipping logging of DVD related directory: " .. cur_path)
        return
    end

    -- stdin/capture 用の '-' は履歴不要
    if is_capture_dummy(cur_title, cur_path) then
        mp.msg.info("Skipping logging of capture dummy entry '-'")
        return
    end

    if not cur_path
        or cur_path:match("^dvd://")
        or cur_path:match("^bd://")
        or cur_path:match("^dvb://")
        or cur_path:match("^cdda://") then
        return
    end

    -- 既存ログから同じパスの行だけ削除してから末尾に追記
    local content = read_log(function(line)
        local pattern = "| " .. esc_string(cur_path) .. "$"
        if line:match(pattern) then
            return nil
        else
            return line
        end
    end)

    local f = io.open(o.log_path, "w+")
    if content then
        for i = 1, #content do
            f:write(("%s\n"):format(content[i]))
        end
    end
    if not delete then
        f:write(("[%s] \"%s\" | %s\n"):format(os.date(o.date_format), cur_title or "", cur_path))
    end
    f:close()
    if dyn_menu.ready then
        update_dyn_menu_items()
    end
end

-- ==== 最終再生項目をロード（必要なら他スクリプトから呼べる） ====

local function play_last()
    local lists = read_log_table()
    if not lists or not lists[1] then
        return
    end
    mp.commandv("loadfile", lists[#lists].path, "replace")
end

-- ==== OSD 用の簡易表示（キーは張らない・idle時などに使う用） ====

local function display_list()
    local list = read_log_table()
    if not list or not list[1] then
        mp.osd_message("Log empty")
        return
    end
    if o.hide_same_dir then
        list = hide_same_dir(list)
    end
    if uosc_available then
        -- uosc には open-menu で渡す
        local menu = {
            type = 'recent_menu',
            title = 'Recent',
            items = { { title = 'Nothing here', value = 'ignore' } },
        }
        local length = math.min(#list, o.list_show_amount)
        for i = 1, length do
            local item = list[#list - i + 1]
            menu.items[i] = {
                title = o.show_paths and strip_title(split_ext(get_filename(item)))
                        or strip_title(split_ext(item.title)),
                hint = get_ext(item.path),
                value = { "loadfile", item.path, "replace" },
            }
        end
        mp.commandv('script-message-to', 'uosc', 'open-menu', utils.format_json(menu))
        return
    end

    -- uosc が無い場合は、単に OSD に一覧だけ表示（選択はなし）
    local lines = {}
    local length = math.min(#list, o.list_show_amount)
    for i = 1, length do
        local item = list[#list - i + 1]
        local p
        if o.show_paths then
            if o.split_paths or is_protocol(item.path) then
                p = get_filename(item)
            else
                p = item.path or ""
            end
        else
            p = item.title or item.path or ""
        end
        table.insert(lines, strip_title(p))
    end
    mp.osd_message(table.concat(lines, "\n"), 5)
end

-- ==== idle 時の自動表示（不要なら o.auto_run_idle=false で抑止） ====

local function run_idle()
    mp.observe_property("idle-active", "bool", function(_, v)
        if o.auto_run_idle and v and not uosc_available then
            display_list()
        end
    end)
end

-- === mpv-menu-plugin 連携 ===

mp.register_script_message('menu-ready', function()
    dyn_menu.ready = true
    update_dyn_menu_items()
end)

-- uosc 検出
mp.register_script_message('uosc-version', function(version)
    uosc_available = true
end)
mp.commandv('script-message-to', 'uosc', 'get-version', mp.get_script_name())

mp.observe_property("display-hidpi-scale", "native", function(_, scale)
    if scale then
        run_idle()
    end
end)

-- ==== イベントハンドラ ====

mp.register_event("file-loaded", function()
    cur_title, cur_path = get_path()
end)

-- "end-file" だと再生位置がリセットされているので on_unload フックでログ
mp.add_hook("on_unload", 9, function ()
    if not o.auto_save then return end
    local pos = mp.get_property("percent-pos")
    if not pos then return end
    if tonumber(pos) <= o.auto_save_skip_past then
        write_log(false)
    else
        write_log(true)
    end
end)

-- ★ ここでは一切 key_binding を追加しない ★
-- （display_list / play_last を呼びたいときは、別スクリプトから script-message などで呼ぶ前提）
