# Ceratek lathe control

Control software, controller notes and bench-test records for the Ceratek ceramic tube
refinement lathe (University of Ottawa, GNG5140, Group 3).

The lathe turns a fired ceramic tube while a blade is fed along it, to bring the wall to a
chosen thickness. Three stepper motors do the work - tube rotation, feed along the tube (Z)
and depth of cut (Y) - driven by an MKS DLC32 V2.2 controller with TMC2209 drivers. A computer
on USB tells the controller what to do. This repository holds that computer-side software and
everything needed to set up and test the controller.

## Status

| Area | State |
| --- | --- |
| Controller pin map and wiring rules | [docs/hardware.md](docs/hardware.md). Our board is a V2.2 and the published drawings stop at V2.1, so each fact is marked as seen on the board or taken from the V2.1 drawings |
| Bench test plan, T0 to T9 | [docs/bench-tests.md](docs/bench-tests.md). T0 to T4 done: the link, the limit inputs and all three motor drivers work. T5 and T6 are next |
| Firmware as delivered | MKS `V2.2.6`, captured in `records/`. It is set up as a laser engraver and has no stop input |
| Serial link, status parsing, simulator | Working, covered by automated tests |
| `probe` bench tool (T2) | Run on the board |
| `monitor` bench tool (T3) | Run on the board |
| Firmware | FluidNC v4.1.1 flashed on 2026-11-14 with [firmware/fluidnc/config.yaml](firmware/fluidnc/config.yaml); it started without a configuration error. The limit, stop and motor tests have to be repeated under it |
| Pass planning (depths, moves of a pass, correction from the measured diameter) | Written and tested without hardware; no move has been run from it yet |
| Machine driver: homing, sending a pass, stop and restart rules | Written and tested against the simulator, which copies the stop behaviour seen on the bench; not yet run on the board |
| Operator workflow: `python -m lathe run` guides homing, the four entries, touching off, each pass and each measurement | Written and tested against the simulator; not yet run on the board. Try it with `python -m lathe run --sim` |

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
```

These tools only ask the controller questions. They do not move a motor or change a setting.

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
