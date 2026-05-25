-- runtime.lua - easyLua 启动时自动加载，用户脚本零 ffi.cdef
--
-- 命名规则（v2 扁平化）：所有 Lua API 都是全局函数，**无命名空间前缀**。
--   Click(x, y)         不再是 Motion.Click
--   FindColor(...)      不再是 Images.FindColor
--   Toast(...)          不再是 Ui.Toast
--   Sleep(ms)           延迟（毫秒）
--   ScreenOff()         熄屏（原 Device.Sleep；与 Sleep 区分开避免冲突）
--   ...
-- 性能：所有调用经 LuaJIT FFI，trace 后内联到 native call (~5-20 ns)
--
-- 冲突表：
--   Sleep      → 延迟（原 Utils.Sleep）
--   ScreenOff  → 熄屏（原 Device.Sleep，改名以避免和上面的 Sleep 冲突）
--   其它方法名都唯一，直接顶层 export

local ffi = require("ffi")

ffi.cdef[[
typedef long long           int64_t;
typedef unsigned long long  uint64_t;
typedef unsigned char       uint8_t;

/* Screen */
int             EasyLua_Screen_Width(void);
int             EasyLua_Screen_Height(void);
const uint8_t*  EasyLua_Screen_Data(void);
uint64_t        EasyLua_Screen_Seq(void);
int             EasyLua_Screen_Pixel(int x, int y);

/* Screen.CaptureScreen 返回的快照（独立 RGBA buffer）。
 * Lua 端把它包装成 cdata + __gc，自动释放。 */
typedef struct EasyLuaSnapshot {
    int      w;
    int      h;
    uint8_t *pix;
} EasyLuaSnapshot;

EasyLuaSnapshot* EasyLua_Screen_Capture(int x1, int y1, int x2, int y2);
void             EasyLua_Screen_CaptureFree(EasyLuaSnapshot *s);
int              EasyLua_Screen_SnapshotPixel(const EasyLuaSnapshot *s, int x, int y);
int              EasyLua_Screen_SnapshotSave(const EasyLuaSnapshot *s,
                                             const char *path, const char *type,
                                             int quality);

/* Images */
int  EasyLua_Images_CmpColor(int x, int y, const char *color, float sim);
int  EasyLua_Images_FindColor(int x1, int y1, int x2, int y2,
                              const char *color, float sim, int dir,
                              int *out_x, int *out_y);
int  EasyLua_Images_GetColorCountInRegion(int x1, int y1, int x2, int y2,
                                          const char *color, float sim);
int  EasyLua_Images_DetectsMultiColors(const char *colors, float sim);
int  EasyLua_Images_FindMultiColors(int x1, int y1, int x2, int y2,
                                    const char *colors, float sim, int dir,
                                    int *out_x, int *out_y);
int  EasyLua_Images_FindMultiColorsAll(int x1, int y1, int x2, int y2,
                                       const char *colors, float sim, int dir,
                                       int *out_xy, int max_n);

/* Images.FindPic（找图）：模板加载 + 两段式 SAD 匹配。
 * 模板结构体在 C 端是不透明类型；Lua 端持有 cdata 指针 + __gc 自动释放。 */
typedef struct EasyLuaTemplate EasyLuaTemplate;

EasyLuaTemplate *EasyLua_Images_LoadTemplate(const uint8_t *bytes, int len,
                                             const char *path_hint);
void             EasyLua_Images_FreeTemplate(EasyLuaTemplate *t);
int              EasyLua_Images_TemplateW(const EasyLuaTemplate *t);
int              EasyLua_Images_TemplateH(const EasyLuaTemplate *t);
int              EasyLua_Images_TemplateValidPx(const EasyLuaTemplate *t);
int              EasyLua_Images_FindPic(int x1, int y1, int x2, int y2,
                                        const EasyLuaTemplate *t,
                                        float sim, int dir,
                                        int *out_x, int *out_y);
int              EasyLua_Images_FindPicAll(int x1, int y1, int x2, int y2,
                                           const EasyLuaTemplate *t, float sim,
                                           int *out_xy, int max_n);

/* 色差模式（大漠 FindPic 兼容） */
int              EasyLua_Images_FindPicDelta(int x1, int y1, int x2, int y2,
                                             const EasyLuaTemplate *t,
                                             int dr, int dg, int db,
                                             float sim, int dir,
                                             int *out_x, int *out_y);
int              EasyLua_Images_FindPicAllDelta(int x1, int y1, int x2, int y2,
                                                const EasyLuaTemplate *t,
                                                int dr, int dg, int db,
                                                float sim,
                                                int *out_xy, int max_n);

/* 多模板（"A.png|B.png" 在 C 内单次扫描） */
int              EasyLua_Images_FindPicMulti(int x1, int y1, int x2, int y2,
                                             const EasyLuaTemplate * const *tpls,
                                             int n_tpls,
                                             float sim, int dir,
                                             int *out_x, int *out_y, int *out_idx);
int              EasyLua_Images_FindPicAllMulti(int x1, int y1, int x2, int y2,
                                                const EasyLuaTemplate * const *tpls,
                                                int n_tpls,
                                                float sim,
                                                int *out_xy, int *out_idxs, int max_n);
int              EasyLua_Images_FindPicMultiDelta(int x1, int y1, int x2, int y2,
                                                  const EasyLuaTemplate * const *tpls,
                                                  int n_tpls,
                                                  int dr, int dg, int db,
                                                  float sim, int dir,
                                                  int *out_x, int *out_y, int *out_idx);
int              EasyLua_Images_FindPicAllMultiDelta(int x1, int y1, int x2, int y2,
                                                     const EasyLuaTemplate * const *tpls,
                                                     int n_tpls,
                                                     int dr, int dg, int db,
                                                     float sim,
                                                     int *out_xy, int *out_idxs, int max_n);

/* Motion */
int  EasyLua_Motion_Init(int max_x, int max_y);
int  EasyLua_Motion_Down(int x, int y, int slot, int orientation);
int  EasyLua_Motion_Move(int x, int y, int slot, int orientation);
int  EasyLua_Motion_Up(int slot);
int  EasyLua_Motion_Swipe(int x1, int y1, int x2, int y2,
                          int duration_ms, int slot, int orientation);
int  EasyLua_Motion_SwipeBezier(int x1, int y1, int x2, int y2,
                                int duration_ms, int slot, int orientation);
void EasyLua_Motion_Cleanup(void);

/* Ui（Toast / Highlight，自绘，不依赖 APK，绕 MIUI TextView） */
int  EasyLua_Ui_Toast(const char *msg, int x, int y, int dur_ms);
int  EasyLua_Ui_Highlight(int x, int y, int w, int h,
                          int color_argb, int dur_ms, const char *label);
int  EasyLua_Ui_HighlightOff(void);

/* Utils */
void    EasyLua_Utils_Sleep(int ms);
int64_t EasyLua_Utils_NowMs(void);
int64_t EasyLua_Utils_NowUs(void);
int     EasyLua_Utils_Log(const char *msg);

/* App */
int  EasyLua_App_CurrentPackage(char *buf, int n);
int  EasyLua_App_CurrentActivity(char *buf, int n);
int  EasyLua_App_Launch(const char *pkg);
int  EasyLua_App_IsInstalled(const char *pkg);
int  EasyLua_App_ForceStop(const char *pkg);
int  EasyLua_App_Clear(const char *pkg);
int  EasyLua_App_OpenUrl(const char *url);

/* Device */
int  EasyLua_Device_IsScreenOn(void);
int  EasyLua_Device_IsScreenUnlock(void);
void EasyLua_Device_WakeUp(void);
void EasyLua_Device_Sleep(void);
int  EasyLua_Device_GetBattery(void);
int  EasyLua_Device_GetBatteryStatus(void);
void EasyLua_Device_Vibrate(int ms);
int  EasyLua_Device_GetSdkInt(void);
int  EasyLua_Device_GetBrand(char *buf, int n);
int  EasyLua_Device_GetModel(char *buf, int n);

/* IME */
int  EasyLua_IME_GetClipText(char *buf, int n);
int  EasyLua_IME_SetClipText(const char *text);
int  EasyLua_IME_InputText(const char *text);
int  EasyLua_IME_KeyAction(int keycode);

/* Shell */
int  EasyLua_Shell_Exec(const char *cmd, char *buf, int n);

/* Net（TCP / UDP / HTTP / DNS） */
int  EasyLua_Net_TcpConnect(const char *host, int port, int timeout_ms);
int  EasyLua_Net_TcpSend(int fd, const char *buf, int len, int timeout_ms);
int  EasyLua_Net_TcpRecv(int fd, char *buf, int cap, int timeout_ms);
int  EasyLua_Net_TcpSetNoDelay(int fd, int on);
int  EasyLua_Net_UdpOpen(void);
int  EasyLua_Net_UdpSendTo(int fd, const char *host, int port,
                           const char *buf, int len);
int  EasyLua_Net_UdpRecvFrom(int fd, char *buf, int cap, int timeout_ms,
                             char *out_host, int out_n, int *out_port);
void EasyLua_Net_Close(int fd);
int  EasyLua_Net_SetTimeout(int fd, int recv_ms, int send_ms);
int  EasyLua_Net_DnsResolve(const char *host, char *out, int n);
int  EasyLua_Net_LocalIp(char *out, int n);
int  EasyLua_Net_HttpRequest(const char *method, const char *url,
                             const char *headers,
                             const char *body, int body_len,
                             char *out_buf, int out_cap,
                             int *out_status, int timeout_ms);
]]

local C = ffi.C

-- ====================================================
-- 通用工具：Sleep / NowMs / NowUs / Log
-- ====================================================

function Sleep(ms)  C.EasyLua_Utils_Sleep(ms or 0) end
function NowMs()    return tonumber(C.EasyLua_Utils_NowMs()) end
function NowUs()    return tonumber(C.EasyLua_Utils_NowUs()) end
function Log(msg)   C.EasyLua_Utils_Log(tostring(msg)) end

-- ====================================================
-- 脚本生命周期：exitScript / setStopCallback
-- ====================================================
--
-- 退出码 (exitcode) 语义：
--   0  主线程正常结束
--   1  用户触发结束（运行时 Lua 错误，未被 pcall 捕获）
--   2  主动调用 exitScript()
--
-- 用法示例：
--   setStopCallback(function(error, exitcode)
--     if error then print("脚本异常结束 exitcode=" .. exitcode)
--     else        print("脚本正常结束 exitcode=" .. exitcode) end
--   end)
--
--   exitScript()   -- 立刻终止，会触发 stop callback 且 exitcode = 2

local _stop_cb       -- 用户注册的回调
local _exit_token = setmetatable({}, { __tostring = function() return "<easylua-exit>" end })

--- 主动终止脚本。会跳到 run_protected 的 xpcall 出口，
-- 并以 exitcode = 2 触发 stop callback。
function exitScript()
  error(_exit_token, 0)   -- level=0 不带行号信息
end

--- 注册脚本结束时的回调。回调签名 fn(isError, exitcode)。
-- 重复调用会覆盖之前的回调；传 nil 取消。
function setStopCallback(fn)
  _stop_cb = fn
end

-- 由 native 入口（runtime 末尾自动安装）回调，把 (isError, exitcode) 透传给用户回调。
-- 不直接暴露给用户脚本。
local function _easylua_dispatch_stop(isError, exitcode)
  if type(_stop_cb) == "function" then
    -- 防止 callback 内部抛错连带挂掉退出流程
    pcall(_stop_cb, isError, exitcode)
  end
