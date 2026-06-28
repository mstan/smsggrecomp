-- mesen_cyc_watch.lua -- record (hit, guest cycleCount) each time the CPU
-- executes an anchor PC. The Mesen side of the offset-independent per-anchor
-- cycle-delta comparison (vs the recomp --cyc-watch CSV; see tools/audio's
-- sibling cyc_compare.py).
--
-- Run: Mesen.exe --testRunner mesen_cyc_watch.lua <rom.sms> --noaudio --doNotSaveSettings
-- Env: MESEN_CYC_ANCHOR  (default 0x0038, the IM1/VBlank handler)
--      MESEN_CYC_MAXHITS (default 2000)
local ANCHOR  = tonumber(os.getenv("MESEN_CYC_ANCHOR")) or 0x0038
local MAXHITS = tonumber(os.getenv("MESEN_CYC_MAXHITS")) or 2000
local hits = 0
local path = emu.getScriptDataFolder() .. "/mesen_cycwatch.csv"
local f = io.open(path, "w")
f:write("hit,cyc\n")

local function cyc(st)
  if st.cpu and st.cpu.cycleCount then return st.cpu.cycleCount end
  if st.cpu and st.cpu.cycle      then return st.cpu.cycle      end
  if st.masterClock               then return st.masterClock    end
  return 0
end

-- NOTE: this build (MesenCE 2.1.1) exposes the type enum as emu.callbackType
-- (exec=2), NOT the docs' emu.memCallbackType (which is nil here).
emu.addMemoryCallback(function(address, value)
  f:write(string.format("%d,%d\n", hits, cyc(emu.getState())))
  hits = hits + 1
  if hits >= MAXHITS then f:close(); emu.log("cycwatch: "..hits.." hits"); emu.stop(0) end
end, emu.callbackType.exec, ANCHOR)

-- safety: stop even if the anchor never reaches MAXHITS
local frame = 0
emu.addEventCallback(function()
  frame = frame + 1
  if frame > 4000 then if f then f:close() end; emu.stop(0) end
end, emu.eventType.endFrame)
