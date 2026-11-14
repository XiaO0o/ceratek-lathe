# Ceratek lathe control

Control software, controller notes and bench-test records for the Ceratek ceramic tube
refinement lathe (University of Ottawa, GNG5140, Group 3).

The lathe turns a fired ceramic tube while a blade is fed along it, to bring the wall to a
chosen thickness. Three stepper motors do the work - tube rotation, feed along the tube (Z)
and depth of cut (Y) - driven by an MKS DLC32 V2.2 controller with TMC2209 drivers. A computer
on USB tells the controller what to do. This repository holds that computer-side software and
everything needed to set up and test the controller.

## Status

As of 2026-11-14. The machine frame was not available yet, so everything below was done on
the bench with the controller, one stand-in motor and three limit switches.

| Area | State |
| --- | --- |
| Controller | MKS DLC32 **V2.2**. Only V2.1 drawings are published, so [docs/hardware.md](docs/hardware.md) marks each fact as seen on our board or taken from the V2.1 drawings |
| Firmware | FluidNC v4.1.1 with [firmware/fluidnc/config.yaml](firmware/fluidnc/config.yaml). The firmware as delivered had no stop input; it is backed up and can be put back |
| Bench tests | [docs/bench-tests.md](docs/bench-tests.md), with the captures in `records/`. Done: serial link, limit inputs, motor drivers, stop input, homing sequence, one dry pass |
| Pass planning | Written and tested; one planned pass has run on the board (dry, rotation motor only) |
| Machine driver: homing, running a pass, stop and restart rules | Written and tested; has run on the board for that dry pass and for homing failures |
| Operator sequence, `python -m lathe run` | Written and tested against the simulator; not yet run on the board |

### Still to do, on the machine

These cannot be done on the bench:

- Move Y and Z with their own motors; confirm directions and steps per millimetre. Under FluidNC
  only the rotation output has turned a motor.
- Home through the software with the carriages pressing the switches. By hand it was too
  dependent on timing to complete.
- Measure and set the driver current (needs a multimeter), then hold the motors when idle
  (`idle_ms: 255`). They are released for now because the stand-in motor overheated when held.
- Find the highest reliable rotation speed. 10 to 60 rev/min ran cleanly; nothing faster was tried.
- Replace the plain jumper plugs on the motor and limit switch leads with latching plugs or
  crimped terminals. All three faults on the bench were plugs that had worked loose.
- Set the values in [config/lathe.toml](config/lathe.toml) and the speeds and travels in the
  FluidNC configuration from cutting trials and the machine's real dimensions. Every one of
  them is a bench starting point.
- Run `python -m lathe run` on the machine with an operator.

## Quick start

Python 3.11 or newer.

```bash
python3 -m venv .venv
source .venv/bin/activate          # Windows: .venv\Scripts\activate
python -m pip install -e ".[dev]"
python -m pytest -q                # needs no hardware
python -m lathe run --sim          # walk through a whole job on the built-in simulator
```

With the controller powered from its 12 V supply (it does not run from USB) and connected by USB:

```bash
python -m lathe ports              # which serial port is the controller?
python -m lathe probe              # T2: read firmware version and all settings, save a record
python -m lathe monitor            # T3: watch the limit / probe inputs while pressing switches
python -m lathe run                # refine one tube: the operator's guided sequence
```

`ports`, `probe` and `monitor` only ask the controller questions. `run` moves the machine, and
asks before every homing cycle and every pass.

## The stop switch

- Pressing it stops all motion at once. Nothing can be sent while it is pressed.
- Releasing it does not bring the machine back. The software has to recover the controller
  and home again before it will cut, because a hard stop can lose steps.
- Unplugging the 12 V supply turns the controller off; the serial port disappears from the
  computer. After power returns the machine has to be homed.

`run` leads the operator through this. The behaviour was recorded on the bench; see
"Stop input" in [docs/bench-tests.md](docs/bench-tests.md).

## Firmware

The controller runs FluidNC. To change its configuration, edit
[firmware/fluidnc/config.yaml](firmware/fluidnc/config.yaml), upload it to the controller as
`config.yaml` (FluidNC's `$Xmodem/Receive=config.yaml`, or its web page) and restart it. The
steps used on 2026-11-14, including how the firmware as delivered was backed up and how to
write it back, are under "Firmware change" in [docs/bench-tests.md](docs/bench-tests.md).
The backup file itself is kept outside the repository because it holds Wi-Fi settings.

## Layout

```
lathe/            the Python package
  grbl.py         what the controller says, parsed: status reports, alarm and error codes
  link.py         one conversation with the controller: commands, replies, timeouts
  ports.py        finding the controller's serial port
  sim.py          an in-memory stand-in for the controller, for tests and dry runs
  records.py      saving captures as text, with network details masked
  machine.py      runs the lathe: homing, sending a pass, stop and restart rules
  session.py      the operator's guided sequence for one tube
  passes.py       from the operator's diameters to the moves of one cutting pass
  settings.py     reads the cutting parameters from config/lathe.toml
  cli.py          the command line (python -m lathe ...)
config/           cutting parameters (speeds, depths, directions)
firmware/         the FluidNC configuration that runs on the controller
docs/             hardware notes and the bench test plan
records/          captures from real test runs, committed as evidence
tests/            automated tests (python -m pytest)
```

## Conventions

- Machine-specific numbers (steps per millimetre, speeds, travel) go in configuration, never in code.
- A capture in `records/` is evidence: add new ones, do not edit old ones.
- Firmware images and flash backups (`*.bin`) are never committed. A backup contains the Wi-Fi
  name and password stored on the board.
