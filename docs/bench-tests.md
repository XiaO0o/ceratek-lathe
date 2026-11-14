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
| T1 Motor coil pairs | how the test motor must be wired | wiring notes in hardware.md | 2026-11-13 | settled by T4: wired straight through, the motor runs | |
| T2 First power-up and firmware baseline | the serial link works; firmware identity and settings as delivered | serial link, status parsing, the baseline to return to | 2026-11-13 | **pass** | `records/20261113-161720_probe.txt` |
| T3 Limit inputs | which header is which input, and its polarity | homing and stop configuration | 2026-11-13 | **pass** | `records/20261113_t3_monitor_xyz.txt` |
| T4 First motor movement | each driver socket turns the motor both ways | jog commands | 2026-11-13 | **pass** | below |
| T5 Steps per revolution | the real microstep setting | steps-per-mm and steps-per-rev values | | | |
| T6 Speed envelope | highest reliable speed, temperature rise | speed and acceleration limits of the rotation axis | 2026-11-14 | smooth to 200 rev/min, lost steps from 300 (12 V, no load); long run not done | `records/20261114_t6_speed_steps_60-600.txt` |
| T7 Homing sequence | search, trigger, pull-off, slow re-approach, failure alarm | homing and alarm handling | 2026-11-14 | **pass** for Y by hand, no motor; failure alarm and Z not done | `records/20261114_t7_home_y.txt` |
| T8 Stop and recovery | stop halts motion, locks commands, needs a deliberate restart | the stop state machine (tasks X1-X3) | 2026-11-14 | **pass** for the stop input; feed hold and power cut not compared yet | `records/20261114_t8_estop_motor_seated.txt` |
| T9 One pass, dry | plunge, feed and retract follow each other with no pause in rotation | the pass generator | 2026-11-14 | **pass** on the controller side with the rotation motor; Y and Z had no motors | `records/20261114_t9_dry_pass.txt` |

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
| X driver module | put back before power-on; sits the same way round as Y and Z |
| Limit switch cable | red = supply, black = ground, green = signal; plain 3-pin plug with no key at the controller end |
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
| Screen | dark at first, with its buzzer ticking twice a second, although the firmware was running: the two ribbons were swapped. It lit once they were corrected |

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

### Pre-check: which pin of the limit header is 5 V

The pin order of the V2.2 limit headers is not published, and the cable's plug fits either
way round; reversed, it shorts the 5 V supply when the switch is pressed (hardware.md,
section 6). So the 5 V pin is found first, with two wires only. Every path in this check
runs through the module's 1 k resistor, so a wrong guess cannot short anything.

1. Supply unplugged. Push two male-female jumper wires into the red and the black position
   of the cable's 3-pin plug. Leave the green position empty.
2. Put the black jumper on the **middle** pin of the `X` header, and the red jumper on the
   outer pin on the motor-header side (where the V2.1 drawing has 5 V).
3. Plug the supply in and press the switch lever. The LED on the module lights clearly while
   the lever is pressed if, and only if, the red jumper sits on the 5 V pin.
4. No light: unplug the supply, move the red jumper to the other outer pin, and try again.

| Outer pin of the `X` header | LED while pressed |
| --- | --- |
| on the motor-header side | lit clearly: this is the 5 V pin, as in the V2.1 drawing |
| on the ESP32 side | not tried; it is the signal pin |

### The test

1. Supply unplugged. Remove the jumpers and plug the cable's 3-pin plug onto the `X` header
   with the red wire on the 5 V pin just found. Green then sits on `S`.
2. Plug the supply in. Check that the `5V` LED is still lit. If it is not, unplug at once.
3. Run `python -m lathe monitor`.
4. Note what is listed as active with nothing pressed. With the firmware as delivered that
   should be `P` only (seen on 2026-11-13 with nothing connected).
5. Press and hold the switch for a second and release it. Watch which letter appears (`+`)
   and disappears (`-`), and whether the LED on the switch lights.
6. Stop with Ctrl-C. Unplug the supply, add the next switch on `Y`, and repeat; then `Z`.

| Header | Letter that changed | Active at rest? | Active when pressed? | LED when pressed? |
| --- | --- | --- | --- | --- |
| `X` | X | no | yes | yes |
| `Y` | Y | no | yes | not noted |
| `Z` | Z | no | yes | not noted |

**Pass:** every header changes its own letter and no other, each press and release is seen
once, and the `5V` LED stays lit with all three switches connected.

