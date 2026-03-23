-- dshow_cmd_launch.lua (v18 portable: NO utils.subprocess)
-- Your mpv build returns status=-1 for utils.subprocess() even for cmd.exe,
-- so device listing must NOT rely on utils.subprocess.
--
-- Solution:
--   Use mp.commandv("run", ...) to launch a tiny .cmd that writes JSON to a file,
--   then poll that file and parse it. This works on builds where "run" works but
--   "utils.subprocess" can't start processes.
--
-- Assumption: dshowCap.exe is in the same folder as mpv.exe (portable).
-- No script-opts required.

local mp      = require("mp")
local utils   = require("mp.utils")
local msg     = require("mp.msg")
local options = require("mp.options")

local script_name = mp.get_script_name()

local o = {
  -- ログを保存するかどうか（既定: false = 保存しない）
  -- true にすると下記 log 系パスへ保存します。
  enable_logs = false,
  -- If empty, we try to use mpv's working-directory (portable typically OK)
  dshowcap_cwd = "",

  dshowcap_exe = "dshowCap.exe",

  -- Prefer NTSC-like (exclude PAL 25/50/576) when dshowCap auto-selects a format
  prefer_ntsc = true,

  video_device = "",
  audio_device = "",

  -- audio output mode for the selected audio device:
  --   system: play via DirectShow Audio Renderer inside dshowCap.exe (no child mpv)
  --   mpv   : spawn mpv (legacy)
  --   none  : no audio
  audio_out = "none",

  w = 1920,
  h = 1080,
  fps = 30,
  mp_format = "nv12",

  -- subtype で分岐（デバイス名に依存しない）
  video_subtype = "",
  video_compressed = false,
  -- 圧縮ストリームの場合に mpv(lavf) へ渡す demuxer-lavf-format
  -- 例: MJPG->mjpeg, H264->h264, HEVC->hevc
  video_lavf_format = "",

  -- OBS Virtual Camera は 1920x1080@60 の生パイプだと詰まりやすい環境があるため、
  -- 安定化のために軽いフォーマットへフォールバック（必要なら false に）
  obs_force_720p30 = false,
  obs_w = 1920,
  obs_h = 1080,
  obs_fps = 60,

  mpv_exe = "",

  dshowcap_log = "portable_config/cache/dshowCap.log",
  mpv_child_log = "portable_config/cache/mpv_child.log",

  debug_cmdline_file = "portable_config/cache/dshow_cmdline.txt",
  launch_cmd_file = "_dshow_launch.cmd",
  launch_run_log = "portable_config/cache/_dshow_launch_run.log",
  last_selection_file = "portable_config/cache/_dshow_last_selection.json",

  -- Listing helper files (written to working directory)
  list_cmd_video = "_dshow_list_video.cmd",
  list_cmd_audio = "_dshow_list_audio.cmd",
  list_out_video = "portable_config/cache/_dshow_list_video.json",
  list_out_audio = "portable_config/cache/_dshow_list_audio.json",
  list_err_video = "_dshow_list_video.err",
  list_err_audio = "_dshow_list_audio.err",
  list_debug_log = "portable_config/cache/_dshow_list.log",
  -- Default format query helper files
  fmt_cmd = "_dshow_fmt_video.cmd",
  fmt_out = "portable_config/cache/_dshow_fmt_video.json",
  fmt_err = "_dshow_fmt_video.err",


  -- Polling (seconds)
  list_poll_interval = 0.08,
  list_timeout = 5.0,

  mpv_child_msg_level = "all=info",
  extra_mpv_args = "",

  restart_self = true,
}

options.read_options(o, "dshow_cmd_launch")

----------------------------------------------------------------------
-- helpers
----------------------------------------------------------------------

local function trim(s)
  if s == nil then return "" end
  s = tostring(s)
  return (s:gsub("^%s+", ""):gsub("%s+$", ""))
end

local function strip_surrounding_quotes(s)
  s = trim(s)
  if #s >= 2 and s:sub(1,1) == '"' and s:sub(-1,-1) == '"' then
    s = s:sub(2, -2)
  end
  return s
end

local function q(s)
  s = strip_surrounding_quotes(s)
  return '"' .. s .. '"'
end

local function write_file(path, content, append)
  local mode = append and "ab" or "wb"
  local ok, err = pcall(function()
    local fh = assert(io.open(path, mode))
    fh:write(content)
    fh:close()
  end)
  if not ok then
    msg.warn("[dshow_cmd_launch] write failed: " .. tostring(err))
  end
end

