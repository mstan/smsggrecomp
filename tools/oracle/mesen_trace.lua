-- mesen_trace.lua -- Mesen 2 headless oracle trace for the SMS/GG recomp.
-- Run:  Mesen.exe --testRunner mesen_trace.lua <rom.sms> --noaudio --enablestdout
--
-- Emits a per-frame CSV (frame,cycleCount) to the script-data folder, the
-- GREEN-leg cycle reference for Axis 2/3 (compare vs the recomp's guest cycles
-- per anchor). Audio is NOT scriptable in Mesen 2 (no Lua sample API); capture
-- the reference WAV via the deterministic movie + Sound Recorder recipe in
-- accuracy/audio.md, then diff with tools/audio/audio_diff.py.
--
-- NOTE: the exact emu.getState() table layout for the SMS/GG core can change
-- between Mesen builds. On first run, inspect the printed key dump and adjust
-- CPU_CYCLE_PATH below if needed.

local TARGET_FRAMES = tonumber(os.getenv("MESEN_TRACE_FRAMES")) or 1800
local frame = 0
local path = emu.getScriptDataFolder() .. "/mesen_cyc.csv"
local f = io.open(path, "w")
f:write("frame,cycleCount\n")

-- one-time state-shape dump so we can confirm the cycle field name
local dumped = false
local function dump_keys(t, prefix)
  for k, v in pairs(t) do
    local ty = type(v)
    emu.log(prefix .. tostring(k) .. " : " .. ty ..
            (ty == "number" and (" = " .. tostring(v)) or ""))
    if ty == "table" and prefix:len() < 8 then
      dump_keys(v, prefix .. tostring(k) .. ".")
    end
  end
end

local function cycle_count(st)
  -- try common locations across Mesen builds
  if st.cpu and st.cpu.cycleCount then return st.cpu.cycleCount end
  if st.cpu and st.cpu.cycle then return st.cpu.cycle end
  if st.masterClock then return st.masterClock end
  return 0
end

emu.addEventCallback(function()
  frame = frame + 1
  local st = emu.getState()
  if not dumped then
    emu.log("=== emu.getState() shape ===")
    dump_keys(st, "")
    dumped = true
  end
  f:write(string.format("%d,%d\n", frame, cycle_count(st)))
  if frame >= TARGET_FRAMES then
    f:close()
    emu.log("mesen_trace: wrote " .. TARGET_FRAMES .. " frames to " .. path)
    emu.stop(0)
  end
end, emu.eventType.endFrame)
