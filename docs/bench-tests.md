# Bench tests

Tests that can be done with the parts on the desk - the controller with its screen and fan,
one stand-in motor, three limit switches, the power supply, a USB cable and jumper wires -
before the machine frame is available. Each test proves one link of the chain the lathe
depends on and settles a value or a behaviour the control software needs.

Read [hardware.md](hardware.md) first, in particular the handling rules.

The controller needs its 12 V supply for every test: it does not run from USB. Tests T2 and
T3 are done with **no motor connected**, so nothing can move.

## Overview

| Test | Proves | Feeds | Date | Result | Record |
| --- | --- | --- | --- | --- | --- |
| T0 Label check | what is actually on the bench | starting values for the configuration | 2026-11-13 | done, three items open | below |
| T1 Motor coil pairs | how the test motor must be wired | wiring notes in hardware.md | | wire order known, pairs not yet checked | |
| T2 First power-up and firmware baseline | the serial link works; firmware identity and settings as delivered | serial link, status parsing, the baseline to return to | 2026-11-13 | **pass** | `records/20261113-161720_probe.txt` |
| T3 Limit inputs | which header is which input, and its polarity | homing and stop configuration | | | |
| T4 First motor movement | each driver socket turns the motor both ways | jog commands | | | |
| T5 Steps per revolution | the real microstep setting | steps-per-mm and steps-per-rev values | | | |
| T6 Speed envelope | highest reliable speed, temperature rise | speed and acceleration limits of the rotation axis | | | |
| T7 Homing sequence | search, trigger, pull-off, slow re-approach, failure alarm | homing and alarm handling | | | |
| T8 Stop and recovery | stop halts motion, locks commands, needs a deliberate restart | the stop state machine (tasks X1-X3) | | | |
| T9 One pass, dry | plunge, feed and retract follow each other with no pause in rotation | the pass generator | | | |

Detailed steps for T4 onward are written as each earlier test settles what they depend on.

---

## T0 Label check

**Needs:** nothing powered.

| Item | Found |
| --- | --- |
| Board revision | MKS DLC32 **V2.2** (the published drawings stop at V2.1) |
| Power supply | 12 V, 2.5 A |
| Driver module | TMC2209-LA, 0.11 ohm sense resistors (X module taken out and photographed) |
| DIP switches under the X driver | M0, M1, M2 all on: 16 microsteps |
| DIP switches under the Y and Z drivers | not looked at; T5 measures the result instead |
| Motor plug wire order | blue, red, green, black |
| On-board power switch | none: the `SWICTH` pads are empty, the red part is the 10 A fuse |
| Fan voltage | open: label not read (it runs on the 12 V supply) |
| Driver current (Vref) | open: needs a multimeter |

## T1 Motor coil pairs

**Needs:** the motor and a paper clip. Nothing powered.

Follow "Test motor" in hardware.md.

**Pass:** bridging blue and red makes the shaft clearly harder to turn; bridging red and
green does not.

## T2 First power-up and firmware baseline

**Needs:** the controller, its 12 V supply and the USB cable. No motor, no limit switches.

1. Plug in the 12 V supply. The `VIN`, `5V` and `3V3` LEDs light and the fan runs.
2. Connect the controller to the computer by USB.
3. `python -m lathe ports` - one line marked `*` should appear
   (macOS: `/dev/cu.usbserial-...`, Windows: `COMn`).
4. `python -m lathe probe` - asks the controller for its version, all settings and its status,
   and saves a record under `records/`.
5. Read the record, then commit it.

The probe only asks questions. It does not change a setting or move anything.

**Pass:** the tool prints "The controller answered." with the firmware identity, and the record
contains the `$$` settings list and a status report.

### Result, 2026-11-13: pass

| Observation | Value |
| --- | --- |
| With USB only | the computer saw no USB device at all; the port appeared once 12 V was applied |
| Port | `/dev/cu.usbserial-130`, CH340, USB id `1a86:7523` |
| Boot text | none: the controller did not restart when the port was opened |
| Identity | `[PRODUCER:MKS DLC32]` `[V2.2.6(8M.H35.20241121)]` `[VER:1.1h.2024112101:]` `[OPT:VMPH,63,512]` |
| Status | `<Idle\|MPos:0.000,0.000,0.000\|FS:0,0\|Pn:P\|WCO:0.000,0.000,0.000>` |
| Screen | stayed dark although the firmware was running: open item |

What the settings as delivered mean for the lathe:

