# Controller hardware notes

What the wiring and the software have to respect.

Facts in this file were read from the manufacturer's drawings for the MKS DLC32 V2.1
(`MKS DLC32 V2.1_003 PIN.pdf` and `SCH.pdf` in <https://github.com/makerbase-mks/MKS-DLC32>,
checked 2026-11-13). Anything marked **TBD** has not been checked on our own board yet;
section 8 lists what still has to be recorded.

## 1. What is on the bench

| Part | Detail |
| --- | --- |
| Controller | MKS DLC32 (ESP32), expected revision V2.1 - **TBD** confirm from the silkscreen |
| Stepper drivers | 3 x TMC2209 plug-in modules under blue heatsinks - **TBD** module make |
| Screen | MKS TS35-R V2.0, 3.5 inch touch screen on the two EXP ribbon cables |
| Fan | On the 2-pin `+12/24V-` header - **TBD** rated voltage |
| Power supply | DC barrel jack, 12-24 V accepted by the board - **TBD** voltage and current |
| Test motor | Prusa MMU2S "Pulley" NEMA17, 1.8 deg/step (stand-in for the machine's motors) |
| Limit switches | 3 x mechanical end-stop modules with LED, 3-wire cables |
| USB | USB-A to USB-B cable; the board's USB-serial chip is a CH340, 115200 baud |

## 2. Connectors

Board seen from above with the motor headers at the top edge and the USB / DC sockets at the bottom:

```
 X-Motor       Y1-Motor      Y2-Motor      Z-Motor      fan      TTL
[2B 2A 1A 1B] [2B 2A 1A 1B] [2B 2A 1A 1B] [2B 2A 1A 1B] [+ -] [IO32 GND V]

                                 I2C    Probe   Z-     Y-     X-
                        top ->   3V3    5V      5V     5V     5V
                                 GND    GND     GND    GND    GND
                     bottom ->   IO0    IO22    IO34   IO35   IO36
                                 IO4
```

- Motor headers: 4 pins, 2.54 mm pitch, order `2B 2A 1A 1B`. The two pins of one motor coil
  are next to each other (`2B 2A` is one coil, `1A 1B` the other).
- `Y1-Motor` and `Y2-Motor` are wired to the same driver. The lathe uses `Y1-Motor` only.
- Limit and probe headers: 3 pins, `5V / GND / S`. On V2.1_003 boards the labels for these
  headers are printed on the underside.
- Below each driver socket is a 4-pin `E S D G` header (enable, step, direction, ground) meant
  for external drivers. Not used.

### Signals

| Function | Connector | ESP32 pin | Notes |
| --- | --- | --- | --- |
| Step / direction, X | driver socket X | I2SO.1 / I2SO.2 | through the shift register, see below |
| Step / direction, Y | driver socket Y | I2SO.5 / I2SO.6 | |
| Step / direction, Z | driver socket Z | I2SO.3 / I2SO.4 | |
| Driver enable | all three sockets | I2SO.0 | one signal shared by X, Y and Z |
| Beeper | `BEEPER` header, EXP1 | I2SO.7 | |
| Shift register clock / latch / data | 74HC595 | IO16 / IO17 / IO21 | |
| X limit | `X-` | IO36 | input only |
| Y limit | `Y-` | IO35 | input only |
| Z limit | `Z-` | IO34 | input only |
| Probe | `Probe` | IO22 | input with clamp diodes |
| Laser / spindle PWM | `TTL` (5 V logic) and `SPINDLE` (switched to ground) | IO32 | output only |
| I2C | `I2C` | IO0 (SDA), IO4 (SCL) | IO0 also selects the boot mode: must not be held low at power-up |
| SD card | card slot | IO12, IO13, IO14, IO15, IO39 | |
| Screen | EXP1 / EXP2 | IO18, IO19, IO23, IO33, IO25, IO26, IO27, IO5 | |

Step and direction do not come straight from ESP32 pins. They are shifted out through a
74HC595 register (the "I2SO" outputs above). Firmware therefore needs an I2S stepping engine,
which the stock firmware and FluidNC both have. This is the main reason the project configures
an existing firmware instead of writing its own.

Each limit input has, on the board, a pull-up to 3.3 V and a small filter capacitor at the
connector, then a series resistor to the ESP32. The supply pin on those headers is 5 V.

## 3. Stepper drivers

The driver sockets carry only step, direction, enable and three microstep DIP switches.
The TMC2209's serial pin (PDN_UART) and its diagnostic pin (DIAG) are **not** connected to the
ESP32. The drivers therefore run in stand-alone mode:

- Motor current is set by the small potentiometer on each driver module (its "Vref"), not by software.
- Microstepping is set by the DIP switches under each driver, not by software.
- The controller cannot read anything back from a driver: no load value, no stall flag,
  no over-temperature flag, no current.
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

The DIP switches are labelled M0, M1, M2 and go to the socket's MS1, MS2 and MS3 positions.
What the third switch does depends on the make of TMC2209 module (**TBD**). Leave the switches
as delivered until bench test T5 has measured the real steps per revolution.

Motor current: for the common TMC2209 modules with 0.11 ohm sense resistors,
`I_rms = 0.71 x Vref` (Vref in volts, measured with a multimeter between the potentiometer
and ground with the board powered). **TBD**: confirm the module make and measure the three
Vref values before any long run.

All three drivers share one enable signal: the motors are energised or released together.

## 4. Power

```
DC jack 12-24 V --> on-board switch / fuse --> motor supply --> drivers, fan header, spindle output
                                                    |
                                                    +--> 5 V regulator --+--> 5 V and 3.3 V logic
USB 5 V ---------------------------------------------------------------+
```

- **USB alone** powers the ESP32, the screen and the limit inputs. The motors have no supply
  and cannot move. Bench tests T2 and T3 are done this way.
- **The on-board power switch cuts only the DC input.** With USB still connected the ESP32
  keeps running and does not know the motors lost power: it will go on reporting a position
  that is no longer true. After the switch has been used, the machine must be re-homed.
  This is part of the stop / restart definition.
- The 2-pin `12/24V` headers (where the fan is plugged in) carry the DC input voltage
  directly. A 12 V fan on a 24 V supply will be destroyed.

## 5. Planned lathe wiring

A proposal, to be confirmed together with the firmware choice and the mechanical layout.

| Machine function | Controller axis | Motor header | Home switch |
| --- | --- | --- | --- |
| Tube rotation | X | `X-Motor` | none |
| Depth of cut (the team's Y axis) | Y | `Y1-Motor` | `Y-` (IO35), at the retracted end |
| Feed along the tube (the team's Z axis) | Z | `Z-Motor` | `Z-` (IO34), at the start end |
| Stop switch | - | - | `Probe` (IO22) or `X-` (IO36), decided with the firmware |

This keeps the axis names used in the design reports, and leaves the X limit input free
because a rotating axis has no end to find.

## 6. Test motor

The Prusa MMU2S pulley motor is rated 1 A per phase and ends in a 4-way Molex 50-57-9404
plug (2.54 mm pitch). It does not latch into the board's motor header, so it is connected with
four male-female jumper wires: male end into the motor plug, female end onto the header pin.

Prusa does not publish the wire order of this plug, so find the coil pairs before powering it:

1. Motor disconnected from everything. Turn the shaft by hand: it turns freely.
2. Bridge holes 1 and 2 of the plug with a jumper wire and turn again. If the shaft is clearly
   harder to turn and feels notchy, holes 1-2 are one coil and holes 3-4 the other.
3. If nothing changed, try 1-3, then 1-4.
4. Wire each coil to two neighbouring header pins (`2B 2A` or `1A 1B`). Which coil goes where,
   and which way round, only affects the direction of rotation, which is set in software.

| Plug hole | Wire colour | Same coil as hole | Goes to header pin |
| --- | --- | --- | --- |
| 1 | **TBD** | **TBD** | **TBD** |
| 2 | **TBD** | **TBD** | **TBD** |
| 3 | **TBD** | **TBD** | **TBD** |
| 4 | **TBD** | **TBD** | **TBD** |

The test motor stands in for the machine's motors to prove wiring, logic and the control
sequence. Speeds, steps per millimetre and directions are measured again on the machine,
which is why they live in configuration and not in code.

## 7. Handling rules

1. Never plug or unplug a motor while the DC supply is on. It can destroy the driver.
2. Switch the DC supply off before moving the motor to another header.
3. Limit switch cables: match `5V / GND / S` to the labels on both the board and the switch
   module. A 3-pin jumper-style plug fits either way round, and reversed it puts 5 V straight
   across the switch contacts, which is a short circuit when the switch closes.
4. Check the fan's rated voltage against the power supply before first power-up.
5. Until the driver current has been measured, keep motor runs short and check by hand that
   the motor and the driver heatsinks stay comfortable to touch.
6. Keep fingers, sleeves and cables away from the motor shaft and its gears when it is powered.

## 8. To be recorded

| Item | Value |
| --- | --- |
| Board revision on the silkscreen | **TBD** |
| Power supply label: voltage, current | **TBD** |
| Fan label: voltage | **TBD** |
| Driver module marking | **TBD** |
| DIP switch positions, X / Y / Z (M0 M1 M2) | **TBD** |
| Vref, X / Y / Z | **TBD** (needs a multimeter) |
| Limit switch module: output when pressed, LED when pressed | **TBD** (bench test T3) |
| Firmware version as delivered | **TBD** (bench test T2) |
