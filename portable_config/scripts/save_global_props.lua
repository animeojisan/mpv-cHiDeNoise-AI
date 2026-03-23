--[[

SOURCE_ https://github.com/zenwarr/mpv-config/blob/master/scripts/remember-props.lua
COMMIT_ 20220811 03cfc0e39682a73d9d24a6e01a3c02716a019d1d
ファイル_ save_global_props.conf

グローバル・プロパティの変更を記録し、次回のアプリケーション起動時の復元をサポートし、対応するデータを対応するファイルに保存する。 saved-props.json
(--save-position-on-quitオプションはファイル固有の属性を保存し、--watch-later-optionsで保存された属性と衝突しない)

使用可能なショートカットの例（input.confに記述）：
 <KEY>   script-message-to save_global_props clean_data   # 記録データのクリア

]]

local mp = require "mp"
mp.options = require "mp.options"
mp.utils = require "mp.utils"

local opt = {
	load = true,

	save_mode = 1,                     -- <1|2>
	props     = "volume,mute",
	dup_block = false,
	cache_dir = "~~/"
}
mp.options.read_options(opt)

if opt.load == false then
	mp.msg.info("スクリプトは初期状態では無効になっています")
	return
end
-- 理由：最初のバージョンで--watch-later-optionsを追加した
local min_major = 0
local min_minor = 34
local min_patch = 0
local mpv_ver_curr = mp.get_property_native("mpv-version", "unknown")
local function incompat_check(full_str, tar_major, tar_minor, tar_patch)
	if full_str == "unknown" then
		return true
	end

	local clean_ver_str = full_str:gsub("^[^%d]*", "")
	local major, minor, patch = clean_ver_str:match("^(%d+)%.(%d+)%.(%d+)")
	major = tonumber(major)
	minor = tonumber(minor)
	patch = tonumber(patch or 0)
	if major < tar_major then
		return true
	elseif major == tar_major then
		if minor < tar_minor then
			return true
		elseif minor == tar_minor then
			if patch < tar_patch then
				return true
			end
		end
	end

	return false
end
if incompat_check(mpv_ver_curr, min_major, min_minor, min_patch) then
	mp.msg.warn("当前mpv版本 (" .. (mpv_ver_curr or "未知") .. ") 低于 " .. min_major .. "." .. min_minor .. "." .. min_patch .. "，已终止脚本。")
	return
end

local function split(inputstr, sep)
	local result = {}
	for str in string.gmatch(inputstr, "([^" .. sep .. "]+)") do
		table.insert(result, str)
	end
	return result
end

opt.props = split(opt.props, ",")
local watch_later_opts = split(mp.get_property("watch-later-options"), ",")
local dup_opts = false

local function check_dup(table1, table2)
	for _, value1 in ipairs(table1) do
		for _, value2 in ipairs(table2) do
			if value1 == value2 then
				dup_opts = true
				mp.msg.warn("存在与 --watch-later-options 重合的项目： " .. value1)
			end
		end
	end
end

check_dup(opt.props, watch_later_opts)

if dup_opts and opt.dup_block then
	mp.msg.warn("自動的に無効化されたグローバル・プロパティのリカバリ保存")
	return
end

local cleaned = false
local data_file_path = (mp.command_native({"expand-path", opt.cache_dir .. "saved-props.json"}))

local function read_data_file()
	local json_file = io.open(data_file_path, "a+")
	local result = mp.utils.parse_json(json_file:read("*all"))
	if result == nil then
		result = {}
	end
	json_file:close()
	return result
end

local saved_data = read_data_file()

local function save_data_file()
	if cleaned then
		mp.msg.verbose("プロパティ記録のクリアによる保存機能の中断")
		return
	end
	local file = io.open(data_file_path, "w+")
	if file == nil then
		return
	end
	local content, ret = mp.utils.format_json(saved_data)
	if ret ~= error and content ~= nil then
		file:write(content)
	end
	file:close()
end

local function clean_data_file()
	local file = io.open(data_file_path, "w+")
	if file == nil then
		return
	end
	local content = ""
	file:write(content)
	file:close()
	cleaned = true
	mp.msg.info("グローバル・プロパティ保存リカバリー・クリア・キャッシュ")
	mp.osd_message("記録されたプロパティのクリーンアップ/再起動推奨 mpv", 2)
end

local function init()
	for _, prop_name in ipairs(opt.props) do
		local saved_value = saved_data[prop_name]
		if saved_value ~= nil then
			mp.set_property_native(prop_name, saved_value)
		end
		if opt.save_mode == 2 then
			mp.observe_property(prop_name, "native", function(_, prop_value)
				saved_data[prop_name] = mp.get_property_native(prop_name)
				save_data_file()
			end)
		end
	end
end

init()
mp.msg.info("グローバル属性保存リカバリーモードの実行" .. opt.save_mode)

if opt.save_mode == 1 then
	mp.register_event("shutdown", function()
		for _, prop_name in ipairs(opt.props) do
			saved_data[prop_name] = mp.get_property_native(prop_name)
			save_data_file()
		end
	end)
end

mp.register_script_message("clean_data", clean_data_file)
