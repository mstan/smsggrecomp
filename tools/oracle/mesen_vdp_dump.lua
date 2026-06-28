-- mesen_vdp_dump.lua -- dump SMS VDP memory (VRAM + CRAM) at a target frame,
-- the Mesen side of the Axis-4 MMIO/VDP state-surface byte diff (vs the recomp
-- <png>.vram/.cram raw dumps). Run:
--   Mesen.exe --testRunner mesen_vdp_dump.lua <rom.sms> --noaudio --doNotSaveSettings
-- Env: MESEN_DUMP_FRAME (default 450)
local FRAME = tonumber(os.getenv("MESEN_DUMP_FRAME")) or 450
local dir = emu.getScriptDataFolder()

local function dumpmem(memType, path)
  local size = emu.getMemorySize(memType)
  local t = {}
  for a = 0, size - 1 do t[a + 1] = string.char(emu.read(a, memType)) end
  local f = io.open(path, "wb"); f:write(table.concat(t)); f:close()
  return size
end

local frame = 0
emu.addEventCallback(function()
  frame = frame + 1
  if frame == FRAME then
    local nv = dumpmem(emu.memType.smsVideoRam,   dir .. "/mesen_vram.bin")
    local nc = dumpmem(emu.memType.smsPaletteRam, dir .. "/mesen_cram.bin")
    emu.log("vdp dump @frame " .. frame .. ": vram=" .. nv .. " cram=" .. nc)
    emu.stop(0)
  end
end, emu.eventType.endFrame)