end

-- 把 dispatcher 暴露给 C 端通过全局表拿到（runtime 加载完毕后由 run_script 调）。
-- 使用 _G 而不是命名空间，C 端 lua_getglobal 即可；下划线开头表明是内部约定。
_G.__easylua_dispatch_stop = _easylua_dispatch_stop
_G.__easylua_exit_token    = _exit_token

-- ====================================================
-- Config（启动前 ui.lua 弹窗收集到的配置）
-- ====================================================
--
-- Dialog 弹窗机制：
--   1. SampleUI APK 在用户点 ▶ 时解析脚本同级的 ui.lua 弹出 AlertDialog；
--   2. 用户填好确定后，SampleUI 把值原地回写到 ui.lua 自身的 default 参数；
--   3. 脚本进程启动时，runtime.lua 自动 dofile(ui.lua)，把 default 收进 Config。
--
-- 用户脚本里直接读：
--   if Config.autoRetry then print("重试次数=" .. Config.retryCount) end
--
-- 脚本运行中如果 ui.lua 被外部修改，可调 LoadConfig() 重新加载。
--
-- 实现要点：
--   - DSL 函数（addCheckBox / addEditText / ...）在 ui.lua 中只是声明，
--     这里 stub 成"按签名取出 default 参数、写到 Config 的纯 Lua 函数"；
--   - 装饰 / 容器 / 修饰符（dialogTitle / beginRow / setHint 等）stub 成 noop；
--   - dofile 完成后立即把这些 stub 从 _G 还原，不污染用户脚本环境。

Config = {}

-- 控件名 -> default 在第几个参数（1-based）
local _CFG_WIDGET_DEFAULT_AT = {
  addCheckBox    = 3,
  addEditText    = 3,
  addEditNumber  = 3,
  addSpinner     = 4,
  addRadioGroup  = 4,
  addSeekBar     = 5,
  addSlider      = 5,  -- alias of addSeekBar
  addProgressBar = 5,
  addDatePicker  = 3,
  addTimePicker  = 3,
  addColorButton = 3,
}

-- 类型归一化：根据控件名把 default 转成对应 lua 类型
local function _cfg_normalize(name, val)
  if name == "addCheckBox" then
    if val == nil or val == false then return false end
    if val == true then return true end
    -- 容错：字符串 "false"/"0" 视为 false，其它非空视为 true
    if type(val) == "string" then return val ~= "" and val ~= "false" and val ~= "0" end
    return val ~= 0
  elseif name == "addEditNumber" or name == "addSeekBar" or name == "addSlider"
      or name == "addProgressBar" then
    return tonumber(val) or 0
  else
    return val == nil and "" or tostring(val)
  end
end

-- ui.lua 路径候选：脚本同级 > 同级 ui/ 子目录 > 部署根 > scripts/ > ui/
local function _cfg_candidate_paths()
  local list = {}

  -- 优先用 bootstrap 写下来的脚本入口推导（_easyLua_bootstrap.entry）
  local bs = rawget(_G, "_easyLua_bootstrap")
  if type(bs) == "table" and type(bs.entry) == "string" then
    local dir = bs.entry:match("^(.+)/[^/]+$")
    if dir and dir ~= "" then
      list[#list + 1] = dir .. "/ui.lua"
      list[#list + 1] = dir .. "/ui/ui.lua"
    end
  end

  list[#list + 1] = "/data/local/tmp/easyLua/ui.lua"
  list[#list + 1] = "/data/local/tmp/easyLua/scripts/ui.lua"
  list[#list + 1] = "/data/local/tmp/easyLua/ui/ui.lua"
  return list
end

local function _cfg_first_existing(paths)
  for _, p in ipairs(paths) do
    local f = io.open(p, "r")
    if f then f:close(); return p end
  end
  return nil
end

--- 重新解析 ui.lua，把所有 default 收集到 Config 表。
-- 启动时 runtime 已自动调过一次；脚本运行期间 ui.lua 被改写后想刷新可手动再调。
-- 返回 ui.lua 的实际路径（找不到返回 nil）。
function LoadConfig()
  local ui_path = _cfg_first_existing(_cfg_candidate_paths())
  if not ui_path then return nil end

  -- 清空老数据（保留 Config 表本身的引用，方便用户脚本持有）
  for k in pairs(Config) do Config[k] = nil end

  -- 备份 + 替换全局；dofile 完成后还原
  local saved = {}
  local function override(name, fn)
    saved[name] = rawget(_G, name)
    rawset(_G, name, fn)
  end

  -- widget stubs：把 default 写到 Config[varName]
  for fname, default_idx in pairs(_CFG_WIDGET_DEFAULT_AT) do
    local capture_name = fname  -- 闭包捕获
    local capture_idx = default_idx
    override(fname, function(...)
      local var = (select(1, ...))
      if type(var) == "string" and var ~= "" then
        Config[var] = _cfg_normalize(capture_name, (select(capture_idx, ...)))
      end
    end)
  end

  -- 装饰 / 容器 / 修饰符 stubs：noop
  local noops = {
    "dialogTitle", "dialogSize", "dialogSizePercent",
    "dialogPosition", "dialogOffset",
    "beginGroup", "endGroup", "beginRow", "endRow",
    "beginTabs", "endTabs", "beginTab", "endTab",
    "addTextView", "addSeparator", "addImage",
    "setWeight", "setHint", "setMargin", "setColor",
    "setTextSize", "setEnabled", "setVisible", "bindVisible",
  }
  for _, n in ipairs(noops) do override(n, function() end) end

  local ok, err = pcall(dofile, ui_path)

  -- 还原全局函数（即使 dofile 报错也要还原）
  for n, v in pairs(saved) do rawset(_G, n, v) end

  if not ok then
    io.stderr:write("[easylua-c] Config load error (" .. ui_path .. "): "
                    .. tostring(err) .. "\n")
    return nil
  end
  return ui_path
end

-- runtime 加载时主动触发一次。找不到 ui.lua 不报错，Config 保持空表。
LoadConfig()

-- ====================================================
-- Screen（屏幕信息 + 像素）
-- ====================================================

function ScreenWidth()   return C.EasyLua_Screen_Width()  end
function ScreenHeight()  return C.EasyLua_Screen_Height() end
function ScreenSeq()     return tonumber(C.EasyLua_Screen_Seq()) end
function ScreenData()    return C.EasyLua_Screen_Data() end

--- 取像素 RGBA（单像素）
function Pixel(x, y)
  local w = C.EasyLua_Screen_Width()
  local h = C.EasyLua_Screen_Height()
  if x < 0 or y < 0 or x >= w or y >= h then return 0, 0, 0, 0 end
  local p = C.EasyLua_Screen_Data()
  if p == nil then return 0, 0, 0, 0 end
  local idx = (y * w + x) * 4
  return p[idx], p[idx + 1], p[idx + 2], p[idx + 3]
end

--- 取像素颜色字符串 "RRGGBB"
function PixelHex(x, y)
  local rgb = C.EasyLua_Screen_Pixel(x, y)
  return string.format("%06X", rgb)
end

-- ----------------------------------------------------------------
-- CaptureScreen：把当前帧的指定区域抓成独立快照
-- ----------------------------------------------------------------
--
-- 与"直接读 Screen API"不同，CaptureScreen 返回的是一份**冻结**的
-- 像素数据，调用方拿到后即使下一帧到达也不会被覆盖。适合：
--   - 截屏存盘
--   - 跨多帧对比
--   - OCR / 模板匹配等耗时处理
--
-- 返回的对象支持：
--   snap:Width() / snap:Height()      尺寸
--   snap:Pixel(x, y)                  返回 r, g, b, a
--   snap:PixelHex(x, y)               返回 "RRGGBB"
--   snap:Save(path [, type [, quality]])  保存到文件，type = "png"/"jpg"/"bmp"
--   snap:Free()                       立即释放（也会被 __gc 自动调）
-- 失败返回 nil（视频流挂了 / 区域非法 / 内存不足）。

local _SnapshotMT
local _CaptureGcType = ffi.typeof([[
  struct { struct EasyLuaSnapshot *p; }
]])

-- ffi.gc 包一层 cdata struct，确保 __gc 触发时把 native 指针 free
local function _wrap_snapshot(cptr)
  if cptr == nil then return nil end
  local holder = ffi.new(_CaptureGcType)
  holder.p = cptr
  ffi.gc(holder, function(h)
    if h.p ~= nil then
      C.EasyLua_Screen_CaptureFree(h.p)
      h.p = nil
    end
  end)
  return setmetatable({ _h = holder }, _SnapshotMT)
end

_SnapshotMT = {
  __index = {
    Width  = function(self) return self._h.p ~= nil and self._h.p.w or 0 end,
    Height = function(self) return self._h.p ~= nil and self._h.p.h or 0 end,

    --- 取像素 RGBA，越界返回 0,0,0,0
    Pixel = function(self, x, y)
      local p = self._h.p
      if p == nil then return 0, 0, 0, 0 end
      local w, h = p.w, p.h
      if x < 0 or y < 0 or x >= w or y >= h then return 0, 0, 0, 0 end
      local idx = (y * w + x) * 4
      local pix = p.pix
      return pix[idx], pix[idx + 1], pix[idx + 2], pix[idx + 3]
    end,

    --- "RRGGBB"
    PixelHex = function(self, x, y)
      local rgb = C.EasyLua_Screen_SnapshotPixel(self._h.p, x, y)
      return string.format("%06X", rgb)
    end,

    --- 保存到文件。
    -- @param path     输出路径
    -- @param type     "png" / "jpg" (alias "jpeg") / "bmp"，
    --                 省略时按 path 后缀推断（识别 .png/.jpg/.jpeg/.bmp）
    -- @param quality  JPG 质量 1..100，省略 / <=0 取 90，其它格式忽略
    -- @return         true 成功，false 失败
    Save = function(self, path, type, quality)
      if self._h.p == nil then return false end
      return C.EasyLua_Screen_SnapshotSave(self._h.p, path,
                                           type or "", quality or 0) == 0
    end,

    --- 立即释放底层 buffer（不必等 GC）。释放后所有访问返回零值，重复调用安全。
    Free = function(self)
      local h = self._h
      if h.p ~= nil then
        C.EasyLua_Screen_CaptureFree(h.p)
        h.p = nil
      end
    end,
  },

  --- to-be-closed 协议：作用域退出时自动释放。
  -- 用法：local snap <close> = CaptureScreen()
  --      do block / 函数 return / error 抛出 都会立即触发，不等 GC。
  __close = function(self)
    local h = self._h
    if h.p ~= nil then
      C.EasyLua_Screen_CaptureFree(h.p)
      h.p = nil
    end
  end,
}

--- 抓帧快照。x2/y2 = 0 表示用屏幕边界。
-- 没传或全 0 = 全屏抓取。
function CaptureScreen(x1, y1, x2, y2)
  return _wrap_snapshot(C.EasyLua_Screen_Capture(x1 or 0, y1 or 0, x2 or 0, y2 or 0))
end

-- ====================================================
-- Images（找色 / 比色）
-- ====================================================

local _xy_buf = ffi.new("int[2]")  -- 给 FindColor 复用，避免每次分配

--- 比色：测试 (x, y) 是否匹配 color
-- color 格式 "RRGGBB" 或 "RRGGBB-tolBytes" 或 "C1|C2-..."
-- 返回 boolean
function CmpColor(x, y, color, sim)
  return C.EasyLua_Images_CmpColor(x, y, color, sim or 1.0) == 1
end

--- 区域找色，返回 x, y。找不到返回 -1, -1。
-- x2 / y2 = 0 表示用屏幕边界
function FindColor(x1, y1, x2, y2, color, sim, dir)
  C.EasyLua_Images_FindColor(x1, y1, x2 or 0, y2 or 0,
                             color, sim or 0.9, dir or 0,
                             _xy_buf, _xy_buf + 1)
  return _xy_buf[0], _xy_buf[1]
end

--- 区域内匹配颜色像素数量
function GetColorCountInRegion(x1, y1, x2, y2, color, sim)
  return C.EasyLua_Images_GetColorCountInRegion(x1, y1, x2 or 0, y2 or 0,
                                                color, sim or 1.0)
end

--- 多点比色：所有点都命中才返回 true
-- colors 格式："x1,y1,RRGGBB-tol,x2,y2,RRGGBB-tol,..."
function DetectsMultiColors(colors, sim)
  return C.EasyLua_Images_DetectsMultiColors(colors, sim or 1.0) == 1
end

--- 多点找色：基色全屏扫描，命中后再验证相对偏移点。
-- colors 格式："base[-tol],dx1,dy1,c1[-tol],dx2,dy2,c2[-tol],..."
-- 返回 (x, y)，找不到返回 (-1, -1)
function FindMultiColors(x1, y1, x2, y2, colors, sim, dir)
  C.EasyLua_Images_FindMultiColors(x1, y1, x2 or 0, y2 or 0,
                                   colors, sim or 0.9, dir or 0,
                                   _xy_buf, _xy_buf + 1)
  return _xy_buf[0], _xy_buf[1]
end

local _xy_all_buf = ffi.new("int[?]", 512)  -- 最多 256 个点
-- 多模板找图复用缓冲（C 端 FindPicMulti / All 系列共用）
local _idx_buf      = ffi.new("int[1]")              -- FindPicMulti 命中模板下标
local _idx_all_buf  = ffi.new("int[?]", 256)         -- FindPicAllMulti 命中模板下标数组
local _tpl_arr_type = ffi.typeof("const struct EasyLuaTemplate *[?]")
-- 解析 "A.png|B.png|..." 后的 cdata 数组缓冲（按需扩容）
local _tpl_arr_cap  = 0
local _tpl_arr_buf  = nil

--- 多点找色 - 找全部位置。
-- 返回 table 列表 {{x=,y=}, ...}，最多 256 个点。
function FindMultiColorsAll(x1, y1, x2, y2, colors, sim, dir)
  local n = C.EasyLua_Images_FindMultiColorsAll(x1, y1, x2 or 0, y2 or 0,
                                                colors, sim or 0.9, dir or 0,
                                                _xy_all_buf, 256)
  if n <= 0 then return {} end
  local list = {}
  for i = 0, n - 1 do
    list[i + 1] = { x = _xy_all_buf[i * 2], y = _xy_all_buf[i * 2 + 1] }
  end
  return list
end

-- ----------------------------------------------------------------
-- Images.FindPic（找图）：模板加载
-- ----------------------------------------------------------------
--
-- LoadTemplate(path) 从磁盘读取 PNG / BMP / JPG 模板字节，交给 C 层一次性
-- 完成解码 + 透明像素剔除 + 特征点选取，返回包装好的模板对象。
--   - 通过 Lua 层 io.open 读字节，Stage 7 的 `.enc` 透明解密自动生效，
--     用户无需关心模板是否加密
--   - 模板对象通过 ffi.gc 绑定 EasyLua_Images_FreeTemplate，作用域结束 /
--     GC 触发时自动释放 native 内存
--   - 模板对象支持 :Width() / :Height() / :ValidPx() / :Free() 方法，
--     以及 to-be-closed 协议（local tpl <close> = LoadTemplate(...)）

-- 模板对象释放：被 :Free() 与 __close 共用，保证幂等。
-- 步骤：
--   1) 已释放（self._t == nil）→ 直接返回，多次调用安全
--   2) 显式调用前先 ffi.gc(cdata, nil) 解绑 finalizer，否则当 cdata 之后
--      被 GC 时 EasyLua_Images_FreeTemplate 会被再次调用，触发 double-free
--   3) 调 native free 后立刻把 self._t 置 nil，让其它元方法识别"已释放"
local function _template_release(self)
  local t = self._t
  if t == nil then return end
  ffi.gc(t, nil)                         -- 先解绑 GC，再 free，防 double-free
  C.EasyLua_Images_FreeTemplate(t)
  self._t = nil
end

-- 模板对象元表：
--   __index 暴露 :Width() / :Height() / :ValidPx() / :Free() 桥接到
--           native EasyLua_Images_Template* / FreeTemplate
--   __close 触发即时释放（用法：local tpl <close> = LoadTemplate(...)）
--
-- 所有 getter 在 self._t == nil（已释放）时返回 0，避免传 NULL 给 C 端
-- 引发段错误，让 :Free() 之后的访问保持安全。
local _TemplateMT = {
  __index = {
    --- 模板宽度（像素）。已释放后返回 0。
    Width = function(self)
      return self._t ~= nil and C.EasyLua_Images_TemplateW(self._t) or 0
    end,

    --- 模板高度（像素）。已释放后返回 0。
    Height = function(self)
      return self._t ~= nil and C.EasyLua_Images_TemplateH(self._t) or 0
    end,

    --- 有效（非透明）像素数；调试 / 衡量模板信息量用。已释放后返回 0。
    ValidPx = function(self)
      return self._t ~= nil and C.EasyLua_Images_TemplateValidPx(self._t) or 0
    end,

    --- 立即释放底层 native 模板内存（不必等 GC）。
    -- 释放后 :Width / :Height / :ValidPx 返回 0，重复调用 :Free 安全。
    Free = _template_release,
  },

  --- to-be-closed 协议：作用域退出时自动调用，立即释放，不等 GC。
  -- 用法：do local tpl <close> = LoadTemplate("btn.png") ... end
  __close = _template_release,
}

-- 把 native cdata 指针包装为 Lua 模板对象（带元表）。
-- cdata == nil 时返回 nil，避免误生成空对象。
local function _wrap_template(cdata)
  if cdata == nil then return nil end
  return setmetatable({ _t = cdata }, _TemplateMT)
end

--- 加载图片模板，供后续 FindPic / FindPicAll 复用。
-- 支持 PNG / BMP / JPG；自动识别 alpha 通道与四角同色 transparent key。
-- @param path 模板文件路径（相对或绝对）；`.enc` 加密资源由 io.open 透明
--             解密层自动处理，用户传明文 path 即可
-- @return     模板对象；文件不存在 / 字节为空 / 解码失败时返回 nil
function LoadTemplate(path)
  if type(path) ~= "string" or path == "" then return nil end

  -- 1) 读字节：失败立刻 return nil（io.open 包装层会先尝试 path，
  --    再尝试 path .. ".enc" 透明解密；这里只关心最终能不能拿到字节）
  local f = io.open(path, "rb")
  if not f then return nil end
  local bytes = f:read("*a")
  f:close()
  if not bytes or #bytes == 0 then return nil end

  -- 2) 字节零拷贝传给 C：lua string 在本函数作用域内不会被 GC，
  --    EasyLua_Images_LoadTemplate 同步返回后不再持有 buf 引用
  local buf = ffi.cast("const uint8_t *", bytes)
  local cdata = C.EasyLua_Images_LoadTemplate(buf, #bytes, path)
  if cdata == nil then return nil end

  -- 3) 绑定 GC 回调：cdata 不可达时自动调 EasyLua_Images_FreeTemplate
  ffi.gc(cdata, C.EasyLua_Images_FreeTemplate)

  -- 4) 包装成 Lua 对象（任务 6.3 会让对象支持 :Width()/:Height() 等方法）
  return _wrap_template(cdata)
