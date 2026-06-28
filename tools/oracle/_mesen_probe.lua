-- Diagnostic v2: find the real callback-type enum + a working exec registration.
local log = io.open(emu.getScriptDataFolder() .. "/probe2.txt", "w")
local function L(s) log:write(tostring(s) .. "\n"); log:flush() end

L("=== emu top-level keys ===")
for k, v in pairs(emu) do L("emu." .. tostring(k) .. " : " .. type(v)) end

for _, name in ipairs({"callbackType", "memCallbackType", "cpuMemCallbackType"}) do
  local t = emu[name]
  if type(t) == "table" then
    L("=== emu." .. name .. " ===")
    for k, v in pairs(t) do L("  " .. tostring(k) .. " = " .. tostring(v)) end
  else
    L("emu." .. name .. " : " .. type(t))
  end
end

local h_ct, h_int = 0, 0
local function try(desc, fn) local ok, err = pcall(fn); L("try " .. desc .. " ok=" .. tostring(ok) .. " err=" .. tostring(err)) end

if type(emu.callbackType) == "table" then
  try("callbackType.exec 0x0000-0xFFFF", function()
    emu.addMemoryCallback(function() h_ct = h_ct + 1 end, emu.callbackType.exec, 0x0000, 0xFFFF)
  end)
end
-- exec is commonly integer 2 (read=0, write=1, exec=2)
try("int type=2 0x0000-0xFFFF", function()
  emu.addMemoryCallback(function() h_int = h_int + 1 end, 2, 0x0000, 0xFFFF)
end)

local frame = 0
emu.addEventCallback(function()
  frame = frame + 1
  if frame >= 300 then
    L("after 300 frames: h_callbackType=" .. h_ct .. " h_int2=" .. h_int)
    log:close(); emu.stop(0)
  end
end, emu.eventType.endFrame)
