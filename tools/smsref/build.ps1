# Build smsref — headless Genesis Plus GX SMS/GG reference oracle.
# GPGX source is an EXTERNAL clone (not in the engine repo; oracle-only, never
# shipped). See README.md. Run via the PowerShell tool.
$env:Path = "C:\msys64\mingw64\bin;$env:Path"
$H = $PSScriptRoot
$G = "F:\Projects\smsggrecomp\smsref-ext\genesis-plus-gx\core"   # external GPGX clone
if (-not (Test-Path "$G\system.c")) { "ERROR: GPGX not found at $G (see README.md)"; exit 1 }

$inc = @("-I","$H","-I","$G","-I","$G\z80","-I","$G\m68k","-I","$G\sound",
         "-I","$G\input_hw","-I","$G\cart_hw","-I","$G\cart_hw\svp",
         "-I","$G\cd_hw","-I","$G\ntsc","-I","$G\sound\minimp3")
$def = @("-DLSB_FIRST","-DUSE_16BPP_RENDERING","-DMAXROMSIZE=33554432")

$core = @(
  "$G\z80\z80.c","$G\m68k\m68kcpu.c","$G\m68k\s68kcpu.c",
  "$G\genesis.c","$G\vdp_ctrl.c","$G\vdp_render.c","$G\system.c","$G\io_ctrl.c",
  "$G\mem68k.c","$G\memz80.c","$G\membnk.c","$G\state.c","$G\loadrom.c",
  "$G\input_hw\input.c","$G\input_hw\gamepad.c","$G\input_hw\lightgun.c",
  "$G\input_hw\mouse.c","$G\input_hw\activator.c","$G\input_hw\xe_1ap.c",
  "$G\input_hw\teamplayer.c","$G\input_hw\paddle.c","$G\input_hw\smash.c",
  "$G\input_hw\sportspad.c","$G\input_hw\terebi_oekaki.c","$G\input_hw\graphic_board.c",
  "$G\sound\sound.c","$G\sound\psg.c","$G\sound\ym2612.c","$G\sound\ym2413.c",
  "$G\sound\blip_buf.c","$G\sound\eq.c",
  "$G\cart_hw\sram.c","$G\cart_hw\ggenie.c","$G\cart_hw\areplay.c",
  "$G\cart_hw\eeprom_93c.c","$G\cart_hw\eeprom_i2c.c","$G\cart_hw\eeprom_spi.c",
  "$G\cart_hw\flash_cfi.c","$G\cart_hw\svp\svp.c","$G\cart_hw\svp\ssp16.c",
  "$G\cart_hw\megasd.c","$G\cart_hw\yx5200.c","$G\cart_hw\md_cart.c","$G\cart_hw\sms_cart.c",
  "$G\cd_hw\scd.c","$G\cd_hw\cdd.c","$G\cd_hw\cdc.c","$G\cd_hw\gfx.c","$G\cd_hw\pcm.c","$G\cd_hw\cd_cart.c",
  "$G\ntsc\sms_ntsc.c","$G\ntsc\md_ntsc.c"
)

Remove-Item "$H\smsref.exe" -EA SilentlyContinue
gcc -O1 -w -std=gnu11 @def @inc "$H\smsref.c" @core -lz -lm -o "$H\smsref.exe" 2>&1 |
  Select-String -Pattern "error:|undefined reference|cannot find" | Select-Object -First 40
if (Test-Path "$H\smsref.exe") { "BUILD OK" } else { "BUILD FAILED" }