### Result, 2026-11-13: pass

X alone was pressed ten times, then X, Y and Z three times each with all three connected.
Every press and release was seen exactly once, each header changed only its own letter, and
the `5V` LED stayed lit. The limit headers are `5V / GND / S` from the motor-header side, and
the polarity as delivered (`$5=1`) suits these switches: nothing has to be changed.

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

### T4 result, 2026-11-13: pass

The motor plug was wired straight through (blue, red, green, black) with four jumper wires,
to `X-Motor`, then `Y1-Motor`, then `Z-Motor`, with the supply unplugged for each change.
On each socket the command `$J=G91 <axis>32 F300` and its reverse were sent.

| Socket | Commanded | Seen at the shaft |
| --- | --- | --- |
| `X-Motor` | +32 then -32 units, 6.5 s each | one full turn anticlockwise, then one full turn back to the start; smooth and quiet |
| `Y1-Motor` | +32 then -32 units | one full turn and back |
| `Z-Motor` | +32 then -32 units | one full turn and back |

- All three drivers work, and the motor header pin order on V2.2 is the V2.1 one.
- 32 units at the delivered 100 steps per unit is 3200 steps, and that was one turn by eye:
  16 microsteps on every socket, as the X DIP switches say. T5 still checks it over ten turns.
- Motor and driver heatsinks stayed at a normal temperature after the X runs.
- A first, quarter-turn jog on X was reported as "no movement" and then recognised as having
  been missed by eye; the later runs were all seen.

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

---

## Firmware change, 2026-11-14

The firmware as delivered has no stop input and only one soft-limit switch for all axes
(T2), so the board was moved to FluidNC v4.1.1.

1. The complete 8 MB flash was read out twice at 115200 baud (this USB link loses data at
   higher rates). The two reads are identical except for 314 bytes inside the `nvs` settings
   partition, which the firmware rewrites while running. The first read is kept as
   `backups/mks-dlc32-v2.2_factory_V2.2.6_20261113.bin`. It is not in the repository because
   it holds the Wi-Fi settings stored on the board. Writing it back to address 0 restores
   the board as delivered.
2. FluidNC's own pre-flash check (secure boot not enabled) passed.
3. The official `fluidnc-v4.1.1-posix.zip` release was written with esptool at the addresses
   its install script uses: bootloader 0x1000, boot_app0 0xe000, firmware 0x10000,
   partitions 0x8000, file system 0x3d0000. Every part verified after writing.
4. `firmware/fluidnc/config.yaml` was uploaded over XMODEM (`$Xmodem/Receive=config.yaml`)
   and the board restarted (`$Bye`).

**Result:** the startup log (`$SS`) lists the machine name, the I2S pins, the three axes with
their step and direction outputs, the Y and Z limit inputs and the stop input, with no error
or warning. The controller starts in `Alarm`, as configured (`must_home`). Record:
`records/20261114-131230_probe.txt`.

**Still to do under FluidNC:** repeat T3 (Y and Z limit inputs, and the stop input on the `X`
header) and T4 (motor on each socket). Until then the pin numbers that were taken from the
V2.1 drawings are confirmed only as far as "FluidNC accepted them".

FluidNC also starts a Wi-Fi access point named `FluidNC` with its default password. It is not
needed for the bench work; whether to turn it off is open.

**Overheating, 2026-11-14.** With the first FluidNC configuration the motors stayed energised
all the time (`idle_ms: 255`). The stand-in motor, connected to `Z-Motor` and standing still,
became too hot to touch within roughly a quarter of an hour and was unplugged. Under the
firmware as delivered the motors were released 25 ms after each move, which had hidden how
high the driver current is. The configuration now releases the motors when idle
(`idle_ms: 250`) until the driver current has been measured and set. The copy on the board
still has the old value until the file is uploaded again.

**Inputs under FluidNC, 2026-11-14: pass.** With the corrected configuration on the board
(startup log: `Idle Delay:250ms`), Y, Z and the stop switch on the `X` header were pressed
three times each. FluidNC reported `Y`, `Z` and `E` respectively, each press and release
once, nothing active at rest. Record: `records/20261114_t3_monitor_fluidnc.txt`.
Unplugging the 12 V supply was also seen to remove the serial port from the computer.