local function read_file(path)
  local fh = io.open(path, "rb")
  if not fh then return nil end
  local data = fh:read("*a")
  fh:close()
  return data
end

local function rm_file(path)
  if not path or path == "" then return end
  os.remove(path)
end

local function log_dbg(line)
  if not o.enable_logs then return end
  local p = trim(o.list_debug_log)
  if p == "" then return end
  write_file(p, line .. "\r\n", true)
end

-- 既定では「ログを保存しない」
-- 以前の版で作成されたログ/errファイルが残っていても増え続けないように、
-- enable_logs=false の場合は対象ファイルを削除してから無効化します。
local function apply_log_policy()
  if o.enable_logs then return end

  local del = {
    o.dshowcap_log,
    o.mpv_child_log,
    o.debug_cmdline_file,
    o.launch_run_log,
    o.list_debug_log,
    o.fmt_err,
    o.list_err_video,
    o.list_err_audio,
  }
  for _, p in ipairs(del) do
    if trim(p) ~= "" then
      rm_file(p)
    end
  end

  -- disable outputs
  o.dshowcap_log = ""
  o.mpv_child_log = ""
  o.debug_cmdline_file = ""
  o.launch_run_log = ""
  o.list_debug_log = ""
  o.fmt_err = ""
  o.list_err_video = ""
  o.list_err_audio = ""
end

-- apply immediately after option parsing
apply_log_policy()

----------------------------------------------------------------------
-- portable defaults
----------------------------------------------------------------------

local function ensure_defaults()
  if trim(o.dshowcap_cwd) ~= "" then return end
  local wd = strip_surrounding_quotes(mp.get_property("working-directory", ""))
  if wd ~= "" then
    o.dshowcap_cwd = wd
    msg.info("[dshow_cmd_launch] auto dshowcap_cwd from working-directory=" .. wd)
  else
    o.dshowcap_cwd = "."
    msg.warn("[dshow_cmd_launch] auto dshowcap_cwd fell back to '.'")
  end
end

local function base_dir()
  ensure_defaults()
  return trim(o.dshowcap_cwd)
end

local function resolve_mpv_path()
  local p = strip_surrounding_quotes(o.mpv_exe)
  if p ~= "" then return p end

  local exe = strip_surrounding_quotes(mp.get_property("executable-path", ""))
  if exe ~= "" and (exe:match("^%a:[/\\]") or exe:match("^\\\\"))
  then return exe end

  return "mpv"
end

----------------------------------------------------------------------
-- persist last selection
----------------------------------------------------------------------

local function load_last_selection()
  local f = trim(o.last_selection_file)
  if f == "" then return end
  local data = read_file(f)
  if not data or data == "" then return end
  local t = utils.parse_json(data)
  if type(t) ~= "table" then return end
  if type(t.video_device) == "string" and t.video_device ~= "" then o.video_device = t.video_device end
  if type(t.audio_device) == "string" then o.audio_device = t.audio_device end
end

local function save_last_selection()
  local f = trim(o.last_selection_file)
  if f == "" then return end
  local t = { video_device = o.video_device, audio_device = o.audio_device }
  write_file(f, utils.format_json(t) .. "\r\n", false)
end

load_last_selection()

----------------------------------------------------------------------
-- Listing via .cmd + file polling
----------------------------------------------------------------------