- **Three axes** (X, Y, Z) are reported, at 100 steps/mm, with homing commands for each
  (`$HX`, `$HY`, `$HZ`), jogging (`$J`) and a motor-release command (`$MD`).
- **Set up as a laser engraver**: laser mode on (`$32=1`), spindle type `LASER`. Both have to
  be turned off for the lathe.
- **Motors are released 25 ms after each move** (`$1=25`). The lathe must hold its depth of
  cut between moves, so this becomes `$1=255` (always energised).
- **Homing is enabled** (`$22=1`) for X and Y together (`$Homing/Cycle0=XY`), but the
  controller still starts in `Idle`, not locked. Hard and soft limits are off.
- **Limit inputs are inverted** (`$5=1`) and none shows as active with nothing plugged in.
  The probe input shows as active (`Pn:P`) with nothing plugged in.
- **No stop, hold or door input** appears anywhere in the settings or the command list. On
  this firmware a hardware stop can only be built from a limit input.

The boot section of the record is empty because the controller did not restart when the port
was opened; the probe now writes a line saying so.

## T3 Limit inputs

**Needs:** T2 passed. The 12 V supply, USB, the three limit switches and their cables. No motor.

Before the first switch is connected, the pin order of the limit headers on this V2.2 board
and of the switch module's cable has to be established, because a reversed plug shorts the
5 V supply (hardware.md, handling rule 4).

1. Supply unplugged. Connect **one** switch to the `X` header, matching `5V / GND / S`.
2. Plug the supply in. Check that the `5V` LED is still lit. If it is not, unplug at once.
3. Run `python -m lathe monitor`.
4. Note what is listed as active with nothing pressed.
5. Press and hold the switch for a second and release it. Watch which letter appears (`+`)
   and disappears (`-`), and whether the LED on the switch lights.
6. Stop with Ctrl-C. Unplug the supply, add the next switch on `Y`, and repeat; then `Z`.

| Header | Letter that changed | Active at rest? | Active when pressed? | LED when pressed? |
| --- | --- | --- | --- | --- |
| `X` | | | | |
| `Y` | | | | |
| `Z` | | | | |

**Pass:** every header changes its own letter and no other, each press and release is seen
once, and the `5V` LED stays lit with all three switches connected.

**How to read it:** a letter that is listed at rest and disappears on pressing means the firmware's
idea of "active" is the opposite of this switch. That is a setting, not a fault; write it down
and it is corrected in the configuration. A letter that never changes means that cable or
header is not making contact.

---

## T4 to T9

These need the motor. Outline and pass criteria; step-by-step instructions follow as the
earlier tests are completed.

**T4 First motor movement.** Motor on `X-Motor`. Jog a small distance in each direction
at low speed. Repeat on `Y1-Motor` and `Z-Motor`, unplugging the supply before each move of the plug.
*Pass:* smooth rotation both ways on all three sockets, no buzzing or stalling.

**T5 Steps per revolution.** Mark the shaft. Command the number of steps that should be exactly
ten revolutions and check the mark returns to where it started.
*Pass:* the mark lines up; the microstep setting is then known. At 16 microsteps that is
3200 steps per revolution, which gives 400 steps per millimetre on the 8 mm lead screws.

**T6 Speed envelope.** Step the rotation speed up until the motor stalls or loses position, at
two acceleration settings. Run ten minutes at 70 % of that speed and check temperatures by hand.
*Pass:* a speed is found that holds position over a long run; that value, with margin, becomes
the limit for the rotation axis. It depends on the supply voltage, so the voltage is recorded with it.

**T7 Homing sequence.** With homing enabled on one axis, start a homing cycle and press the
switch by hand to play the part of the carriage.
*Pass:* the motor seeks, stops on the press, backs off, re-approaches slowly, stops again and
backs off; if the switch is never pressed, the controller raises a homing alarm instead of
running on.

**T8 Stop and recovery.** The third switch plays the stop button. Press it during a long move.
*Pass:* motion stops at once; further motion commands are refused; while the button is held the
machine cannot be unlocked; after release it still needs a deliberate reset and re-homing before
it will move. The same run is repeated with a software feed hold (which may resume) and with
the supply unplugged (which must be followed by re-homing), and the three are compared.

**T9 One pass, dry.** Run the plunge - feed - retract sequence of one cutting pass with the
motor on each socket in turn: on the rotation socket it must turn continuously through all
three moves; on the feed and depth sockets it must make the expected moves at the expected time.
*Pass:* no stop or stutter in rotation between moves; distances and timing match the plan.