**Not yet shown:** that the stop input locks the machine. The presses above were made while
the controller was already in the start-up alarm; afterwards `$X` unlocked it and a jog
command was accepted. The stop test proper (T8) has to press the switch while the machine
is unlocked and moving, and try to unlock while it is held.

**Stop input locks the machine, 2026-11-14: pass (no motor connected).** A 50 mm Y jog was
started and the stop switch on the `X` header was pressed 18 s in and held for 19 s.
Record: `records/20261114_t8_estop_no_motor.txt`.

| Step | What the controller did |
| --- | --- |
| Switch pressed during the move | stopped within one 0.1 s poll: `Alarm`, `ALARM:13`, "Stopped by estop_pin" |
| `$X`, a jog and `$H` while the switch was held | none accepted; `$X` answered "Reset to continue" |
| `$X` and a jog after release, without a reset | none accepted; still `Alarm` |
| Soft reset (Ctrl-X) after release | back to `Idle`; commands accepted again |

This is the behaviour the design report asks for: motion stops, commands stay inhibited while
the switch is active, and release alone does not restart the machine.

Two things the control software has to handle:

- In this alarm FluidNC sends neither `ok` nor `error` for a refused command, so the software
  must not wait for a reply; it has to watch the status report instead.
- After the reset the controller is `Idle` and keeps its old position, although a hard stop
  can lose steps. FluidNC does not insist on homing again here, so the software must:
  after a stop, home before any further move.

Still open for T8: the same test with a motor turning, the comparison with a feed hold, and
the stopping time.

**Motor under FluidNC, 2026-11-14: X pass; Y and Z not repeated.** With the motor back on
`X-Motor`, `$J=G91 X1 F10` and its reverse turned it one full turn and back (seen), and after
stopping the shaft could be turned by hand, so the idle release works. The Y and Z outputs
were not re-tested with a motor under FluidNC, by the team's decision; their pin numbers are
the V2.1 ones that FluidNC accepted, and both sockets worked under the firmware as delivered.
They are checked when the machine's own motors are connected.

**Stop input with the motor turning, 2026-11-14.** Motor on `X-Motor` at 60 rev/min
(`$J=G91 X30 F60`); the stop switch was pressed after 21.7 turns and held about 6 s.
The controller behaved exactly as without a motor: `Alarm` and `ALARM:13` at once, no unlock,
jog or homing accepted while held or after release, `Idle` again only after a soft reset.
Record: `records/20261114_t8_estop_motor_LOOSE_WIRE_void.txt` (void, see below). Seen at the motor: it stopped when the switch was pressed.

**But the motor was not turning properly at 60 rev/min.** It shook instead of rotating, so
this run does not show a stop from a clean rotation, and the 21.7 turns are what the
controller counted, not what the shaft did. At 10 rev/min the same motor turned correctly
earlier the same day. The cause is not known yet (resonance of the unloaded motor, too little
torque at this current and 12 V, or the step pulses); the speed test T6 has to find where it
starts. The stop test with a motor is repeated at a speed where the motor turns properly.

**Cause of the shaking, and first speed steps, 2026-11-14.** The shaking at 60 rev/min was a
motor jumper wire that was not seated: with one wire loose only one coil is driven. After the
wires were pushed home, 10, 20, 30, 45 and 60 rev/min for five seconds each all turned
normally (seen). Record: `records/20261114_t6_speed_steps_10-60.txt`. The run made earlier
with the loose wire is not kept.

- Loose jumper wires on the motor header look exactly like a speed limit. On the machine the
  motor leads need latching plugs or crimped terminals, not jumper wires.
- 60 rev/min is not the limit; T6 continues upward from here.
- The stop test with the motor turning still has to be repeated with the wires seated.

**Stop input with the motor turning, repeated with the wires seated, 2026-11-14: pass.**
Motor on `X-Motor` at 60 rev/min, turning smoothly (seen). The stop switch was pressed after
about 4 turns and held about 7 s. The motor stopped at once (seen). The controller went to
`Alarm` with `ALARM:13`, accepted no unlock, jog or homing while the switch was held or
after its release, and returned to `Idle` only after a soft reset.
Record: `records/20261114_t8_estop_motor_seated.txt`. The earlier run with the loose wire is
kept only as a void record.

**Y homing by hand, 2026-11-14: pass (no motor on Y).** `$HY` was sent and the Y switch
pressed by hand in place of the carriage. Record: `records/20261114_t7_home_y.txt`.