end

-- ----------------------------------------------------------------
-- Images.FindPic（找图）：单次 / 找全部
-- ----------------------------------------------------------------
--
-- 这两个函数与 FindColor / FindMultiColorsAll 完全对齐：
--   - 复用同一份 _xy_buf / _xy_all_buf，不在每次调用里分配新的 cdata 数组
--   - x2 / y2 = 0 走 native 的全屏边界
--   - sim / dir 缺省走 0.9 / 0
--   - 模板参数 tpl 通常是 LoadTemplate 返回的元表对象（带 ._t 字段），
--     这里同时也兼容直接传 cdata 指针的防御性写法

-- 把用户传进来的模板参数解包成内部 cdata 指针。
--   - nil           → nil（视为未提供 / 已释放）
--   - table（带 _t）→ table._t（已 :Free() / __close 后会是 nil）
--   - cdata         → 原样返回（用户绕开 _wrap_template 直接持有指针）
-- ----------------------------------------------------------------
-- Images.FindPic（找图）：模板缓存（按文件名自动 LoadTemplate）
-- ----------------------------------------------------------------
--
-- 用法：
--   FindPic(0, 0, 0, 0, "1.png")              -- 直接传文件名（首次自动加载）
--   FindPic(0, 0, 0, 0, "ui/btn_ok.png", 0.9) -- 也支持子目录相对路径
--   Templates.preload()                       -- 主动遍历 scripts/ 加载所有图
--   Templates.get("1.png"):Width()            -- 显式拿模板对象
--   Templates.dir                             -- 当前模板搜索根目录
--   Templates.set_dir("/sdcard/my_assets")    -- 切换模板根目录
--
-- 设计：
--   - 缓存以"绝对路径"为 key，同一文件多次 require 不重复解码
--   - 文件名解析按"原样 → ScriptDir/name → ScriptDir/<子目录>/name"顺序
--     探测，找到后 LoadTemplate 一次并缓存
--   - 缓存里的模板不绑 ffi.gc finalizer 兜底依然有（来自 LoadTemplate），
--     脚本退出时 LuaJIT 会触发 GC 把 native 内存归还
--   - 默认搜索目录优先用 _easyLua_bootstrap.entry 推导（脚本同级），找不到
--     退回 /data/local/tmp/easyLua/scripts，最后退回 /data/local/tmp/easyLua

Templates = {}
local _tpl_cache = {}                          -- abs_path -> tpl object

-- 推导默认模板搜索根目录
local function _templates_default_dir()
  local bs = rawget(_G, "_easyLua_bootstrap")
  if type(bs) == "table" and type(bs.entry) == "string" then
    local dir = bs.entry:match("^(.+)/[^/]+$")
    if dir and dir ~= "" then return dir end
  end
  -- 回退顺序：APK 部署根 → /data/local/tmp/easyLua/scripts → /data/local/tmp/easyLua
  for _, p in ipairs({
    "/data/local/tmp/easyLua/scripts",
    "/data/local/tmp/easyLua",
  }) do
    local f = io.open(p .. "/.", "r")
    if f then f:close(); return p end
  end
  return "/data/local/tmp/easyLua/scripts"
end

Templates.dir = _templates_default_dir()