local function build_list_cmd(cmd_name, out_name, err_name, which_flag)
  -- Avoid embedding non-ASCII paths by using %~dp0 (dir of this .cmd)
  -- and calling dshowCap.exe by name in that dir.
  local lines = {}
  lines[#lines+1] = "@echo off"
  lines[#lines+1] = "setlocal"
  lines[#lines+1] = "chcp 65001 >nul"
  lines[#lines+1] = "cd /d \"%~dp0\""
  lines[#lines+1] = "del /q " .. q(out_name) .. " 2>nul"
  if trim(err_name) ~= "" then
    lines[#lines+1] = "del /q " .. q(err_name) .. " 2>nul"
  end
  -- stderr はファイルには保存せず、NUL に捨てる
  lines[#lines+1] = q(o.dshowcap_exe) .. " " .. which_flag .. " --json --quiet 1> " .. q(out_name) .. " 2>nul"
  lines[#lines+1] = "exit /b %errorlevel%"
  lines[#lines+1] = "endlocal"
  write_file(cmd_name, table.concat(lines, "\r\n") .. "\r\n", false)
end

local function list_async(kind, on_ok, on_fail)
  ensure_defaults()

  local cmd_name, out_name, err_name, flag
  if kind == "video" then
    cmd_name = trim(o.list_cmd_video)
    out_name = trim(o.list_out_video)
    err_name = trim(o.list_err_video)
    flag = "--list-video"
  else
    cmd_name = trim(o.list_cmd_audio)
    out_name = trim(o.list_out_audio)
    err_name = trim(o.list_err_audio)
    flag = "--list-audio"
  end

  -- log
  log_dbg(("=== list(%s) %s ==="):format(kind, os.date("%Y-%m-%d %H:%M:%S")))
  log_dbg("working-directory=" .. strip_surrounding_quotes(mp.get_property("working-directory", "")))
  log_dbg("base_dir=" .. base_dir())
  log_dbg("cmd=" .. cmd_name .. " out=" .. out_name .. " err=" .. err_name)
  log_dbg("dshowcap_exe=" .. o.dshowcap_exe)

  -- Prepare cmd
  build_list_cmd(cmd_name, out_name, err_name, flag)

  -- Ensure old outputs are gone
  rm_file(out_name)
  rm_file(err_name)

  -- Run the cmd (relative path, in mpv working dir)
  mp.commandv("run", "cmd.exe", "/D", "/S", "/V:OFF", "/C", cmd_name)

  local deadline = mp.get_time() + tonumber(o.list_timeout)

  local poll_timer
  poll_timer = mp.add_periodic_timer(tonumber(o.list_poll_interval), function()
    local now = mp.get_time()
    local out = read_file(out_name)

    if out and #out > 0 then
      poll_timer:kill()
      local t = utils.parse_json(out)
      if type(t) ~= "table" then
        local err = read_file(err_name) or ""
        log_dbg("JSON parse failed. out=" .. tostring(out))
        log_dbg("err=" .. tostring(err))
        if on_fail then on_fail("JSON解析に失敗しました", err_name, out_name) end
        return
      end
      if on_ok then on_ok(t) end
      return
    end

    if now >= deadline then
      poll_timer:kill()
      local err = read_file(err_name) or ""
      log_dbg("timeout. err=" .. tostring(err))
      if on_fail then on_fail("列挙タイムアウト/失敗", err_name, out_name) end
    end
  end)
end

-- デフォルトビデオフォーマットを dshowCap.exe から取得して o.w/o.h/o.fps に反映
-- NOTE:
--   環境によって --get-default-format の JSON が stdout に出たり stderr に出たりします。
--   ただしポータブル配布向けに _dshow_fmt_video.err を残したくないため、stdout/stderr を out にまとめて保存します。
local function build_fmt_cmd(cmd_name, out_name, device)
  local lines = {}
  lines[#lines+1] = "@echo off"
  lines[#lines+1] = "setlocal"
  lines[#lines+1] = "chcp 65001 >nul"
  lines[#lines+1] = "cd /d \"%~dp0\""
  lines[#lines+1] = "del /q " .. q(out_name) .. " 2>nul"
  lines[#lines+1] =
    q(o.dshowcap_exe) ..
    " --device " .. q(device) ..
    (o.prefer_ntsc and " --prefer-ntsc" or " --prefer-pal") ..
    " --get-default-format --json --quiet 1> " .. q(out_name) .. "  2>&1"
  lines[#lines+1] = "endlocal"
  write_file(cmd_name, table.concat(lines, "\r\n") .. "\r\n", false)
end

local function query_default_format_async(device, on_ok, on_fail)
  ensure_defaults()

  local cmd_name = trim(o.fmt_cmd)
  local out_name = trim(o.fmt_out)

  if cmd_name == "" or out_name == "" then
    if on_fail then on_fail("fmt paths not set", "", out_name) end
    return
  end

  build_fmt_cmd(cmd_name, out_name, device)

  rm_file(out_name)

  mp.commandv("run", "cmd.exe", "/D", "/S", "/V:OFF", "/C", cmd_name)

  local deadline = mp.get_time() + tonumber(o.list_timeout)
  local poll_timer
  poll_timer = mp.add_periodic_timer(tonumber(o.list_poll_interval), function()
    local now = mp.get_time()
    local out = read_file(out_name)

    -- stdout/stderr は out にまとめて保存済み
    local combined = out or ""

    if combined ~= "" and combined:find("{", 1, true) then

      -- combined にはログ行と JSON 行が混ざる場合があるので、
      -- 最後の '{...}' 行だけを抜き出して JSON として解釈する
      local json_str = combined
      do
        -- 末尾側から最後の { ... } を抽出（途中にログが混ざってもOK）
        local last_brace = nil
        for s in string.gmatch(json_str, "{[^{}]*}") do
          last_brace = s
        end
        if last_brace then
          json_str = last_brace
        else
          -- フォールバック: 最終行が JSON のケース
          local last_line = nil
          for line in string.gmatch(json_str, "[^\r\n]+") do
            last_line = line
          end
          if last_line and last_line:find("{", 1, true) then
            local braced = last_line:match("{.*}")
            json_str = braced or last_line
          end
        end
      end
      local t = utils.parse_json(json_str)
      if type(t) ~= "table" then
        -- まだJSONが揃っていない可能性がある（stderrのログだけ先に出た等）ので
        -- タイムアウトまでポーリングを継続する
        return
      end
      poll_timer:kill()

      local changed = false
      if t.width ~= nil then
        local v = tonumber(t.width)
        if v and v > 0 then
          o.w = v
          changed = true
        end
      end
      if t.height ~= nil then
        local v = tonumber(t.height)
        if v and v > 0 then
          o.h = v
          changed = true
        end
      end
      if t.fps ~= nil then
        local v = tonumber(t.fps)
        if v and v > 0 then
          o.fps = v
          changed = true
        end
      elseif t.fps_num and t.fps_den then
        local num = tonumber(t.fps_num)
        local den = tonumber(t.fps_den)
        if num and den and den ~= 0 then
          o.fps = num / den
          changed = true
        end
      end

      -- subtype (例: NV12 / YUY2 / MJPG など)
      o.video_subtype = ""
      if t.subtype ~= nil and t.subtype ~= "" then
        o.video_subtype = tostring(t.subtype)
      end

      -- mpv の rawvideo 用フォーマットと「圧縮/非圧縮」判定
      -- ここは「デバイス名」ではなく、実際の subtype / mp_format で判断する。
      local function norm4cc(s)
        s = tostring(s or "")
        s = s:gsub("%s+", "")
        s = s:upper()
        return s
      end

      local function subtype_to_lavf_format(st)
        st = norm4cc(st)
        if st == "MJPG" or st == "MJPEG" then return "mjpeg" end
        if st == "H264" or st == "AVC1" or st == "X264" then return "h264" end
        if st == "H265" or st == "HEVC" or st == "HVC1" then return "hevc" end
        if st == "MP4V" or st == "MPEG4" then return "mpeg4" end
        if st == "MP2V" or st == "MPEG2" or st == "MPEG" then return "mpegvideo" end
        return ""
      end

      local function subtype_to_mp_format(st)
        st = norm4cc(st)
        -- best-effort fallback when dshowCap couldn't provide mp_format
        if st == "NV12" then return "nv12" end
        if st == "YUY2" then return "yuyv422" end
        if st == "UYVY" then return "uyvy422" end
        if st == "I420" then return "i420" end
        if st == "YV12" then return "yv12" end
        if st == "BGRA" then return "bgra" end
        if st == "RGBA" then return "rgba" end
        if st == "RGB24" then return "rgb24" end
        if st == "BGR24" then return "bgr24" end
        return ""
      end

      o.video_lavf_format = ""

      -- 強制: subtype が圧縮系なら mp_format の有無に関わらず lavf 経由にする
      -- (一部環境で mp_format が誤って入る/初期値が残ると rawvideo になって砂嵐になるため)
      local st_norm = norm4cc(o.video_subtype)
      local forced_lavf = subtype_to_lavf_format(st_norm)
      if forced_lavf ~= "" then
        o.video_compressed = true
        o.video_lavf_format = forced_lavf
        -- mp_format は参照されないが、念のため空に
        -- (rawvideo 側へ落ちないようにする)
        o.mp_format = o.mp_format or ""
        changed = true
      elseif t.mp_format ~= nil and t.mp_format ~= "" then
        -- NV12 / YUY2 / I420 など「生ピクセル」のとき
        o.mp_format = tostring(t.mp_format)
        o.video_compressed = false
        o.video_lavf_format = ""
        changed = true
      else
        -- mp_format が空：圧縮ストリーム or 未対応の raw かもしれない。
        -- まず subtype が raw ピクセルを示しているなら mp_format を補完して rawvideo を維持。
        local guess_mp = subtype_to_mp_format(o.video_subtype)
        if guess_mp ~= "" then
          o.mp_format = guess_mp
          o.video_compressed = false
          o.video_lavf_format = ""
          changed = true
        else
          -- それ以外は「圧縮ストリーム」とみなし lavf 経由にする。
          o.video_compressed = true
          o.video_lavf_format = subtype_to_lavf_format(o.video_subtype)
        end
      end

      if changed then
        log_dbg(string.format("default fmt: %dx%d @%s", o.w, o.h, tostring(o.fps)))
      end



-- ---- OBS Virtual Camera 安定化フォールバック ----
-- 1920x1080@60 の rawvideo をパイプすると、環境によって「最初の1フレームで停止」や
-- mpv が終了できない状態（stdin 読み待ちで固まる）になりやすいため、
-- まずは 1280x720@30 に落として帯域を半減させる。
if o.obs_force_720p30 then
  local dn = tostring(device or ""):lower()
  if dn:find("obs virtual camera", 1, true) then
    o.w = tonumber(o.obs_w) or o.w
    o.h = tonumber(o.obs_h) or o.h
    o.fps = tonumber(o.obs_fps) or o.fps
    o.mp_format = "nv12"
    changed = true
    log_dbg(string.format("OBS fallback fmt: %dx%d @%s", o.w, o.h, tostring(o.fps)))
  end
end

      if on_ok then on_ok(t) end
      return
    end

    if now >= deadline then
      poll_timer:kill()
      log_dbg("fmt timeout")
      if on_fail then on_fail("解像度取得タイムアウト/失敗", "", out_name) end
    end
  end)
end



----------------------------------------------------------------------
-- simple OSD menu (same style)
----------------------------------------------------------------------

local menu = { open=false, items={}, idx=1, on_pick=nil, title="", timer=nil }

local function menu_stop_timer()
  if menu.timer then menu.timer:kill(); menu.timer=nil end
end

local function menu_render()
  if not menu.open then return end
  local lines = {}
  if menu.title ~= "" then
    lines[#lines+1] = menu.title
    lines[#lines+1] = ""
    lines[#lines+1] = "(↑↓で選択 / Enterで決定 / Escでキャンセル)"
    lines[#lines+1] = ""
  end
  for i, it in ipairs(menu.items) do
    if i == menu.idx then lines[#lines+1] = ("▶ %s"):format(it)
    else lines[#lines+1] = ("  %s"):format(it) end
  end
  mp.osd_message(table.concat(lines, "\n"), 1.5)
end

local function menu_start_timer()
  menu_stop_timer()
  menu.timer = mp.add_periodic_timer(0.5, function()
    if menu.open then menu_render() else menu_stop_timer() end
  end)
end

local function menu_close()
  if not menu.open then return end
  menu.open = false
  menu_stop_timer()
  mp.remove_key_binding(script_name .. "_up")
  mp.remove_key_binding(script_name .. "_down")
  mp.remove_key_binding(script_name .. "_enter")
  mp.remove_key_binding(script_name .. "_kp_enter")
  mp.remove_key_binding(script_name .. "_esc")
  mp.osd_message("", 0.1)
end

local function menu_do_pick()
  local pick = menu.items[menu.idx]
  menu_close()
  if menu.on_pick then menu.on_pick(pick) end
end

local function menu_open(items, title, on_pick)
  menu_close()
  menu.open = true
  menu.items = items or {}
  menu.idx = 1
  menu.on_pick = on_pick
  menu.title = title or ""

  mp.add_forced_key_binding("UP", script_name .. "_up", function()
    if not menu.open or #menu.items == 0 then return end
    menu.idx = math.max(1, menu.idx - 1)
    menu_render()
  end)

  mp.add_forced_key_binding("DOWN", script_name .. "_down", function()
    if not menu.open or #menu.items == 0 then return end
    menu.idx = math.min(#menu.items, menu.idx + 1)
    menu_render()
  end)

  mp.add_forced_key_binding("ENTER", script_name .. "_enter", function()
    if not menu.open or #menu.items == 0 then return end
    menu_do_pick()
  end)

  mp.add_forced_key_binding("KP_ENTER", script_name .. "_kp_enter", function()
    if not menu.open or #menu.items == 0 then return end
    menu_do_pick()
  end)

  mp.add_forced_key_binding("ESC", script_name .. "_esc", function()
    if not menu.open then return end
    menu_close()
    mp.osd_message("キャンセル", 1.0)
  end)

  menu_render()
  menu_start_timer()
end

----------------------------------------------------------------------
-- Launch pipeline (restart mpv)
----------------------------------------------------------------------

local function build_pipeline(dshowcap_path, mpv_path)
  local left_parts = {
    q(dshowcap_path),
    "--device", q(o.video_device),
    "--w", tostring(o.w),
    "--h", tostring(o.h),
    "--fps", tostring(o.fps),
  }

  if o.prefer_ntsc then
    table.insert(left_parts, "--prefer-ntsc")
  else
    table.insert(left_parts, "--prefer-pal")
  end

    -- audio is optional (e.g. HDMI / GV-USB2 etc.)
  if trim(o.audio_device) ~= "" then
    table.insert(left_parts, "--audio-device")
    table.insert(left_parts, q(o.audio_device))
  end

-- Tell dshowCap how to output audio (system/mpv/none)
if trim(o.audio_out) ~= "" then
  table.insert(left_parts, "--audio-out")
  table.insert(left_parts, q(o.audio_out))
end


  -- mpv.exe のパスを dshowCap に渡す（音声 mpv 起動用）
  table.insert(left_parts, "--mpv-path")
  table.insert(left_parts, q(mpv_path))

  local left = table.concat(left_parts, " ")

  local right_parts = {
    q(mpv_path),
    "--profile=low-latency",
    "--untimed=yes",
    "--no-cache",
    "--demuxer-readahead-secs=0",
    "--no-audio",
  }

  -- 映像ストリームの扱い:
  --   * 通常: rawvideo (NV12 / YUY2 / YV12 等)
  --   * MJPG 等: ffmpeg(mjpeg) でデコード
  if o.video_compressed then
    -- 圧縮 MJPEG ストリームをそのまま流す
    table.insert(right_parts, "--demuxer=lavf")
    if trim(o.video_lavf_format) ~= "" then
      table.insert(right_parts, "--demuxer-lavf-format=" .. tostring(o.video_lavf_format))
    else
      -- subtype から demuxer を特定できない場合は lavf にプローブさせる（未知デバイス向け）
      -- うまくいかない環境があれば、dshowCap 側で mp_format を返す/固定する方が確実。
    end
    if o.fps and tonumber(o.fps) and tonumber(o.fps) > 0 then
      table.insert(right_parts, "--demuxer-lavf-o-add=framerate=" .. tostring(o.fps))
    end
  else
    -- 従来どおり rawvideo 経由で受け取る
    table.insert(right_parts, "--demuxer=rawvideo")
    table.insert(right_parts, "--demuxer-rawvideo-w=" .. tostring(o.w))
    table.insert(right_parts, "--demuxer-rawvideo-h=" .. tostring(o.h))
    table.insert(right_parts, "--demuxer-rawvideo-fps=" .. tostring(o.fps))
    table.insert(right_parts, "--demuxer-rawvideo-mp-format=" .. tostring(o.mp_format))
  end

  if o.enable_logs and trim(o.mpv_child_log) ~= "" then
    table.insert(right_parts, "--log-file=" .. q(o.mpv_child_log))
  end

  local ml = trim(o.mpv_child_msg_level)
  if ml ~= "" then table.insert(right_parts, "--msg-level=" .. ml) end

  local extra = trim(o.extra_mpv_args)
  if extra ~= "" then table.insert(right_parts, extra) end

  table.insert(right_parts, "-")
  local right = table.concat(right_parts, " ")

  local err_redir = "2>nul"
  if o.enable_logs and trim(o.dshowcap_log) ~= "" then
    err_redir = "2> " .. q(o.dshowcap_log)
  end
  return left .. " " .. err_redir .. " | " .. right
end

local function launch_now()
  if trim(o.video_device) == "" then
    mp.osd_message("ビデオデバイス未選択です（showで選択してください）", 2)
    return
  end

  local mpv_path = resolve_mpv_path()
  local dshowcap_path = o.dshowcap_exe  -- run from working dir (same folder)
  local pipeline = build_pipeline(dshowcap_path, mpv_path)

  if trim(o.debug_cmdline_file) ~= "" then
    write_file(o.debug_cmdline_file, pipeline .. "\r\n", false)
  end

  local cmd_path = trim(o.launch_cmd_file)
  local cmd_lines = {}
  cmd_lines[#cmd_lines+1] = "@echo off"
  cmd_lines[#cmd_lines+1] = "setlocal"
  cmd_lines[#cmd_lines+1] = "chcp 65001 >nul"
  cmd_lines[#cmd_lines+1] = "cd /d \"%~dp0\""
  if o.enable_logs and trim(o.launch_run_log) ~= "" then
    cmd_lines[#cmd_lines+1] = "( " .. pipeline .. " ) > " .. q(o.launch_run_log) .. " 2>&1"
  else
    cmd_lines[#cmd_lines+1] = pipeline .. " >nul 2>&1"
  end
  cmd_lines[#cmd_lines+1] = "endlocal"
  write_file(cmd_path, table.concat(cmd_lines, "\r\n") .. "\r\n", false)

  mp.osd_message(("起動: 映像=%s / 音声=%s"):format(o.video_device, o.audio_device), 2)
  mp.commandv("run", "cmd.exe", "/D", "/S", "/V:OFF", "/C", cmd_path)

  if o.restart_self then mp.commandv("quit") end
end

----------------------------------------------------------------------
-- show: pick video then audio then start
----------------------------------------------------------------------

local function pick_audio_and_launch()
  mp.osd_message("オーディオデバイス検索中...", 1.0)

  list_async("audio",
    function(audios)
      local audio_items = { "(オーディオなし)" }
      for _, a in ipairs(audios) do
        table.insert(audio_items, a)
      end

      menu_open(audio_items, "オーディオキャプチャデバイスを選択", function(apick)
        if apick == "(オーディオなし)" then
          o.audio_device = ""
        else
          o.audio_device = apick
        end
        save_last_selection()
        launch_now()
      end)
    end,
    function(reason, errfile, outfile)
      mp.osd_message(("オーディオ列挙失敗: %s\nerr=%s"):format(reason, errfile), 3.0)
    end
  )
end

local function show_picker()
  mp.osd_message("ビデオデバイス検索中...", 1.0)

  list_async("video",
    function(videos)
      if #videos == 0 then
        mp.osd_message("ビデオデバイスが0件です", 2.0)
        return
      end

      menu_open(videos, "ビデオキャプチャデバイスを選択", function(vpick)
        o.video_device = vpick

        mp.osd_message("解像度/FPS取得中...", 1.0)
        query_default_format_async(vpick,
          function(_fmt)
            mp.osd_message(string.format("解像度: %dx%d @%s fps", o.w, o.h, tostring(o.fps)), 1.5)
            pick_audio_and_launch()
          end,
          function(reason, errfile, outfile)
            mp.osd_message(
              string.format("解像度取得失敗 (%s)。既定 %dx%d @%s fps を使用します。",
                           tostring(reason or "?"), o.w, o.h, tostring(o.fps)), 3.0)
            pick_audio_and_launch()
          end
        )
      end)
    end,
    function(reason, errfile, outfile)
      mp.osd_message(("ビデオ列挙失敗: %s\nerr=%s"):format(reason, errfile), 3.0)
    end
  )
end

----------------------------------------------------------------------
-- When this mpv instance is the "capture viewer" launched by dshowCap
-- (demuxer=rawvideo, reading raw frames from stdin), then dropping a
-- normal media file (mp4, mkv, etc.) on the window will normally try
-- to decode it as rawvideo and show "green snow".
--
-- To avoid this, detect that situation and:
--   1) spawn a *new* normal mpv process for the dropped file
--   2) quit this capture mpv so that dshowCap.exe also終了
--      (it will exit when its pipe is closed)
--
-- NOTE:
--   * We never touch the main "launcher" mpv (the one where F3 opens
--     the device picker), because it does not use demuxer=rawvideo.
--   * We also avoid using utils.subprocess(): only mp.commandv("run").
----------------------------------------------------------------------

-- Helper: ensure dshowCap.exe (and its child audio mpv) are stopped.
-- We don't care about the exit status; on systems without dshowCap.exe
-- this will just fail silently.
local function kill_dshowcap_tree()
  msg.info("[dshow_cmd_launch] requesting dshowCap.exe shutdown (if running)")
  -- Use taskkill directly so we don't need any temporary .cmd files.
  -- /T kills the entire process tree (including the audio mpv that
  -- dshowCap.exe may have spawned).
  mp.commandv("run", "taskkill.exe", "/IM", "dshowCap.exe", "/T", "/F")
end

local function is_capture_child_process()
  -- Heuristic: this mpv was started by our pipeline as a capture viewer.
  --
  -- Raw formats:
  --   demuxer=rawvideo, input="-"
  -- Compressed formats (MJPEG/H264/HEVC etc.):
  --   demuxer=lavf, (optionally) demuxer-lavf-format=..., input="-"
  --
  -- When such a process tries to play a normal media file, the forced demuxer
  -- options can prevent playback (e.g. demuxer-lavf-format=mjpeg).
  -- So we detect both cases and hand off to a fresh mpv.
  local demuxer_opt = mp.get_property("options/demuxer", "")
  local is_raw = (demuxer_opt == "rawvideo")
  local is_lavf = (demuxer_opt == "lavf")
  if not is_raw and not is_lavf then
    return false
  end
  -- For extra safety, require that we are reading from stdin ( "-" or "fd://" )
  local fname = mp.get_property("stream-open-filename", "")
  fname = trim(fname)
  if fname == "" then
    return false
  end
  if fname == "-" or fname:match("^fd://") then
    if is_raw then
      return true
    end
    -- For lavf capture, require a hint that we are forcing lavf handling.
    -- This keeps the heuristic conservative for rare manual stdin use.
    local forced_fmt = trim(mp.get_property("options/demuxer-lavf-format", ""))
    local forced_oadd = trim(mp.get_property("options/demuxer-lavf-o-add", ""))
    if forced_fmt ~= "" or forced_oadd ~= "" then
      return true
    end
    return false
  end
  -- If capture-like demuxer but not stdin, we leave it alone (rare/manual use).
  return false
end

local capture_child_mode = false
local handoff_done = false

-- We initialise capture_child_mode lazily on first file-loaded, because
-- stream-open-filename is reliably set there.
local function on_file_loaded_handoff()
  if handoff_done then
    return
  end

  if not capture_child_mode then
    -- First time here: decide if this process is the capture child.
    capture_child_mode = is_capture_child_process()
    if not capture_child_mode then
      return
    end
  end

  -- At this point, this process *is* the capture viewer.
  -- Check what we are now trying to play.
  local fname = mp.get_property("stream-open-filename", "")
  fname = trim(fname)

  if fname == "" then
    return
  end

  -- If we are still just reading from stdin ("-" or "fd://"), then we
  -- are still in capture mode. Nothing to do.
  if fname == "-" or fname:match("^fd://") then
    return
  end

  -- Here: capture viewer, but the current file is a "real" media file.
  -- Stop dshowCap.exe (and its audio mpv) so capture audio does not
  -- continue playing behind the new file playback.
  kill_dshowcap_tree()
  -- Hand off to a fresh mpv process without rawvideo options.
  handoff_done = true

  local mpv_path = resolve_mpv_path()
  if not mpv_path or trim(mpv_path) == "" then
    msg.error("[dshow_cmd_launch] handoff: failed to resolve mpv path")
    return
  end

  msg.info(("[dshow_cmd_launch] Detected normal file during capture, handoff to new mpv: %s"):format(fname))

  -- Spawn new mpv normally (no cmd.exe wrapper).
  -- Working directory is inherited from this mpv process.
  mp.commandv("run", mpv_path, fname)

  -- Quit this capture mpv; this will also close the pipe to dshowCap.exe.
  mp.commandv("quit")
end

-- We always register the event, but it will be a no-op on normal mpv
-- instances (capture_child_mode stays false).
mp.register_event("file-loaded", on_file_loaded_handoff)





---------------------------------------------------------------------
-- Global cleanup when capture mpv is closed
-- 「キャプチャ中に mpv を終了したら、すべて taskkill してクリア」用。
--
-- ・capture_child_mode が true かつ handoff_done == false のときだけ実行
--   -> パイプ視聴用 mpv ウィンドウをユーザーが閉じたケースのみ。
-- ・handoff_done == true の場合（ファイルドロップ時の引き継ぎ）は
--   ここで taskkill "mpv.exe" してしまうと、新しく起動した mpv まで
--   巻き込んで終了してしまうので、何もしない。
---------------------------------------------------------------------
local function on_shutdown_kill_all()
  if capture_child_mode and not handoff_done then
    -- ユーザー指定: 最後にこれを走らせて完全にクリアにする
    -- run "taskkill" "/IM" "dshowCap.exe" "/IM" "mpv.exe" "/F" ; quit
    mp.commandv("run", "taskkill.exe", "/IM", "dshowCap.exe", "/IM", "mpv.exe", "/F")
  end
end

mp.register_event("shutdown", on_shutdown_kill_all)

mp.register_script_message("show", show_picker)
mp.register_script_message("start", launch_now)



-- ---- Audio-out mode helper (system/none/mpv) ----
local audio_out_modes = {"system","none","mpv"}
local function audio_out_index(v)
  for i,m in ipairs(audio_out_modes) do if m==v then return i end end
  return 1
end

local function cycle_audio_out()
  local idx = audio_out_index(o.audio_out)
  idx = idx + 1
  if idx > #audio_out_modes then idx = 1 end
  o.audio_out = audio_out_modes[idx]
  mp.osd_message(("Audio out mode: %s (system=software monitor, none=use device direct output, mpv=legacy)"):format(o.audio_out), 3)
  msg.info("audio_out mode changed to: " .. o.audio_out)
end

-- Expose as: script-message-to dshow_cmd_launch cycle-audio-out
mp.register_script_message("cycle-audio-out", cycle_audio_out)


