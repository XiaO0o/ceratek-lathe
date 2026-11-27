# Ceratek lathe touchscreen firmware

**2026-11-24: the guided job follows the group's plan** (lathe/core/Plan.h): home to the start corner
(switches B and C), touch off, then passes that each go one fixed step further in, travel along the
tube until switch A (wired as the probe input, G38.2), and return to the start corner; after each
pass the operator answers whether the tube is right. The old diameter-based planner (passes.py port,
vectors.txt) is gone. The turning tube axis is added later.

**2026-11-26:**
- Speeds: X (the tube) up to 200 rev/min, and the cut uses 200 by default. Y and Z go up to
  960 mm/min (120 motor rev/min on the 8 mm lead). The moves themselves keep their slower speeds.
- Each new tube starts by asking for the step per round toward the tube (0.01 to 10 mm). The
  step used last is shown first and kept in flash.
- A length that ends at switch A is cut 1 mm short (`switch_a_margin_mm`), so the cut does not
  touch A again with the tool at depth.
- Warnings: the buzzer (`beeper_pin: I2SO.7`) sounds only for a stop nobody planned, shown in red
  as `WARNING:` on the screen. That means a switch touched during the depth steps, the cut, the
  way back, a manual move or the free-turning tube, or a failed homing.
  - Planned switch touches are messages only: homing at B and C, Locate and the length
    measurement at A.
  - Stops the operator makes are messages only: Main, or Stop during homing.
  - The buttons no longer click.


FluidNC v4.1.1 plus one added module, `lathe_screen`, that drives the MKS TS35-R on the MKS
DLC32 and runs the operator's guided sequence on the controller, so the lathe can be used
without a computer. FluidNC's motion, homing, limit and stop code is unchanged except for the two
small changes in `Homing.cpp` and `Protocol.cpp` listed below (both under `#ifdef CERATEK_LATHE_SCREEN`).

Step-by-step instructions (Chinese, Windows 10): [GUIDE-zh-Windows.md](GUIDE-zh-Windows.md). Design: the team's design doc.

## Layout

```
lathe.bat + lathe.ps1          Windows: lathe setup / build / flash / config / monitor / restore / test
lathe.sh                      the same commands on macOS / Linux (./lathe.sh ...)
config/config_screen.yaml     the team's config.yaml + the lathe_screen: block (allow_motion: true since the S1-S3 screen tests)
config/config_stock.yaml      the team's config.yaml as of commit 2adacb1, for going back
fluidnc/                      copied over a FluidNC v4.1.1 checkout by lathe.sh
  platformio_override.ini     envs ceratek (no Wi-Fi), ceratek_wifi (8 MB layout), stock_wifi
  FluidNC/lathe/core/         port of lathe/grbl.py, link.py, passes.py, machine.py, session.py
                              (standard C++ only; also built by the PC tests)
  FluidNC/lathe/LatheScreen.* the FluidNC module: config block, command channel, status channel
  FluidNC/lathe/Display.*     LovyanGFX set-up for ST7796 + ADS7843E, calibration in NVS
  FluidNC/lathe/TouchUi.*     screens (home, manual, guided job), keypad, jog pad, and the job task
  FluidNC/src/Machine/Homing.cpp  FluidNC's file with two changes, both under #ifdef CERATEK_LATHE_SCREEN:
                              rearm all limit pins when homing starts/ends/fails (stuck Pn:YZ),
                              and wait up to 30 s for a switch still pressed after pull-off
                              instead of alarm 8 at once
  FluidNC/src/Protocol.cpp    FluidNC's file, one change under #ifdef CERATEK_LATHE_SCREEN: a stop switch
                              pressed during homing leaves Homing before the motors are released, so
                              they no longer stay powered in the alarm state (heating, 2026-11-22)
host-tests/                   PC tests (g++ or clang++): ports of tests/test_*.py and tests of the group's plan
```

## How it fits into FluidNC

`LatheScreen` is a `Channel` and a `ConfigurableModule`, like FluidNC's own `OLED` module. To
FluidNC it is one more command channel, like USB: lines pushed into it are run by FluidNC's
normal code, which answers with the same `ok`, `error:n`, `ALARM:n` and status lines it sends
over USB. The C++ port of the Python software therefore talks to it through a byte port, the
way `lathe/link.py` talks to the serial port, and the rules bench-tested in T7 and T8 carry over
unchanged. The command channel has automatic reports off, as USB does; a second, listen-only
channel (`StatusTap`, built like `Status_outputs`) feeds the status bar.

Tasks: the guided sequence runs on its own task (`lathejob`) and blocks while it waits, like the
Python; the display and touch run on `lathescr`. Both are on core 0 at priority 1, next to
FluidNC's polling task; core 1 (FluidNC's motion loop, priority 2) is left alone.

## Status

- Core logic: 52 PC tests pass (2026-11-26, clang++ on macOS, no warnings).
- 2026-11-21: built for the ESP32 (PlatformIO espressif32 6.10.0, Arduino core 2.0.17) and run on
  the bench board: screen, touch, inputs, homing (success, alarm 8, alarm 9), the guided job and the
  stop switch (T8) all worked. Fixes made after that run: limit pins rearmed at the start and end of
  homing; a held switch pauses homing instead of alarm 8; bench `feed_scaler` 5.0 so the slow second
  touch can be made by hand.
- To be measured at the first flash: free heap, backlight polarity (drawing says high = lit),
  touch calibration, image size of the Wi-Fi build.

Licence: the module is built into FluidNC, which is GPLv3, so the firmware and these files are
GPLv3 as well. LovyanGFX is FreeBSD-licensed.