--- 切换模板搜索根目录。会清空已缓存的模板（旧目录的模板对象保持有效，
-- 直到 GC 或显式 :Free()）。
function Templates.set_dir(new_dir)
  if type(new_dir) ~= "string" or new_dir == "" then
    return false, "set_dir: 路径必须是非空字符串"
  end
  Templates.dir = new_dir
  for k in pairs(_tpl_cache) do _tpl_cache[k] = nil end
  return true
end

-- 把文件名解析为绝对路径；找不到返回 nil
local function _resolve_template_path(name)
  if type(name) ~= "string" or name == "" then return nil end

  -- 1) 已经是绝对路径或相对当前 cwd 的路径，直接探测
  local f = io.open(name, "rb")
  if f then f:close(); return name end

  -- 2) Templates.dir/name
  local p = Templates.dir .. "/" .. name
  local f2 = io.open(p, "rb")
  if f2 then f2:close(); return p end

  -- 3) Templates.dir 一级子目录递归（最多 4 层），按 lfs.dir 遍历
  if lfs and lfs.dir and lfs.attributes then
    local function search(root, depth)
      if depth > 4 then return nil end
      local ok, iter, state = pcall(lfs.dir, root)
      if not ok or not iter then return nil end
      for entry in iter, state do
        if entry ~= "." and entry ~= ".." then
          local sub = root .. "/" .. entry
          local mode = lfs.attributes(sub, "mode")
          if mode == "file" and entry == name then
            return sub
          elseif mode == "directory" then
            local hit = search(sub, depth + 1)
            if hit then return hit end
          end
        end
      end
      return nil
    end
    local hit = search(Templates.dir, 1)
    if hit then return hit end
  end

  return nil
end

-- 文件后缀大小写不敏感判断，识别 png / bmp / jpg / jpeg
local function _is_template_file(name)
  local ext = name:match("%.([%w]+)$")
  if not ext then return false end
  ext = ext:lower()
  return ext == "png" or ext == "bmp" or ext == "jpg" or ext == "jpeg"
end

--- 按文件名取（或加载并缓存）模板对象。
-- @param name 文件名（如 "1.png"）或相对/绝对路径
-- @return 模板对象，失败返回 nil + 错误信息
function Templates.get(name)
  if type(name) ~= "string" or name == "" then
    return nil, "Templates.get: name 必须是非空字符串"
  end

  -- 命中缓存：直接返回（即使后续 _resolve 失败也以缓存为准，避免 IO 抖动）
  -- 命中规则：先试 Templates.dir/name，再试原始 name
  local cached = _tpl_cache[Templates.dir .. "/" .. name] or _tpl_cache[name]
  if cached and cached._t ~= nil then return cached end

  local abs = _resolve_template_path(name)
  if not abs then
    return nil, "Templates.get: 找不到 " .. name ..
                "（搜索根 = " .. Templates.dir .. "）"
  end

  local tpl = LoadTemplate(abs)
  if not tpl then
    return nil, "Templates.get: LoadTemplate 失败 path=" .. abs
  end

  _tpl_cache[abs] = tpl
  return tpl
end

--- 遍历 Templates.dir 加载所有 png/bmp/jpg/jpeg 模板（递归）。
-- 仅缓存解码成功的；解码失败的会通过 Log 打印一行警告但不中断。
-- @return 成功加载数, 失败数
function Templates.preload()
  if not (lfs and lfs.dir and lfs.attributes) then
    Log("Templates.preload: lfs 不可用，跳过")
    return 0, 0
  end

  local ok_n, fail_n = 0, 0
  local function walk(root, depth)
    if depth > 6 then return end
    local ok, iter, state = pcall(lfs.dir, root)
    if not ok or not iter then return end
    for entry in iter, state do
      if entry ~= "." and entry ~= ".." then
        local sub = root .. "/" .. entry
        local mode = lfs.attributes(sub, "mode")
        if mode == "directory" then
          walk(sub, depth + 1)
        elseif mode == "file" and _is_template_file(entry) then
          if not _tpl_cache[sub] then
            local tpl = LoadTemplate(sub)
            if tpl then
              _tpl_cache[sub] = tpl
              ok_n = ok_n + 1
            else
              fail_n = fail_n + 1
              Log("Templates.preload: 加载失败 " .. sub)
            end
          end
        end
      end
    end
  end
  walk(Templates.dir, 1)
  Log(string.format("Templates.preload: 成功 %d, 失败 %d, 根目录 %s",
                    ok_n, fail_n, Templates.dir))
  return ok_n, fail_n
end

--- 列出当前已缓存的模板及其元信息。
-- 返回数组 {{path=, w=, h=, valid=}, ...}，按路径排序，方便排错。
function Templates.list()
  local out = {}
  for abs, tpl in pairs(_tpl_cache) do
    if tpl and tpl._t ~= nil then
      out[#out + 1] = {
        path  = abs,
        w     = tpl:Width(),
        h     = tpl:Height(),
        valid = tpl:ValidPx(),
      }
    end
  end
  table.sort(out, function(a, b) return a.path < b.path end)
  return out
end

--- 顶层别名：与 LoadTemplate / FindPic 同级，方便快速调用。
-- 等同于 Templates.preload()，遍历当前 Templates.dir 加载所有 png/bmp/jpg/jpeg。
-- @return ok_n, fail_n
function PreloadTemplates()
  return Templates.preload()
end

-- ----------------------------------------------------------------
-- Images.FindPic（找图）：单次 / 找全部
-- ----------------------------------------------------------------
--
-- 这两个函数与 FindColor / FindMultiColorsAll 完全对齐：
--   - 复用同一份 _xy_buf / _xy_all_buf，不在每次调用里分配新的 cdata 数组
--   - x2 / y2 = 0 走 native 的全屏边界
--   - sim / dir 缺省走 0.9 / 0
--   - 模板参数 tpl 通常是 LoadTemplate 返回的元表对象（带 ._t 字段），
--     这里同时也兼容直接传 cdata 指针的防御性写法

-- 把用户传进来的模板参数解包成内部 cdata 指针。
--   - nil           → nil（视为未提供 / 已释放）
--   - string        → 走 Templates.get(name) 自动加载并缓存（找不到返回 nil）
--   - table（带 _t）→ table._t（已 :Free() / __close 后会是 nil）
--   - cdata         → 原样返回（用户绕开 _wrap_template 直接持有指针）
--   - 其它类型      → nil（视为非法）
local function _unwrap_template(tpl)
  if tpl == nil then return nil end
  local tt = type(tpl)
  if tt == "string" then
    local obj, err = Templates.get(tpl)
    if not obj then
      if err then Log(err) end
      return nil
    end
    return obj._t
  end
  if tt == "table" then
    return tpl._t                            -- 可能为 nil：模板已被 :Free() 释放
  elseif tt == "cdata" then
    return tpl
  end
  return nil
end

--- 解析 6 位 hex 色差字符串 "RRGGBB" 为 dr/dg/db。
-- 失败返回 nil。空串、nil、非字符串视为 nil（让调用方走 SAD 模式）。
local function _parse_delta(delta)
  if type(delta) ~= "string" or #delta == 0 then return nil end
  -- 兼容 "0xRRGGBB" 与 "RRGGBB"
  local s = delta:gsub("^0[xX]", "")
  if #s ~= 6 then return nil end
  local n = tonumber(s, 16)
  if not n then return nil end
  local dr = math.floor(n / 0x10000) % 256
  local dg = math.floor(n / 0x100) % 256
  local db = n % 256
  return dr, dg, db
end

--- 解析 "A.png|B.png|..." 多模板路径，返回 cdata 指针数组与个数。
-- 失败（任一模板加载失败）返回 nil。注意：单文件名（无 '|'）也会走这条路，
-- 调用方需要先用 string.find 判断是否含 '|' 决定走单 / 多模板 API。
local function _resolve_multi_templates(s)
  -- 切分（不依赖 string.gmatch 的复杂用法，简单循环）
  local names = {}
  local i = 1
  while true do
    local sep = s:find("|", i, true)
    if not sep then
      names[#names + 1] = s:sub(i)
      break
    end
    names[#names + 1] = s:sub(i, sep - 1)
    i = sep + 1
  end

  local n = #names
  if n == 0 then return nil end

  -- 扩容 cdata 数组缓冲
  if n > _tpl_arr_cap then
    local new_cap = n
    if new_cap < 8 then new_cap = 8 end
    _tpl_arr_buf = ffi.new(_tpl_arr_type, new_cap)
    _tpl_arr_cap = new_cap
  end

  -- 逐个 Templates.get；任一失败立即放弃（不让"半数失败"导致命中错位）
  for k = 1, n do
    local nm = names[k]
    if nm == nil or nm == "" then
      Log("FindPic: 多模板路径含空段，已跳过解析")
      return nil
    end
    local obj, err = Templates.get(nm)
    if not obj then
      if err then Log(err) end
      return nil
    end
    _tpl_arr_buf[k - 1] = obj._t
  end
  return _tpl_arr_buf, n
end

--- 区域找图，返回 x, y。找不到 / 模板无效 / 已释放时返回 -1, -1。
-- x2 / y2 = 0 表示用屏幕边界。
--
-- tpl 支持四种形式：
--   1) 模板对象（LoadTemplate / Templates.get 返回值）
--   2) 单文件名字符串      "btn_ok.png"        — 走 Templates 缓存
--   3) 多文件名字符串      "A.png|B.png|C.png" — C 内单次扫描，命中任一即返
--   4) cdata（防御写法）   直接的 EasyLuaTemplate *
--
-- 双签名（按第 6 个参数类型自动 dispatch）：
--   1) FindPic(x1, y1, x2, y2, tpl, sim, dir)               -- SAD 模式
--   2) FindPic(x1, y1, x2, y2, tpl, "RRGGBB", sim, dir)     -- 色差模式
function FindPic(x1, y1, x2, y2, tpl, a6, a7, a8)
  local dr, dg, db = _parse_delta(a6)

  -- 多模板分支：tpl 是字符串且含 '|'
  if type(tpl) == "string" and tpl:find("|", 1, true) then
    local arr, n = _resolve_multi_templates(tpl)
    if not arr then return -1, -1 end
    if dr then
      local sim = a7 or 0.9
      local dir = a8 or 0
      C.EasyLua_Images_FindPicMultiDelta(x1, y1, x2 or 0, y2 or 0,
                                         arr, n, dr, dg, db, sim, dir,
                                         _xy_buf, _xy_buf + 1, _idx_buf)
    else
      local sim = a6 or 0.9
      local dir = a7 or 0
      C.EasyLua_Images_FindPicMulti(x1, y1, x2 or 0, y2 or 0,
                                    arr, n, sim, dir,
                                    _xy_buf, _xy_buf + 1, _idx_buf)
    end
    return _xy_buf[0], _xy_buf[1], _idx_buf[0]
  end

  -- 单模板分支
  local t = _unwrap_template(tpl)
  if t == nil then return -1, -1 end

  if dr then
    local sim = a7 or 0.9
    local dir = a8 or 0
    C.EasyLua_Images_FindPicDelta(x1, y1, x2 or 0, y2 or 0,
                                  t, dr, dg, db, sim, dir,
                                  _xy_buf, _xy_buf + 1)
  else
    local sim = a6 or 0.9
    local dir = a7 or 0
    C.EasyLua_Images_FindPic(x1, y1, x2 or 0, y2 or 0,
                             t, sim, dir,
                             _xy_buf, _xy_buf + 1)
  end
  return _xy_buf[0], _xy_buf[1]
end

