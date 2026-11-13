# Bench tests

Tests that can be done with the parts on the desk - the controller with its screen and fan,
one stand-in motor, three limit switches, the power supply, a USB cable and jumper wires -
before the machine frame is available. Each test proves one link of the chain the lathe
depends on and settles a value or a behaviour the control software needs.

Read [hardware.md](hardware.md) first, in particular the handling rules.

## Overview

| Test | Proves | Feeds | Date | Result | Record |
| --- | --- | --- | --- | --- | --- |
| T0 Label check | what is actually on the bench | starting values for the configuration | | | |
| T1 Motor coil pairs | how the test motor must be wired | wiring table in hardware.md | | | |
| T2 USB-only power-up | the serial link works; firmware identity and settings as delivered | serial link, status parsing, the baseline to return to | | | |
| T3 Limit inputs | which header is which input, and its polarity | homing and stop configuration | | | |
| T4 First motor movement | each driver socket turns the motor both ways | jog commands | | | |
| T5 Steps per revolution | the real microstep setting | steps-per-mm and steps-per-rev values | | | |
| T6 Speed envelope | highest reliable speed, temperature rise | speed and acceleration limits of the rotation axis | | | |
| T7 Homing sequence | search, trigger, pull-off, slow re-approach, failure alarm | homing and alarm handling | | | |
| T8 Stop and recovery | stop halts motion, locks commands, needs a deliberate restart | the stop state machine (tasks X1-X3) | | | |
| T9 One pass, dry | plunge, feed and retract follow each other with no pause in rotation | the pass generator | | | |

T2 and T3 run on USB power alone, so the motor cannot move. They come first.
Detailed steps for T4 onward are written once T2 has shown which firmware build is on the board.

---

## T0 Label check

**Needs:** nothing powered.

Read and write down (or photograph):

- the board revision printed on the controller;
- the power supply label: output voltage and current;
- the fan label: voltage. It must match the power supply;
- the marking on the driver modules;
- the position of the three DIP switches under each driver;
- the order of the wire colours in the motor plug.

**Done when:** section 8 of hardware.md is filled in, apart from the items that need a later test.

## T1 Motor coil pairs

**Needs:** the motor and one jumper wire. Nothing powered.

Follow "Test motor" in hardware.md and fill in its table.

**Pass:** two pairs of plug holes are identified; bridging either pair makes the shaft clearly
harder to turn, and bridging one hole from each pair does not.

## T2 USB-only power-up

**Needs:** the controller and the USB cable. DC supply unplugged, motor unplugged.

1. Connect the controller to the computer by USB. The screen may light up; that is expected.
2. `python -m lathe ports` - one line marked `*` should appear
   (macOS: `/dev/cu.usbserial-...`, Windows: `COMn`).
3. `python -m lathe probe` - this opens the port (which restarts the controller), waits for it
   to boot, asks for its version, all settings and its status, and saves a record under `records/`.
4. Read the record, then commit it.

The probe only asks questions. It does not change a setting or move anything.

**Pass:** the tool prints "The controller answered." with a Grbl banner and a `[VER:...]` line,
and the record contains the `$$` settings list and a status report.

**Record:** firmware version, number of axes, and the settings as delivered. This record is the
baseline: whatever is changed later can be compared with it and put back.

| Observation | Value |
| --- | --- |
| Port name | |
| Banner line | |
| `[VER:...]` line | |
| Status report | |

## T3 Limit inputs

**Needs:** T2 passed. Still USB only. The three limit switches and their cables.

1. Unplug USB. Connect one switch each to the `X-`, `Y-` and `Z-` headers, matching `5V / GND / S`
   (see the handling rules: a reversed plug shorts the 5 V supply).
2. Plug USB back in and run `python -m lathe monitor`.
3. Note what is listed as active with nothing pressed.
4. Press and hold each switch for a second, one at a time, and release it. Watch which letter
   appears (`+`) and disappears (`-`), and whether the LED on the switch lights.
5. Stop with Ctrl-C and fill in the table.

| Header | Letter that changed | Active at rest? | Active when pressed? | LED when pressed? |
| --- | --- | --- | --- | --- |
| `X-` | | | | |
| `Y-` | | | | |
| `Z-` | | | | |

**Pass:** every header changes its own letter and no other, and each press and release is seen
once.

**How to read it:** a letter that is listed at rest and disappears on pressing means the firmware's
idea of "active" is the opposite of this switch. That is a setting, not a fault; write it down
and it is corrected in the configuration. A letter that never changes means that cable or
header is not making contact.

---

## T4 to T9

These need the DC supply and the motor. Outline and pass criteria; step-by-step instructions
follow after T2.

**T4 First motor movement.** Motor on `X-Motor`, DC on. Jog a small distance in each direction
at low speed. Repeat on `Y1-Motor` and `Z-Motor`, switching DC off before each move of the plug.
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
the on-board power switch (which must be followed by re-homing), and the three are compared.

**T9 One pass, dry.** Run the plunge - feed - retract sequence of one cutting pass with the
motor on each socket in turn: on the rotation socket it must turn continuously through all
three moves; on the feed and depth sockets it must make the expected moves at the expected time.
*Pass:* no stop or stutter in rotation between moves; distances and timing match the plan.
