# Controller hardware notes

What the wiring and the software have to respect.

Our controller is an **MKS DLC32 V2.2**. Makerbase has published drawings only up to V2.1
(<https://github.com/makerbase-mks/MKS-DLC32>; the request for V2.2 files is open there as
issue #600). Every fact below therefore says how we know it:

- **seen** - observed on our own board: a label, a photo, or a bench test record;
- **V2.1** - read from the V2.1 drawings (`MKS DLC32 V2.1_003 PIN.pdf` and `SCH.pdf`). Our board
  has the same connectors in the same places, but an electrical detail taken from those
  drawings is not confirmed for V2.2 until a bench test has checked it.

One V2.1 detail is already known not to hold: the V2.1 schematic shows USB feeding the 5 V
rail, but our board does not run from USB (section 4).

## 1. What is on the bench

| Part | Detail | Source |
| --- | --- | --- |
| Controller | MKS DLC32 V2.2, ESP32 module, CH340C USB-serial chip | seen (silkscreen) |
| Firmware as delivered | MKS `V2.2.6(8M.H35.20241121)`, Grbl 1.1h protocol, 3 axes | seen (T2 record) |
| Firmware now | FluidNC v4.1.1 (esp32-wifi) with `firmware/fluidnc/config.yaml`, flashed 2026-11-14. The screen is not supported by FluidNC and stays dark | seen (probe record of 2026-11-14) |
| Stepper drivers | 3 plug-in modules, TMC2209-LA, 0.11 ohm sense resistors, blue heatsinks | seen on the X module; Y and Z assumed identical |
| Screen | MKS TS35-R V2.0 (board marked 002) on the two EXP ribbon cables. Works once each ribbon is on the right socket (section 5) | seen |
| Fan | on the 2-pin `-12/24V+` header; runs on the 12 V supply | seen |
| Power supply | 12 V, 2.5 A adapter on the DC jack | seen (label) |
| Test motor | Prusa MMU2S "Pulley" NEMA17, 1.8 deg/step (stand-in for the machine's motors) | seen |
| Limit switches | 3 mechanical end-stop modules with LED, 3-wire cables | seen |
| USB | USB-A to USB-B cable; `/dev/cu.usbserial-*` on macOS, 115200 baud | seen (T2) |

## 2. Connectors

Board seen from above with the motor headers at the top edge and the USB / DC sockets at the bottom:

```
 X-Motor       Y1-Motor      Y2-Motor      Z-Motor      fan      TTL
[2B 2A 1A 1B] [2B 2A 1A 1B] [2B 2A 1A 1B] [2B 2A 1A 1B] [+ -] [IO32 GND V]

                                 I2C    Probe   Z      Y      X
                        top ->   3V3    5V      5V     5V     5V
                                 GND    GND     GND    GND    GND
                     bottom ->   IO0    IO22    IO34   IO35   IO36
                                 IO4
```

- The connector positions and names are **seen**. The limit header pin order is **seen**
  (bench test T3). The motor header pin order is **seen** to work as
  drawn for V2.1 (bench test T4).
- Motor headers: 4 pins, 2.54 mm pitch, order `2B 2A 1A 1B`. The two pins of one motor coil
  are next to each other (`2B 2A` is one coil, `1A 1B` the other).
- `Y1-Motor` and `Y2-Motor` are wired to the same driver. The lathe uses `Y1-Motor` only.
- Limit and probe headers: 3 pins, `5V / GND / S`. On our board they are labelled `X`, `Y`,
  `Z` and `Probe`.
- Along the left edge: `-12/24V+`, `-BEEPER+`, `-SPINDLE+` (screw terminal), and three status
  LEDs marked `VIN`, `5V`, `3V3`.
- Beside each driver socket is a 4-pin `E S D G` header (enable, step, direction, ground) meant
  for external drivers. Not used.

### Signals (V2.1)

| Function | Connector | ESP32 pin | Notes |
| --- | --- | --- | --- |
| Step / direction, X | driver socket X | I2SO.1 / I2SO.2 | through the shift register, see below |
| Step / direction, Y | driver socket Y | I2SO.5 / I2SO.6 | |
| Step / direction, Z | driver socket Z | I2SO.3 / I2SO.4 | |
| Driver enable | all three sockets | I2SO.0 | one signal shared by X, Y and Z |
| Beeper | `BEEPER` header, EXP1 | I2SO.7 | |
| Shift register clock / latch / data | 74HC595 | IO16 / IO17 / IO21 | a 74HC595 is fitted on our board (seen) |
| X limit | `X` | IO36 | input only |
| Y limit | `Y` | IO35 | input only |
| Z limit | `Z` | IO34 | input only |
| Probe | `Probe` | IO22 | input with clamp diodes |
| Laser / spindle PWM | `TTL` (5 V logic) and `SPINDLE` (switched to ground) | IO32 | output only |
| I2C | `I2C` | IO0 (SDA), IO4 (SCL) | IO0 also selects the boot mode: must not be held low at power-up |
| SD card | card slot | IO12, IO13, IO14, IO15, IO39 | |
| Screen | EXP1 / EXP2 | IO18, IO19, IO23, IO33, IO25, IO26, IO27, IO5 | |

Step and direction do not come straight from ESP32 pins. They are shifted out through a
74HC595 register (the "I2SO" outputs above). Firmware therefore needs an I2S stepping engine,
which the stock firmware and FluidNC both have. This is the main reason the project configures
an existing firmware instead of writing its own.

In the V2.1 drawings each limit input has a pull-up to 3.3 V and a small filter capacitor at
the connector, then a series resistor to the ESP32, and the supply pin on those headers is 5 V.

## 3. Stepper drivers

The X module was taken out and photographed: a TMC2209-LA with two `R110` (0.11 ohm) sense
resistors (**seen**).

The driver sockets carry only step, direction, enable and three microstep DIP switches
(**V2.1**; the switches are **seen**). The TMC2209's serial pin (PDN_UART) and its diagnostic
pin (DIAG) are not connected to the ESP32. The drivers therefore run in stand-alone mode:

- Motor current is set by the small potentiometer on each driver module (its "Vref"), not by software.
- Microstepping is set by the DIP switches, not by software.
- The controller cannot read anything back from a driver: no load value, no stall flag,
  no over-temperature flag, no current. The `Current`, `Microsteps` and `StallGuard` settings
  the firmware lists apply to serially configured drivers and do nothing here.
- Reading motor load ("StallGuard") or homing without switches would need jumper wires from
  each driver's serial pin to a spare ESP32 pin, a firmware configured for it, and tuning.
  It is not planned; the limit switches do the job.

Microstepping of a TMC2209 in stand-alone mode (from its datasheet):

| MS1 | MS2 | Microsteps |
| --- | --- | --- |
| off | off | 8 |
| on | off | 32 |
| off | on | 64 |
| on | on | 16 |

The DIP switch sits under each driver module, labelled M0, M1, M2, and goes to the socket's
MS1, MS2 and MS3 positions. On the X socket all three are **ON** (**seen**), which means
16 microsteps: 3200 steps per motor revolution, 400 steps per millimetre on the 8 mm lead
screws. The Y and Z switches are hidden under their modules; bench test T5 measures the
real steps per revolution on every socket, so the modules do not need to come out.

Motor current: with 0.11 ohm sense resistors, `I_rms = 0.71 x Vref` (Vref in volts, measured
with a multimeter between the potentiometer and ground with the board powered).
**TBD**: measure the three Vref values before any long run.

All three drivers share one enable signal: the motors are energised or released together.

**Putting a driver module back.** Power off first. The module has one green and one black
row of pins, and the socket has one green and one black row: green goes into green. A module
plugged in the other way round is destroyed at power-up.

## 4. Power

```
DC jack 12-24 V --> 10 A fuse --> motor supply --> drivers, fan header, spindle output
                                       |
                                       +--> 5 V regulator --> 5 V and 3.3 V logic, screen, USB-serial chip
```

- **The DC supply powers everything, including the USB-serial chip** (**seen**). With USB alone
  the computer does not see the board at all; with 12 V applied the serial port appears. The
  manufacturer's wiring manual says the same: connect the DC supply before programming or
  online control.
- **There is no power switch on the board** (**seen**). The `SWICTH` pads next to the fuse are
  empty; per the wiring manual an external switch can be fitted there, with the fuse removed.
  The red part beside the DC jack is the 10 A blade fuse.
- Power is therefore cut by unplugging the adapter, and that turns the controller itself off.
  When power returns the firmware starts from scratch and the machine position is unknown:
  the machine must be re-homed. **TBD**: confirm on the bench that the serial port disappears
  when the supply is removed, so the software can tell a power cut from a silent controller.
- Three LEDs show the supply rails: `VIN`, `5V`, `3V3`. All three must be lit. The wiring
  manual's rule: if the `5V` LED goes out, unplug the supply at once and look for a short
  circuit on a limit switch connector.
- The 2-pin `12/24V` headers (where the fan is plugged in) carry the DC input voltage
  directly. A 12 V fan on a 24 V supply will be destroyed.
- 12 V at 2.5 A is enough for bench work with one motor. Three motors running together will
  be close to its limit, and a stepper loses torque earlier at 12 V than at 24 V; bench test
  T6 measures what the rotation motor can do on this supply.

## 5. Screen and ribbon cables

The MKS TS35-R V2.0 connects with two identical 10-wire ribbons, and nothing stops them from
being plugged into each other's sockets. The screen's drawings are published
(`MKS TS35-R V2.0_002` in <https://github.com/makerbase-mks/MKS-TFT-Hardware>).

| | EXP1 | EXP2 |
| --- | --- | --- |
| Carries | buzzer, backlight on/off, touch and display select lines, 5 V | display data lines (MISO, SCK, MOSI), reset, 3.3 V |
| On the screen, held back-up with the buzzer at the top left | upper socket | lower socket |
| On the controller | the socket next to the ESP32 module | the socket at the board corner, above the SD card slot |

The ribbon from the screen's EXP1 must end in the controller's EXP1. The screen-side positions
are from the screen's drawing; the controller-side positions are **V2.1** and agree with the
arrangement that works on our board.

**Seen on 2026-11-13:** with the two ribbons swapped the firmware ran normally and answered
over USB, but the screen stayed completely dark and its buzzer ticked about twice a second.
Swapped, the backlight line and the buzzer line end on pins that nothing drives. With the
ribbons corrected the screen lit. On our screen the resistor position that joins EXP2's 3.3 V
pin to the screen's own 3.3 V circuit (R8) looks empty, so the swap should not have put 5 V
onto that circuit.

## 6. Limit switches

Three red "Mech Endstop" style modules: a lever microswitch, an LED with a 1 k resistor (R1),
a 10 k resistor (R2), a capacitor (C1) and a 4-pin white connector, each with a 3-wire cable.

Traced from photos of our module and from the MakerBot design it copies, and confirmed by
bench test T3:

| Wire | Module | Switch terminal |
| --- | --- | --- |
| red | supply (5 V) | NC |
| black | ground (the two middle pins of the connector) | NO |
| green | signal | COM |

- At rest the signal is connected to the supply and reads 5 V. Pressed, it is connected to
  ground and reads 0 V, and the LED lights.
- That is the polarity the firmware as delivered expects (`$5=1`): the input should show as
  active only while the switch is pressed.
- The controller end of the cable is a plain 3-pin jumper plug with no key, in the order
  green, black, red. It fits either way round. Reversed, pressing the switch connects the
  controller's 5 V pin straight to ground.
- So the 5 V pin of a limit header is identified first, with a two-wire check that cannot
  short anything (the pre-check in bench test T3).

## 7. Planned lathe wiring

A proposal, to be confirmed together with the firmware choice and the mechanical layout.

| Machine function | Controller axis | Motor header | Home switch |
| --- | --- | --- | --- |
| Tube rotation | X | `X-Motor` | none |
| Depth of cut (the team's Y axis) | Y | `Y1-Motor` | `Y` (IO35), at the retracted end |
| Feed along the tube (the team's Z axis) | Z | `Z-Motor` | `Z` (IO34), at the start end |
| Stop switch | - | - | `Probe` (IO22) or `X` (IO36), decided with the firmware |

This keeps the axis names used in the design reports, and leaves the X limit input free
because a rotating axis has no end to find.

## 8. Test motor

The Prusa MMU2S pulley motor is rated 1 A per phase and ends in a 4-way Molex 50-57-9404
plug (2.54 mm pitch). It does not latch into the board's motor header, so it is connected with
four male-female jumper wires: male end into the motor plug, female end onto the header pin.

The wires in the plug run **blue, red, green, black** (**seen**; which end counts as hole 1 is
not known and does not matter). On motors of this kind blue-red is one coil and green-black
the other, which puts each coil on two neighbouring holes - the same arrangement as the
board's `2B 2A 1A 1B`. So the plug is wired straight through, hole 1 to pin 1 and so on. Which
way round it goes only decides the direction of rotation, which is set in software.

To confirm the coil pairs before powering the motor (bench test T1):

1. Motor disconnected from everything. Turn the shaft by hand: it turns freely.
2. Bridge the blue and red holes with a bent paper clip and turn again. The shaft should be
   clearly harder to turn and feel notchy.
3. Bridge red and green instead: nothing should change.

A jumper wire that is not pushed fully home leaves one coil undriven: the motor shakes and
loses steps instead of turning, which is easy to mistake for a speed limit (seen at
60 rev/min on 2026-11-14). The machine's motors need latching plugs or crimped terminals.

A motor wired with the coils mixed up does not turn, but it is not damaged. What damages a
driver is connecting or disconnecting the motor while the supply is on.

The test motor stands in for the machine's motors to prove wiring, logic and the control
sequence. Speeds, steps per millimetre and directions are measured again on the machine,
which is why they live in configuration and not in code.

## 9. Handling rules

1. Unplug the DC supply before connecting or disconnecting anything: a motor, a driver
   module, a limit switch, a ribbon cable.
2. Never plug or unplug a motor or a driver while the supply is on. It can destroy the driver.
3. Driver modules go in green row to green row (section 3).
4. Limit switch cables: the red wire goes on the header's 5 V pin, green on `S`, black in the
   middle (section 6). The plug fits either way round, and reversed it shorts the 5 V supply
   whenever the switch is pressed. Connect one switch at a time and watch the `5V` LED when
   power comes back on.
5. Screen ribbons: EXP1 to EXP1 and EXP2 to EXP2 (section 5). A dark screen with a ticking
   buzzer means they are swapped: unplug the supply and correct them.
6. Until the driver current has been measured, keep motor runs short and check by hand that
   the motor and the driver heatsinks stay comfortable to touch.
7. Keep fingers, sleeves and cables away from the motor shaft and its gears when it is powered.

## 10. To be recorded

| Item | Value |
| --- | --- |
| Board revision on the silkscreen | MKS DLC32 V2.2 |
| Power supply label | 12 V, 2.5 A |
| Fan label: voltage | **TBD** (runs on 12 V) |
| Driver module | TMC2209-LA, 0.11 ohm sense resistors |
| DIP switches, X (M0 M1 M2) | on, on, on |
| DIP switches, Y and Z | not looked at; all three sockets gave one turn for 3200 steps by eye (T4), so 16 microsteps |
| Vref, X / Y / Z | **TBD** (needs a multimeter) |
| Firmware as delivered | `V2.2.6(8M.H35.20241121)`, see `records/20261113-161720_probe.txt` |
| Limit header pin order on V2.2 | `5V / GND / S` from the motor-header side, same as V2.1 (T3) |
| Limit switch module | active (0 V) and LED on while pressed; inputs X, Y, Z each read their own switch (T3) |
| Screen: why it stayed dark | the two ribbons were swapped; corrected (section 5) |
| X driver module after it was taken out | put back before power-on, same way round as Y and Z |
| Does the serial port disappear when the supply is unplugged | **TBD** |