--- 区域找图 - 找全部位置，返回 table 列表，最多 256 个点。
-- 模板无效 / 已释放 / 未命中时返回空表 {}。
--
-- tpl 支持形式同 FindPic（包括 "A.png|B.png" 多模板）。
-- 双签名同 FindPic。
--
-- 返回元素结构：
--   单模板：{x=, y=}
--   多模板："A.png|B.png" 时元素含 idx（0 起的命中模板下标）：{x=, y=, idx=}
function FindPicAll(x1, y1, x2, y2, tpl, a6, a7)
  local dr, dg, db = _parse_delta(a6)

  if type(tpl) == "string" and tpl:find("|", 1, true) then
    local arr, n_tpl = _resolve_multi_templates(tpl)
    if not arr then return {} end
    local n
    if dr then
      local sim = a7 or 0.9
      n = C.EasyLua_Images_FindPicAllMultiDelta(x1, y1, x2 or 0, y2 or 0,
                                                arr, n_tpl, dr, dg, db, sim,
                                                _xy_all_buf, _idx_all_buf, 256)
    else
      local sim = a6 or 0.9
      n = C.EasyLua_Images_FindPicAllMulti(x1, y1, x2 or 0, y2 or 0,
                                           arr, n_tpl, sim,
                                           _xy_all_buf, _idx_all_buf, 256)
    end
    if n <= 0 then return {} end
    local list = {}
    for i = 0, n - 1 do
      list[i + 1] = {
        x = _xy_all_buf[i * 2],
        y = _xy_all_buf[i * 2 + 1],
        idx = _idx_all_buf[i],
      }
    end
    return list
  end

  local t = _unwrap_template(tpl)
  if t == nil then return {} end

  local n
  if dr then
    local sim = a7 or 0.9
    n = C.EasyLua_Images_FindPicAllDelta(x1, y1, x2 or 0, y2 or 0,
                                         t, dr, dg, db, sim,
                                         _xy_all_buf, 256)
  else
    local sim = a6 or 0.9
    n = C.EasyLua_Images_FindPicAll(x1, y1, x2 or 0, y2 or 0,
                                    t, sim,
                                    _xy_all_buf, 256)
  end
  if n <= 0 then return {} end
  local list = {}
  for i = 0, n - 1 do
    list[i + 1] = { x = _xy_all_buf[i * 2], y = _xy_all_buf[i * 2 + 1] }
  end
  return list
end

-- ====================================================
-- Motion（触摸事件注入）
-- ====================================================

local _motion_ready = false
local function _ensure_motion()
  if _motion_ready then return true end
  -- ScreenWidth/Height 在 native 端已经会自动等首帧（最多 3s），
  -- 这里调用就能拿到真实分辨率；返回 0 表示视频流挂了。
  local w = C.EasyLua_Screen_Width()
  local h = C.EasyLua_Screen_Height()
  if w <= 0 or h <= 0 then
    Log("Motion: 等不到首帧，无法获取分辨率")
    return false
  end
  if C.EasyLua_Motion_Init(w, h) ~= 0 then
    Log("Motion: uinput 初始化失败（需要 root + /dev/uinput）")
    return false
  end
  _motion_ready = true
  return true
end

--- 单点点击，duration 默认 50ms
function Click(x, y, duration)
  if not _ensure_motion() then return false end
  duration = duration or 50
  C.EasyLua_Motion_Down(x, y, 0, 0)
  C.EasyLua_Utils_Sleep(duration)
  C.EasyLua_Motion_Up(0)
  return true
end

--- 长按，duration 默认 800ms
function LongClick(x, y, duration)
  return Click(x, y, duration or 800)
end

--- 直线滑动
function Swipe(x1, y1, x2, y2, duration)
  if not _ensure_motion() then return false end
  C.EasyLua_Motion_Swipe(x1, y1, x2, y2, duration or 300, 0, 0)
  return true
end

--- 贝塞尔曲线滑动（更接近真人）
function SwipeBezier(x1, y1, x2, y2, duration)
  if not _ensure_motion() then return false end
  C.EasyLua_Motion_SwipeBezier(x1, y1, x2, y2, duration or 300, 0, 0)
  return true
end

--- 多指：按下 / 移动 / 抬起
-- 这三个用 `TouchDown / TouchMove / TouchUp` 而非裸 Down/Move/Up，
-- 避免和用户脚本里常见的局部变量名冲突。
function TouchDown(x, y, finger)
  if not _ensure_motion() then return false end
  return C.EasyLua_Motion_Down(x, y, finger or 0, 0) == 0
end

function TouchMove(x, y, finger)
  if not _ensure_motion() then return false end
  return C.EasyLua_Motion_Move(x, y, finger or 0, 0) == 0
end

function TouchUp(finger)
  if not _ensure_motion() then return false end
  return C.EasyLua_Motion_Up(finger or 0) == 0
end

-- ====================================================
-- Ui（自绘 Toast + Highlight）
-- 完全在 root 进程内用 Canvas 绘制，无需 APK，不挑 ROM
-- ====================================================

--- 显示 Toast（屏幕底部圆角矩形 + 白字，2 秒自动消失）
-- @param msg     文本
-- @param x       屏幕 x 坐标（省略或 -1 表示自动居中）
-- @param y       屏幕 y 坐标（省略或 -1 表示自动居底）
-- @param dur     显示时长 ms（默认 2000）
function Toast(msg, x, y, dur)
  return C.EasyLua_Ui_Toast(tostring(msg or ""), x or -1, y or -1, dur or 2000) == 0
end

--- 显示 highlight 矩形（SikuliX 风格）
-- @param x, y, w, h    矩形位置和大小
-- @param color         ARGB 颜色字符串 "#RRGGBB" / "#AARRGGBB"，省略 = 红色
-- @param dur           显示时长 ms（默认 2000，<=0 表示常驻）
-- @param label         矩形上方的文字标签（可省）
function Highlight(x, y, w, h, color, dur, label)
  local argb = 0
  if type(color) == "string" then
    local s = color:gsub("^#", "")
    if #s == 6 then s = "FF" .. s end
    argb = tonumber(s, 16) or 0xFFFF0000
  elseif type(color) == "number" then
    argb = color
  end
  return C.EasyLua_Ui_Highlight(x, y, w, h, argb, dur or 2000, label or "") == 0
end

--- 立即清掉所有 highlight
function HighlightOff()
  C.EasyLua_Ui_HighlightOff()
end

-- ====================================================
-- App（应用信息 + 启停）
-- ====================================================

local _str_buf_256 = ffi.new("char[?]", 256)
local _str_buf_2k  = ffi.new("char[?]", 2048)

--- 获取当前前台应用包名
function CurrentPackage()
  local n = C.EasyLua_App_CurrentPackage(_str_buf_256, 256)
  if n <= 0 then return "" end
  return ffi.string(_str_buf_256, n)
end

--- 获取当前前台应用 Activity 类名
function CurrentActivity()
  local n = C.EasyLua_App_CurrentActivity(_str_buf_256, 256)
  if n <= 0 then return "" end
  return ffi.string(_str_buf_256, n)
end

function Launch(pkg)         return C.EasyLua_App_Launch(pkg or "") == 1 end
function IsInstalled(pkg)    return C.EasyLua_App_IsInstalled(pkg or "") == 1 end
function ForceStop(pkg)      return C.EasyLua_App_ForceStop(pkg or "") == 1 end
function Clear(pkg)          return C.EasyLua_App_Clear(pkg or "") == 1 end
function OpenUrl(url)        return C.EasyLua_App_OpenUrl(url or "") == 1 end

-- ====================================================
-- Device（设备状态 + 系统操作）
-- ====================================================

function IsScreenOn()       return C.EasyLua_Device_IsScreenOn() == 1 end
function IsScreenUnlock()   return C.EasyLua_Device_IsScreenUnlock() == 1 end
function WakeUp()           C.EasyLua_Device_WakeUp() end

--- 熄屏（按电源键）。
-- 注意：不叫 Sleep，因为 Sleep 在顶层是 "延迟 N 毫秒"。
function ScreenOff()        C.EasyLua_Device_Sleep() end

function GetBattery()       return C.EasyLua_Device_GetBattery() end
function GetBatteryStatus() return C.EasyLua_Device_GetBatteryStatus() end
function Vibrate(ms)        C.EasyLua_Device_Vibrate(ms or 200) end
function GetSdkInt()        return C.EasyLua_Device_GetSdkInt() end

function GetBrand()
  local n = C.EasyLua_Device_GetBrand(_str_buf_256, 256)
  if n <= 0 then return "" end
  return ffi.string(_str_buf_256, n)
end

function GetModel()
  local n = C.EasyLua_Device_GetModel(_str_buf_256, 256)
  if n <= 0 then return "" end
  return ffi.string(_str_buf_256, n)
end

-- ====================================================
-- IME（剪贴板 + 文本输入 + 按键）
-- ====================================================

function GetClipText()
  local n = C.EasyLua_IME_GetClipText(_str_buf_2k, 2048)
  if n <= 0 then return "" end
  return ffi.string(_str_buf_2k, n)
end

function SetClipText(text)     return C.EasyLua_IME_SetClipText(text or "") == 1 end
function InputText(text)       return C.EasyLua_IME_InputText(text or "") == 1 end
function KeyAction(keycode)    return C.EasyLua_IME_KeyAction(keycode or 0) == 1 end

-- 常用 KeyEvent 常量（顶层全局）。前缀 KEY_ 避免 BACK / HOME 这种短词撞业务命名。
KEY_HOME       = 3
KEY_BACK       = 4
KEY_VOLUME_UP  = 24
KEY_VOLUME_DOWN= 25
KEY_POWER      = 26
KEY_MENU       = 82
KEY_ENTER      = 66
KEY_DEL        = 67
KEY_RECENTS    = 187
KEY_SEARCH     = 84
KEY_DPAD_UP    = 19
KEY_DPAD_DOWN  = 20
KEY_DPAD_LEFT  = 21
KEY_DPAD_RIGHT = 22

-- ====================================================
-- Shell（直接执行系统命令）
-- ====================================================

local _shell_buf = ffi.new("char[?]", 16384)

--- 执行 shell 命令，返回 stdout 字符串
function Exec(cmd)
  local n = C.EasyLua_Shell_Exec(cmd or "", _shell_buf, 16384)
  if n <= 0 then return "" end
  return ffi.string(_shell_buf, n)
end

-- ====================================================
-- Net（TCP / UDP / HTTP / DNS）
-- ====================================================
--
-- 设计：
--   - 顶层全局函数：TcpOpen / UdpOpen / HttpGet / HttpPost / HttpRequest /
--                  DnsResolve / LocalIp
--   - TcpOpen / UdpOpen 返回带 metatable 的句柄对象，方法链写起来顺手：
--       local s = TcpOpen("example.com", 80, 3000)
--       s:Send("GET / HTTP/1.0\r\nHost: example.com\r\n\r\n")
--       local data = s:Recv(4096, 3000)
--       s:Close()
--   - HTTP：http:// 走纯 socket（native 直发请求）；https:// 走 curl 子进程
--   - 字符串结果都是 lua string，调用方不必管 ffi.string

local _net_recv_cap = 65536  -- 单次 Recv 最大值；过大就分多次调
local _net_recv_buf = ffi.new("char[?]", _net_recv_cap)

local _net_ip_buf  = ffi.new("char[64]")
local _net_port_buf= ffi.new("int[1]")