| Time | What the controller did |
| --- | --- |
| 0 s | `Home`, moving toward the switch at 300 mm/min |
| 1.0 s | first press seen: stopped, then backed off 2 mm |
| 4.4 s | second press seen during the slow approach: stopped, backed off 2 mm |
| 6.9 s | `[MSG:Homed:Y]`, `Idle`, Y position set to 0 |
| 8.5 s | a third press, after homing: `ALARM:1` hard limit, "Reset to continue" |

- The sequence is search, stop on the switch, back off, slow approach, stop, back off, as
  planned. The slow approach only lasts about 2 s, so on the bench the second press has to
  follow within that time.
- The third press was not planned, and shows that the limit switch also works as a hard
  limit outside homing: touching it stops the machine and needs a reset.
- Not done: letting the search run out to see the failure alarm, homing Z, and homing with
  a motor actually moving a carriage.

**Homing failures, 2026-11-14.** Run through the machine driver (`lathe/machine.py`, on the
`feature/pass-planner` branch). Record: `records/20261114_t7_homing_failures.txt`.

- Y switch never pressed: the search stopped after 110 mm (the 100 mm travel times 1.1),
  `ALARM:9`. The driver reported "switch not found within the search distance" and did not
  go on to cut.
- Y switch still held when the pull-off ended: `ALARM:8`, reported as "switch still triggered
  after pull-off".

Homing by hand depends on how long the switch is held, and several attempts failed for that
reason. A full Y-then-Z homing through the driver has not succeeded on the bench; it is left
for the machine, where the carriage presses and releases the switch itself.

**A limit input stuck active, 2026-11-14.** For a while the Y input read active with the
switch untouched and its LED off, which made every homing attempt fail. Re-seating the plug
did not cure it; after the Y and Z cables were swapped on the board both inputs read
correctly. Neither a switch nor the board input was faulty: the plain 3-pin plug had not
been seated on the header. Together with the loose motor wire this is the third contact
fault of the day. The machine needs latching plugs or crimped terminals on the limit
switches as well as on the motors.

**One pass, dry (T9), 2026-11-14: pass on the controller side.** The pass planner's first
pass (10.6 mm to 10.0 mm, 0.05 mm deep, length shortened to 10 mm) was sent by the machine
driver with the motor on `X-Motor`. Homing was bypassed in the test script, for the bench
only, after a restart had set all positions to zero. Record: `records/20261114_t9_dry_pass.txt`.

| Move | Sent | Result |
| --- | --- | --- |
| spin up | 2 turns | |
| plunge | 5 turns while Y moves +1.05 mm | |
| feed | 100 turns while Z moves +10 mm | |
| retract | 5 turns while Y moves -1.05 mm | |
| return | Z -10 mm, no rotation | |
| whole pass | 112 turns planned in 112 s | the controller was in `Run` without a break for 113 s and ended `Idle` at X 112.000, Y 0.000, Z 0.000 |

- The controller accepted inverse-time moves (G93) that combine turns of the tube with
  millimetres of travel, and ended exactly where the plan says.
- Seen at the motor: it turned smoothly for the whole 113 s, with no pause between the
  spin-up, plunge, feed and retract moves.
- Not shown: that Y and Z really move, since they had no motors.

**Rotation speed limit (T6), 2026-11-14.** Motor on `X-Motor`, 12 V supply, nothing on the
shaft. Each step ran for about five seconds. Record: `records/20261114_t6_speed_steps_60-600.txt`.

| rev/min | 60 | 75 | 90 | 105 | 120 | 150 | 200 | 300 | 400 | 500 | 600 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Seen at the motor | normal | normal | normal | normal | normal | normal | normal | not normal | not normal | not normal | not normal |

- From 150 rev/min up, a temporary configuration (not committed) raised the X limit to 600
  rev/min and the acceleration to 10 rev/s^2; the committed configuration was uploaded again
  afterwards.
- The controller reached every commanded speed and raised no alarm, also where the motor
  lost steps: a stepper that stalls is not noticed by the controller. The limit has to come
  from watching the motor.
- The X limit in `firmware/fluidnc/config.yaml` is now 150 rev/min, 75 % of the last speed
  that ran smoothly. Cutting is planned at 60 rev/min (`config/lathe.toml`).
- Not done: the ten-minute run with a temperature check, a finer step between 200 and 300,
  and the effect of the acceleration. All of it changes with the real motor, the tube holder
  on the shaft, and a 24 V supply, so T6 is repeated on the machine.

