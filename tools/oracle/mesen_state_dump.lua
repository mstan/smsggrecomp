-- mesen_state_dump.lua -- dump CPU regs + PSG latched regs + VRAM + CRAM at a
-- LIST of frames in a single Mesen run (frame-tagged files), the oracle side of
-- the measurement sweep. Env: MESEN_DUMP_FRAMES="450,1200,1600,2000".
--
-- NOTE: Mesen getState() returns a FLAT table with dotted string keys
-- (st["cpu.a"], st["psg.tone[0].reloadValue"]) -- NOT nested. Verified via
-- _mesen_probe.lua on MesenCE 2.1.1.
local list = os.getenv("MESEN_DUMP_FRAMES") or "450"
local want = {}
local maxf = 0
for n in string.gmatch(list, "%d+") do
  local v = tonumber(n); want[v] = true; if v > maxf then maxf = v end
end
local dir = emu.getScriptDataFolder()

local function dumpbin(memType, path)
  local size = emu.getMemorySize(memType)
  local t = {}
  for a = 0, size - 1 do t[a + 1] = string.char(emu.read(a, memType)) end
  local f = io.open(path, "wb"); f:write(table.concat(t)); f:close()
end

local function b2i(v) return v and 1 or 0 end

local function dumpstate(frame)
  local st = emu.getState()
  local f = io.open(dir .. "/mesen_cpu_" .. frame .. ".txt", "w")
  f:write(string.format("a=%d\nf=%d\nb=%d\nc=%d\nd=%d\ne=%d\nh=%d\nl=%d\n",
    st["cpu.a"], st["cpu.flags"], st["cpu.b"], st["cpu.c"],
    st["cpu.d"], st["cpu.e"], st["cpu.h"], st["cpu.l"]))
  f:write(string.format("a_=%d\nf_=%d\nb_=%d\nc_=%d\nd_=%d\ne_=%d\nh_=%d\nl_=%d\n",
    st["cpu.altA"], st["cpu.altFlags"], st["cpu.altB"], st["cpu.altC"],
    st["cpu.altD"], st["cpu.altE"], st["cpu.altH"], st["cpu.altL"]))
  f:write(string.format("ix=%d\niy=%d\nsp=%d\npc=%d\nwz=%d\ni=%d\nr=%d\n" ..
                        "iff1=%d\niff2=%d\nim=%d\nhalted=%d\n",
    st["cpu.ixh"] * 256 + st["cpu.ixl"], st["cpu.iyh"] * 256 + st["cpu.iyl"],
    st["cpu.sp"], st["cpu.pc"], st["cpu.wz"], st["cpu.i"], st["cpu.r"],
    b2i(st["cpu.iff1"]), b2i(st["cpu.iff2"]), st["cpu.im"], b2i(st["cpu.halted"])))
  f:close()

  local g = io.open(dir .. "/mesen_psg_" .. frame .. ".txt", "w")
  g:write(string.format("tone0_period=%d\ntone1_period=%d\ntone2_period=%d\n",
    st["psg.tone[0].reloadValue"], st["psg.tone[1].reloadValue"], st["psg.tone[2].reloadValue"]))
  g:write(string.format("tone0_vol=%d\ntone1_vol=%d\ntone2_vol=%d\nnoise_vol=%d\n",
    st["psg.tone[0].volume"], st["psg.tone[1].volume"], st["psg.tone[2].volume"], st["psg.noise.volume"]))
  g:write(string.format("noise_ctrl=%d\nlfsr=%d\ngg_panning=%d\n",
    st["psg.noise.control"], st["psg.noise.lfsr"], st["psg.gameGearPanningReg"]))
  g:close()

  dumpbin(emu.memType.smsVideoRam,   dir .. "/mesen_vram_" .. frame .. ".bin")
  dumpbin(emu.memType.smsPaletteRam, dir .. "/mesen_cram_" .. frame .. ".bin")
  emu.log("state dump @frame " .. frame)
end

local frame = 0
emu.addEventCallback(function()
  frame = frame + 1
  if want[frame] then dumpstate(frame) end
  if frame >= maxf then emu.stop(0) end
end, emu.eventType.endFrame)