-- ---------- TCP ----------

local _TcpMT
local function _wrap_tcp(fd)
  if fd < 0 then return nil end
  local sock = { _fd = fd }
  -- 用 ffi.gc 挂在一个 cdata holder 上，确保 GC 时自动 close
  local holder = ffi.new("int[1]", fd)
  ffi.gc(holder, function(h)
    if h[0] >= 0 then
      C.EasyLua_Net_Close(h[0])
      h[0] = -1
    end
  end)
  sock._holder = holder
  return setmetatable(sock, _TcpMT)
end

_TcpMT = {
  __index = {
    --- 发送数据。data 是 lua string 或 cdata buffer。
    -- @param timeout_ms 0/省略 = 用 SetTimeout 设的（无则一直阻塞）
    -- @return 发送字节数；-1 失败
    Send = function(self, data, timeout_ms)
      if self._fd < 0 then return -1 end
      local s = type(data) == "string" and data or tostring(data)
      return C.EasyLua_Net_TcpSend(self._fd, s, #s, timeout_ms or 0)
    end,

    --- 接收最多 cap 字节，返回 lua string
    -- @param cap         缓冲容量上限（默认 65536）
    -- @param timeout_ms  0/省略 = 用 SetTimeout 设的
    -- @return  string（可能为空字符串表示对端关闭）；nil = 错误/超时
    Recv = function(self, cap, timeout_ms)
      if self._fd < 0 then return nil end
      cap = cap or _net_recv_cap
      if cap > _net_recv_cap then cap = _net_recv_cap end
      local n = C.EasyLua_Net_TcpRecv(self._fd, _net_recv_buf, cap, timeout_ms or 0)
      if n < 0 then return nil end
      return ffi.string(_net_recv_buf, n)
    end,

    --- 接收恰好 n 字节（少了会循环 Recv 直到拿满或对端关闭）。
    -- 不够 n 字节就返回所拿到的 string（可能为空）；错误返回 nil。
    RecvExact = function(self, n, timeout_ms)
      if self._fd < 0 or n <= 0 then return "" end
      local parts = {}
      local got = 0
      while got < n do
        local need = n - got
        if need > _net_recv_cap then need = _net_recv_cap end
        local got_n = C.EasyLua_Net_TcpRecv(self._fd, _net_recv_buf, need,
                                            timeout_ms or 0)
        if got_n < 0 then return nil end
        if got_n == 0 then break end  -- 对端关闭
        parts[#parts + 1] = ffi.string(_net_recv_buf, got_n)
        got = got + got_n
      end
      return table.concat(parts)
    end,

    --- 设置 TCP_NODELAY（关闭 Nagle）。脚本里发心跳/小包时建议开。
    SetNoDelay = function(self, on)
      if self._fd < 0 then return false end
      return C.EasyLua_Net_TcpSetNoDelay(self._fd, on and 1 or 0) == 0
    end,

    --- 设置 recv / send 超时（毫秒）。0 = 无超时（永远阻塞）。
    -- send_ms 省略时用 recv_ms。
    SetTimeout = function(self, recv_ms, send_ms)
      if self._fd < 0 then return false end
      return C.EasyLua_Net_SetTimeout(self._fd, recv_ms or 0,
                                      send_ms or recv_ms or 0) == 0
    end,

    --- 关闭。重复关闭安全，关闭后所有方法返回 nil/失败。
    Close = function(self)
      local h = self._holder
      if h and h[0] >= 0 then
        C.EasyLua_Net_Close(h[0])
        h[0] = -1
      end
      self._fd = -1
    end,
  },
  -- to-be-closed：local s <close> = TcpOpen(...) 离开作用域自动关
  __close = function(self) self:Close() end,
  __gc    = function(self) self:Close() end,
}

--- 建连 TCP，返回 socket 对象；失败返回 nil。
-- @param host        IP 或域名
-- @param port        端口
-- @param timeout_ms  连接超时；省略走系统默认
function TcpOpen(host, port, timeout_ms)
  local fd = C.EasyLua_Net_TcpConnect(host or "", port or 0, timeout_ms or 0)
  if fd < 0 then return nil end
  return _wrap_tcp(fd)
end

-- ---------- UDP ----------

local _UdpMT

local function _wrap_udp(fd)
  if fd < 0 then return nil end
  local sock = { _fd = fd }
  local holder = ffi.new("int[1]", fd)
  ffi.gc(holder, function(h)
    if h[0] >= 0 then
      C.EasyLua_Net_Close(h[0])
      h[0] = -1
    end
  end)
  sock._holder = holder
  return setmetatable(sock, _UdpMT)
end

_UdpMT = {
  __index = {
    SendTo = function(self, host, port, data)
      if self._fd < 0 then return -1 end
      local s = type(data) == "string" and data or tostring(data)
      return C.EasyLua_Net_UdpSendTo(self._fd, host, port, s, #s)
    end,

    --- 接收一个 datagram。返回 (data, srcHost, srcPort)；失败/超时返回 nil。
    RecvFrom = function(self, cap, timeout_ms)
      if self._fd < 0 then return nil end
      cap = cap or _net_recv_cap
      if cap > _net_recv_cap then cap = _net_recv_cap end
      _net_ip_buf[0] = 0
      _net_port_buf[0] = 0
      local n = C.EasyLua_Net_UdpRecvFrom(self._fd, _net_recv_buf, cap,
                                          timeout_ms or 0,
                                          _net_ip_buf, 64, _net_port_buf)
      if n < 0 then return nil end
      return ffi.string(_net_recv_buf, n),
             ffi.string(_net_ip_buf),
             tonumber(_net_port_buf[0])
    end,

    SetTimeout = function(self, recv_ms, send_ms)
      if self._fd < 0 then return false end
      return C.EasyLua_Net_SetTimeout(self._fd, recv_ms or 0,
                                      send_ms or recv_ms or 0) == 0
    end,

    Close = function(self)
      local h = self._holder
      if h and h[0] >= 0 then
        C.EasyLua_Net_Close(h[0])
        h[0] = -1
      end
      self._fd = -1
    end,
  },
  __close = function(self) self:Close() end,
  __gc    = function(self) self:Close() end,
}

--- 创建 UDP socket。失败返回 nil。
function UdpOpen()
  return _wrap_udp(C.EasyLua_Net_UdpOpen())
end

-- ---------- DNS / 本机 IP ----------

local _net_str_buf = ffi.new("char[?]", 256)

--- 域名解析为 IPv4 字符串。失败返回 nil。
function DnsResolve(host)
  local n = C.EasyLua_Net_DnsResolve(host or "", _net_str_buf, 256)
  if n <= 0 then return nil end
  return ffi.string(_net_str_buf, n)
end

--- 取本机非 loopback 的 IPv4 地址。优先 wlan0。失败返回 nil。
function LocalIp()
  local n = C.EasyLua_Net_LocalIp(_net_str_buf, 256)
  if n <= 0 then return nil end
  return ffi.string(_net_str_buf, n)
end

-- ---------- HTTP ----------
--
-- 内部统一走 native HttpRequest：http:// 用纯 socket，https:// fallback 到 curl。
-- 高阶 API 提供 GET / POST，也保留 HttpRequest({...}) 全控版本。

local _HTTP_RESP_CAP = 1024 * 1024     -- 单次响应最大 1 MB
local _http_resp_buf = ffi.new("char[?]", _HTTP_RESP_CAP)
local _http_status   = ffi.new("int[1]")

-- 把 headers table -> "K: V\r\nK: V" 字符串
local function _http_headers_str(t)
  if not t then return nil end
  if type(t) == "string" then return t end
  local lines = {}
  for k, v in pairs(t) do
    lines[#lines + 1] = tostring(k) .. ": " .. tostring(v)
  end
  return table.concat(lines, "\r\n")
end

--- 通用 HTTP 请求。
-- @param opts.method      默认 "GET"
-- @param opts.url         必填
-- @param opts.headers     table 或 string，例 { ["Content-Type"]="application/json" }
-- @param opts.body        请求体 string；GET/HEAD 一般不传
-- @param opts.timeout_ms  连接 + 收发总超时；省略走系统默认
-- @return body, status, err
--   成功：body=string, status=int (HTTP 状态码), err=nil
--   失败：nil, nil, errMsg
function HttpRequest(opts)
  if type(opts) ~= "table" or type(opts.url) ~= "string" or opts.url == "" then
    return nil, nil, "HttpRequest: opts.url required"
  end
  local method = opts.method or "GET"
  local headers = _http_headers_str(opts.headers)
  local body = opts.body
  local body_len = body and #body or 0
  _http_status[0] = 0
  local n = C.EasyLua_Net_HttpRequest(method, opts.url, headers,
                                      body, body_len,
                                      _http_resp_buf, _HTTP_RESP_CAP,
                                      _http_status, opts.timeout_ms or 0)
  if n < 0 then
    return nil, nil, "HTTP request failed (see stderr)"
  end
  -- n 可能 > _HTTP_RESP_CAP：表示原始 body 比 buffer 大，调用方根据需要重发
  local copy = n < _HTTP_RESP_CAP and n or _HTTP_RESP_CAP
  return ffi.string(_http_resp_buf, copy), tonumber(_http_status[0]), nil
end

--- HTTP GET。返回 body, status, err。
function HttpGet(url, headers, timeout_ms)
  return HttpRequest{
    method = "GET", url = url, headers = headers, timeout_ms = timeout_ms,
  }
end

--- HTTP POST。body 可以是 string 或 table（table 自动 cjson.encode 并默认带
-- Content-Type: application/json）。
function HttpPost(url, body, headers, timeout_ms)
  local h = headers
  if type(body) == "table" then
    body = (rawget(_G, "cjson") and _G.cjson.encode or tostring)(body)
    -- 把 headers 转成 table 以便插 Content-Type
    if type(h) == "string" then
      h = h .. "\r\nContent-Type: application/json"
    else
      h = h or {}
      local has_ct = false
      for k in pairs(h) do
        if type(k) == "string" and k:lower() == "content-type" then
          has_ct = true; break
        end
      end
      if not has_ct then h["Content-Type"] = "application/json" end
    end
  end
  return HttpRequest{
    method = "POST", url = url, headers = h, body = body,
    timeout_ms = timeout_ms,
  }
end

-- ====================================================
-- cjson（纯 Lua JSON，API 兼容 lua-cjson）
-- ====================================================
--
-- 之所以内置：脚本写 HTTP 接口、序列化配置、调试输出等都需要 JSON；
-- 系统不带 lua-cjson 的 .so，从源码编进引擎又会让 .so 大几十 KB。
-- 用纯 Lua 实现常规 encode/decode 已经够用（性能比 cjson C 版慢约 5-10x，
-- 但脚本场景每次几百字节 JSON 完全无感）。
--
-- 暴露方式：
--   require("cjson")       → 返回 cjson 表（与 luarocks 版一致）
--   全局 cjson 表           → 也直接可用
--
-- 提供：
--   cjson.encode(v)        → string，table/数字/布尔/字符串/nil 全支持
--   cjson.decode(s)        → 任意 lua 值
--   cjson.null             → 用于在 encode 中表示 JSON null
-- 不提供（脚本侧用不上）：array/object hint、稀疏数组配置、多 decoder 实例

local cjson = {}
cjson.null = setmetatable({}, { __tostring = function() return "null" end })

-- ---------- encode ----------

local _enc_escape_map = {
  ['"']  = '\\"',
  ['\\'] = '\\\\',
  ['/']  = '\\/',
  ['\b'] = '\\b',
  ['\f'] = '\\f',
  ['\n'] = '\\n',
  ['\r'] = '\\r',
  ['\t'] = '\\t',
}

local function _enc_string(s)
  return '"' .. s:gsub('[%z\1-\31\\"]', function(c)
    return _enc_escape_map[c] or string.format("\\u%04x", c:byte())
  end) .. '"'
end

-- 判断 table 是不是"数组":键 1..n 全部存在且没有别的键
local function _is_array(t)
  local n = 0
  for _ in pairs(t) do n = n + 1 end
  if n == 0 then return false, 0 end  -- 空表当 object（避免 {} 与 [] 混淆）
  for i = 1, n do
    if t[i] == nil then return false, n end
  end
  return true, n
end

local _enc_value  -- forward
local function _enc_array(t, n, pretty, indent)
  if n == 0 then return "[]" end
  if not pretty then
    local parts = {}
    for i = 1, n do parts[i] = _enc_value(t[i], false, "") end
    return "[" .. table.concat(parts, ",") .. "]"
  end
  local sub = indent .. "  "
  local parts = {}
  for i = 1, n do parts[i] = sub .. _enc_value(t[i], true, sub) end
  return "[\n" .. table.concat(parts, ",\n") .. "\n" .. indent .. "]"
end

local function _enc_object(t, pretty, indent)
  -- 收集 key 排序，输出稳定（方便 diff）
  local keys = {}
  for k in pairs(t) do keys[#keys + 1] = k end
  if #keys == 0 then return "{}" end
  table.sort(keys, function(a, b) return tostring(a) < tostring(b) end)
  if not pretty then
    local parts = {}
    for i, k in ipairs(keys) do
      parts[i] = _enc_string(tostring(k)) .. ":" .. _enc_value(t[k], false, "")
    end
    return "{" .. table.concat(parts, ",") .. "}"
  end
  local sub = indent .. "  "
  local parts = {}
  for i, k in ipairs(keys) do
    parts[i] = sub .. _enc_string(tostring(k)) .. ": " .. _enc_value(t[k], true, sub)
  end
  return "{\n" .. table.concat(parts, ",\n") .. "\n" .. indent .. "}"
end

_enc_value = function(v, pretty, indent)
  local t = type(v)
  if v == cjson.null then return "null" end
  if t == "nil"     then return "null" end
  if t == "boolean" then return v and "true" or "false" end
  if t == "number"  then
    if v ~= v or v == math.huge or v == -math.huge then return "null" end
    if v == math.floor(v) and math.abs(v) < 1e15 then
      return string.format("%d", v)
    end
    return tostring(v)
  end
  if t == "string"  then return _enc_string(v) end
  if t == "table"   then
    local is_arr, n = _is_array(v)
    if is_arr then return _enc_array(v, n, pretty, indent) end
    return _enc_object(v, pretty, indent)
  end
  -- function / userdata / thread / cdata：转字符串占位
  return _enc_string(tostring(v))
end

--- encode lua 值为 JSON 字符串。
-- @param v       任意值
-- @param pretty  true 输出多行缩进（2 空格），默认紧凑单行
function cjson.encode(v, pretty)
  return _enc_value(v, pretty == true, "")
end

-- ---------- decode ----------

local function _skip_ws(s, i)
  while true do
    local c = s:byte(i)
    if c == 32 or c == 9 or c == 10 or c == 13 then i = i + 1
    else return i end
  end
end

local _dec_value  -- forward

local function _dec_string(s, i)
  -- s[i] == '"'，已检过
  local j = i + 1
  local buf = {}
  while j <= #s do
    local c = s:byte(j)
    if c == 34 then  -- "
      return table.concat(buf), j + 1
    elseif c == 92 then  -- \
      local n = s:byte(j + 1)
      if     n ==  34 then buf[#buf+1] = '"'  ; j = j + 2
      elseif n ==  92 then buf[#buf+1] = '\\' ; j = j + 2
      elseif n ==  47 then buf[#buf+1] = '/'  ; j = j + 2
      elseif n ==  98 then buf[#buf+1] = '\b' ; j = j + 2
      elseif n == 102 then buf[#buf+1] = '\f' ; j = j + 2
      elseif n == 110 then buf[#buf+1] = '\n' ; j = j + 2
      elseif n == 114 then buf[#buf+1] = '\r' ; j = j + 2
      elseif n == 116 then buf[#buf+1] = '\t' ; j = j + 2
      elseif n == 117 then  -- \uXXXX
        local hex = s:sub(j + 2, j + 5)
        local code = tonumber(hex, 16)
        if not code then error("invalid \\u escape at " .. j) end
        -- 简化：只处理 BMP，转 UTF-8
        if code < 0x80 then
          buf[#buf+1] = string.char(code)
        elseif code < 0x800 then
          buf[#buf+1] = string.char(0xC0 + math.floor(code / 0x40),
                                    0x80 + (code % 0x40))
        else
          buf[#buf+1] = string.char(
            0xE0 +  math.floor(code / 0x1000),
            0x80 + (math.floor(code / 0x40) % 0x40),
            0x80 + (code % 0x40))
        end
        j = j + 6
      else error("invalid escape \\" .. string.char(n) .. " at " .. j) end
    else
      buf[#buf+1] = string.char(c)
      j = j + 1
    end
  end
  error("unterminated string at " .. i)
end

local function _dec_number(s, i)
  local j = i
  while j <= #s do
    local c = s:byte(j)
    if (c >= 48 and c <= 57) or c == 45 or c == 43 or c == 46
       or c == 69 or c == 101 then
      j = j + 1
    else break end
  end
  local n = tonumber(s:sub(i, j - 1))
  if n == nil then error("invalid number at " .. i) end
  return n, j
end

local function _dec_array(s, i)
  local arr = {}
  i = _skip_ws(s, i + 1)
  if s:byte(i) == 93 then return arr, i + 1 end  -- ]
  while true do
    local v, ni = _dec_value(s, i)
    arr[#arr + 1] = v
    i = _skip_ws(s, ni)
    local c = s:byte(i)
    if c == 44 then i = _skip_ws(s, i + 1)  -- ,
    elseif c == 93 then return arr, i + 1   -- ]
    else error("expect , or ] at " .. i) end
  end
end

local function _dec_object(s, i)
  local obj = {}
  i = _skip_ws(s, i + 1)
  if s:byte(i) == 125 then return obj, i + 1 end  -- }
  while true do
    if s:byte(i) ~= 34 then error("expect string key at " .. i) end
    local k, ni = _dec_string(s, i)
    i = _skip_ws(s, ni)
    if s:byte(i) ~= 58 then error("expect : at " .. i) end  -- :
    i = _skip_ws(s, i + 1)
    local v
    v, ni = _dec_value(s, i)
    obj[k] = v
    i = _skip_ws(s, ni)
    local c = s:byte(i)
    if c == 44 then i = _skip_ws(s, i + 1)
    elseif c == 125 then return obj, i + 1
    else error("expect , or } at " .. i) end
  end
end

_dec_value = function(s, i)
  i = _skip_ws(s, i)
  local c = s:byte(i)
  if c == 34 then return _dec_string(s, i) end                  -- "
  if c == 123 then return _dec_object(s, i) end                 -- {
  if c == 91 then return _dec_array(s, i) end                   -- [
  if c == 116 and s:sub(i, i + 3) == "true"  then return true,  i + 4 end
  if c == 102 and s:sub(i, i + 4) == "false" then return false, i + 5 end
  if c == 110 and s:sub(i, i + 3) == "null"  then return cjson.null, i + 4 end
  if c == 45 or (c >= 48 and c <= 57) then return _dec_number(s, i) end
  error("unexpected char at " .. tostring(i))
end

--- decode JSON 字符串。出错抛 lua error，包外面套 pcall 兜底。
function cjson.decode(s)
  if type(s) ~= "string" or s == "" then error("decode requires non-empty string") end
  local v, i = _dec_value(s, 1)
  -- 允许尾部空白；非空白则报错
  i = _skip_ws(s, i)
  if i <= #s then error("unexpected trailing content at " .. i) end
  return v
end

cjson._VERSION = "easylua-cjson 1.0 (pure-lua)"

-- 注册：全局 + package.preload，让 require("cjson") 也能拿到同一份
cjson_safe = cjson  -- 兼容 lua-cjson 的 _safe 别名（API 不分 strict/safe）
_G.cjson = cjson
if package and package.loaded then
  package.loaded["cjson"]      = cjson
  package.loaded["cjson.safe"] = cjson
end

-- ====================================================
-- LuaSocket 兼容 shim（require("socket")）
-- ====================================================
--
-- 设计取舍：
--   - 顶层 TcpOpen/UdpOpen/HttpGet 是 easyLua 推荐的扁平 API；
--   - require("socket") 是给"想抄 LuaSocket 教程 / 第三方库依赖"的人保留兼容路径；
--   - shim 只覆盖 BSD socket（tcp/udp/dns/select/sleep/gettime），**不**包含 HTTP，
--     避免和顶层 HttpGet/HttpPost 双轨——HTTP 用法只学一套。
--   - shim 内部直接调 EasyLua_Net_*，不走 TcpOpen 的 metatable，确保两种风格各跑各的。
--
-- 兼容覆盖（与 LuaSocket 3.0 行为对齐）：
--   socket.tcp()                        → 阻塞 TCP 客户端句柄
--     :connect(host, port)              → 1 / nil, err
--     :send(data [, i [, j]])           → 字节数 / nil, err
--     :receive(pattern [, prefix])      → "*l" / "*a" / N（数字字节数）
--     :close()
--     :settimeout(t [, mode])           t = 秒（或 ms 的 "b"/"t" 用法都按秒处理）
--     :setoption("tcp-nodelay", true)
--     :getsockname() / :getpeername()   尽力而为
--   socket.udp()
--     :sendto(data, host, port)
--     :receivefrom([cap])               → data, host, port / nil, "timeout"
--     :setsockname / :setpeername       尽力而为
--     :close / :settimeout
--   socket.dns.toip(host)               → ip, { ip = ip }
--   socket.gettime()                    → 单调时钟秒（带小数）
--   socket.sleep(s)                     → 阻塞 s 秒
--   socket.select(reads, writes, t)     → 简化版（仅 timeout 后的轮询语义）
--
-- 不实现：
--   * socket.http / socket.url / mime.* — 用顶层 HttpGet/HttpPost 替代
--   * unix domain socket / SCTP / 原始 socket
--   * 异步 / 半异步（LuaSocket 的 nonblocking + select 那套，脚本几乎用不上）

local socket = {}

-- ----- TCP -----
local _LSocketTcpMT
local function _new_lsocket_tcp(fd)
  local self = {
    _fd = fd or -1,
    _timeout_ms = 0,   -- 0 = 永远阻塞（LuaSocket 默认行为）
    _connected = false,
  }
  return setmetatable(self, _LSocketTcpMT)
end

_LSocketTcpMT = {
  __index = {
    --- LuaSocket 的 settimeout(t [, mode])。t 单位：秒（与 LuaSocket 一致）。
    -- mode "b"/"t"/省略 都按总超时处理（够用，不区分阻塞/非阻塞）。
    settimeout = function(self, t)
      if not t or t < 0 then self._timeout_ms = 0
      else self._timeout_ms = math.floor(t * 1000) end
      if self._fd >= 0 and self._connected then
        C.EasyLua_Net_SetTimeout(self._fd, self._timeout_ms, self._timeout_ms)
      end
      return 1
    end,

    connect = function(self, host, port)
      if self._fd >= 0 then return nil, "already connected" end
      local fd = C.EasyLua_Net_TcpConnect(host or "", port or 0, self._timeout_ms)
      if fd < 0 then return nil, "connection failed" end
      self._fd = fd
      self._connected = true
      if self._timeout_ms > 0 then
        C.EasyLua_Net_SetTimeout(fd, self._timeout_ms, self._timeout_ms)
      end
      return 1
    end,

    --- LuaSocket send(data [, i [, j]]) 部分实现：
    --   - i/j 切片支持，缺省 i=1, j=#data
    --   - 返回 (last_index_sent, nil)；失败 (nil, err, last_index_sent)
    send = function(self, data, i, j)
      if self._fd < 0 then return nil, "closed" end
      data = type(data) == "string" and data or tostring(data)
      i = i or 1
      j = j or #data
      if i > j then return j end
      local seg = data:sub(i, j)
      local n = C.EasyLua_Net_TcpSend(self._fd, seg, #seg, self._timeout_ms)
      if n < 0 then return nil, "send failed", i - 1 end
      return i + n - 1
    end,

    --- LuaSocket receive(pattern [, prefix])：
    --   - pattern 数字 N    收恰好 N 字节
    --   - "*a" / "a"        读到对端关闭
    --   - "*l" / "l" / nil  读一行（LF 结尾，丢弃 CR）
    -- 失败返回 (nil, err, partial)
    receive = function(self, pattern, prefix)
      if self._fd < 0 then return nil, "closed" end
      pattern = pattern or "*l"
      prefix = prefix or ""

      if type(pattern) == "number" then
        -- 收恰好 N 字节
        local need = pattern - #prefix
        if need <= 0 then return prefix end
        local parts = { prefix }
        local got = 0
        while got < need do
          local rem = need - got
          if rem > _net_recv_cap then rem = _net_recv_cap end
          local n = C.EasyLua_Net_TcpRecv(self._fd, _net_recv_buf, rem, self._timeout_ms)
          if n < 0 then return nil, "timeout", table.concat(parts) end
          if n == 0 then return nil, "closed", table.concat(parts) end
          parts[#parts + 1] = ffi.string(_net_recv_buf, n)
          got = got + n
        end
        return table.concat(parts)
      end

      if pattern == "*a" or pattern == "a" then
        local parts = { prefix }
        while true do
          local n = C.EasyLua_Net_TcpRecv(self._fd, _net_recv_buf, _net_recv_cap, self._timeout_ms)
          if n < 0 then return nil, "timeout", table.concat(parts) end
          if n == 0 then break end
          parts[#parts + 1] = ffi.string(_net_recv_buf, n)
        end
        return table.concat(parts)
      end

      -- "*l" / "l"：按字节读直到看到 \n
      local buf = { prefix }
      while true do
        local n = C.EasyLua_Net_TcpRecv(self._fd, _net_recv_buf, 1, self._timeout_ms)
        if n < 0 then return nil, "timeout", table.concat(buf) end
        if n == 0 then return nil, "closed", table.concat(buf) end
        local c = _net_recv_buf[0]
        if c == 10 then
          -- LF 结束；去掉末尾 CR
          local line = table.concat(buf)
          if #line > 0 and line:byte(#line) == 13 then line = line:sub(1, -2) end
          return line
        end
        buf[#buf + 1] = string.char(c)
      end
    end,

    setoption = function(self, name, val)
      if self._fd < 0 then return nil, "closed" end
      if name == "tcp-nodelay" or name == "TCP_NODELAY" then
        return C.EasyLua_Net_TcpSetNoDelay(self._fd, val and 1 or 0) == 0 and 1 or nil
      end
      -- 其它选项当前不支持，按 LuaSocket 习惯静默成功
      return 1
    end,

    -- LuaSocket 的 getsockname/getpeername 我们没拿到 native 实现；
    -- 给一个 best-effort 占位，绝大多数脚本只是打印调试用，能给 ip 就行
    getsockname = function(self) return LocalIp() or "0.0.0.0", 0 end,
    getpeername = function(self) return "?", 0 end,

    close = function(self)
      if self._fd >= 0 then
        C.EasyLua_Net_Close(self._fd)
        self._fd = -1
        self._connected = false
      end
      return 1
    end,

    -- 让 LuaSocket 风格脚本里的 fh:close() === gc 路径都工作
    shutdown = function(self) self:close() end,
  },
  __gc    = function(self) if self._fd >= 0 then C.EasyLua_Net_Close(self._fd) end end,
  __close = function(self) self:close() end,
  __tostring = function(self)
    return self._fd >= 0 and ("tcp{fd=" .. self._fd .. "}") or "tcp{closed}"
  end,
}

function socket.tcp() return _new_lsocket_tcp() end

-- LuaSocket 的便捷函数：socket.connect(host, port [, locaddr [, locport]])
function socket.connect(host, port)
  local s = _new_lsocket_tcp()
  local ok, err = s:connect(host, port)
  if not ok then s:close(); return nil, err end
  return s
end

-- ----- UDP -----
local _LSocketUdpMT
local function _new_lsocket_udp()
  local fd = C.EasyLua_Net_UdpOpen()
  if fd < 0 then return nil, "udp open failed" end
  local self = { _fd = fd, _timeout_ms = 0 }
  return setmetatable(self, _LSocketUdpMT)
end

_LSocketUdpMT = {
  __index = {
    settimeout = function(self, t)
      if not t or t < 0 then self._timeout_ms = 0
      else self._timeout_ms = math.floor(t * 1000) end
      if self._fd >= 0 then
        C.EasyLua_Net_SetTimeout(self._fd, self._timeout_ms, self._timeout_ms)
      end
      return 1
    end,

    sendto = function(self, data, host, port)
      if self._fd < 0 then return nil, "closed" end
      data = type(data) == "string" and data or tostring(data)
      local n = C.EasyLua_Net_UdpSendTo(self._fd, host, port, data, #data)
      if n < 0 then return nil, "send failed" end
      return n
    end,

    receivefrom = function(self, cap)
      if self._fd < 0 then return nil, "closed" end
      cap = cap or _net_recv_cap
      if cap > _net_recv_cap then cap = _net_recv_cap end
      _net_ip_buf[0] = 0
      _net_port_buf[0] = 0
      local n = C.EasyLua_Net_UdpRecvFrom(self._fd, _net_recv_buf, cap, self._timeout_ms,
                                          _net_ip_buf, 64, _net_port_buf)
      if n < 0 then return nil, "timeout" end
      return ffi.string(_net_recv_buf, n),
             ffi.string(_net_ip_buf),
             tonumber(_net_port_buf[0])
    end,

    -- "已连接 UDP"风格不实现，让 sendto/receivefrom 即可
    setpeername = function(self, host, port) self._peer = { host, port }; return 1 end,
    setsockname = function(self) return 1 end,
    getsockname = function(self) return LocalIp() or "0.0.0.0", 0 end,

    close = function(self)
      if self._fd >= 0 then C.EasyLua_Net_Close(self._fd); self._fd = -1 end
      return 1
    end,
  },
  __gc    = function(self) if self._fd >= 0 then C.EasyLua_Net_Close(self._fd) end end,
  __close = function(self) self:close() end,
}

function socket.udp() return _new_lsocket_udp() end

-- ----- DNS -----
socket.dns = {}

--- LuaSocket: ip, info = socket.dns.toip(host)
-- info 在原版是带 ip / alias 的 table；我们只填 { ip = ip }。
function socket.dns.toip(host)
  local ip = DnsResolve(host)
  if not ip then return nil, "host not found" end
  return ip, { ip = ip, name = host, alias = {}, ipaddr = { ip } }
end

--- 反向查询：我们没有 PTR 实现，返回 host 自身字符串占位。
function socket.dns.tohostname(addr)
  return addr, { name = addr, alias = {}, ipaddr = { addr } }
end

--- 取本机名（LuaSocket 的语义是 gethostname()），返回一个能解析到本机的字符串。
function socket.dns.gethostname()
  return LocalIp() or "localhost"
end

-- ----- 时间 / 睡眠 -----
function socket.gettime() return tonumber(C.EasyLua_Utils_NowUs()) / 1e6 end
function socket.sleep(s)
  if not s or s <= 0 then return end
  C.EasyLua_Utils_Sleep(math.floor(s * 1000))
end

-- ----- select（简化）-----
--
-- 真版 select 用 epoll / poll；我们没暴露多路复用。这里给一个"忙等 + per-socket
-- timedrecv 触发"的退化实现，仅用来兼容那种"只 select 一个 socket 等数据"的脚本。
-- 多于 1 个的 reads 数组会按顺序逐个用 1ms 超时探测。
function socket.select(reads, writes, timeout)
  reads = reads or {}
  writes = writes or {}
  -- writes：我们认为写总是可写（TCP 缓冲区一般够），直接全部回填
  local r_ready, w_ready = {}, {}
  for _, w in ipairs(writes) do w_ready[#w_ready + 1] = w end

  local deadline = (timeout and timeout > 0) and (socket.gettime() + timeout) or nil
  while true do
    for _, s in ipairs(reads) do
      if s and s._fd and s._fd >= 0 then
        -- peek 1 字节：用 MSG_PEEK 我们底层没暴露，退而求其次用很短的 timed recv，
        -- 但那会消费数据。这里只能保守：永远把 reads 全部当 ready 返回（让用户脚本
        -- 自己 receive 时用 settimeout(0.001) 兜底）。
        r_ready[#r_ready + 1] = s
      end
    end
    if #r_ready > 0 then break end
    if deadline and socket.gettime() >= deadline then break end
    socket.sleep(0.005)
  end
  return r_ready, w_ready, (#r_ready == 0 and #w_ready == 0) and "timeout" or nil
end

-- ----- 模块元信息 -----
socket._VERSION = "easylua-socket 1.0 (BSD compat shim)"
socket.BLOCKSIZE = 8192

-- 注册：require("socket") 拿到这个表
if package and package.loaded then
  package.loaded["socket"]      = socket
  package.loaded["socket.core"] = socket   -- 老脚本里偶尔会 require("socket.core")
end

-- ====================================================
-- print 增强：table 自动展成 JSON
-- ====================================================
--
-- 默认 print 对 table 输出 "table: 0x..." 看不到内容；
-- 改成"对每个 table 参数走 cjson.encode（带缩进），其它类型保持原样"。
-- 仍走 native 那条 print（带 [HH:MM:SS.mmm script.lua:line] 前缀），
-- 只是在 lua 层把参数预处理一下。

local _native_print = print  -- C 层注入的 lua_print_via_stdout
print = function(...)
  local n = select("#", ...)
  if n == 0 then return _native_print() end
  local args = {}
  for i = 1, n do
    local v = select(i, ...)
    if type(v) == "table" then
      -- table：encode pretty。失败兜底到原 tostring（避免循环引用之类把脚本搞挂）
      local ok, s = pcall(cjson.encode, v, true)
      args[i] = ok and s or tostring(v)
    else
      args[i] = v
    end
  end
  return _native_print((unpack or table.unpack)(args, 1, n))
end
