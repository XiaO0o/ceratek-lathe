# Ceratek lathe control

Control software, controller notes and bench-test records for the Ceratek ceramic tube
refinement lathe (University of Ottawa, GNG5140, Group 3).

The lathe turns a fired ceramic tube while a blade is fed along it, to bring the wall to a
chosen thickness. Three stepper motors do the work - tube rotation, feed along the tube (Z)
and depth of cut (Y) - driven by an MKS DLC32 V2.1 controller with TMC2209 drivers. A computer
on USB tells the controller what to do. This repository holds that computer-side software and
everything needed to set up and test the controller.

## Status

| Area | State |
| --- | --- |
| Controller pin map and wiring rules | Checked against the manufacturer's drawings: [docs/hardware.md](docs/hardware.md) |
| Bench test plan, T0 to T9 | Written; results are filled in as the tests are run: [docs/bench-tests.md](docs/bench-tests.md) |
| Serial link, status parsing, simulator | Working, covered by automated tests |
| `probe` and `monitor` bench tools (tests T2, T3) | Working against the simulator; not yet run on the board |
| Firmware choice, motion, homing, stop handling, operator workflow | Not started |

## Quick start

Python 3.10 or newer.

```bash
python3 -m venv .venv
source .venv/bin/activate          # Windows: .venv\Scripts\activate
python -m pip install -e ".[dev]"
python -m pytest -q                # needs no hardware
python -m lathe probe --sim        # try a tool against the built-in simulator
```

With the controller connected by USB:

```bash
python -m lathe ports              # which serial port is the controller?
python -m lathe probe              # T2: read firmware version and all settings, save a record
python -m lathe monitor            # T3: watch the limit / probe inputs while pressing switches
```

These tools only ask the controller questions. They do not move a motor or change a setting.
Opening the port restarts the controller, as it does with any serial program.

## Layout

```
lathe/            the Python package
  grbl.py         what the controller says, parsed: status reports, alarm and error codes
  link.py         one conversation with the controller: commands, replies, timeouts
  ports.py        finding the controller's serial port
  sim.py          an in-memory stand-in for the controller, for tests and dry runs
  records.py      saving captures as text, with network details masked
  cli.py          the command line (python -m lathe ...)
docs/             hardware notes and the bench test plan
records/          captures from real test runs, committed as evidence
tests/            automated tests (python -m pytest)
```

## Conventions

- Machine-specific numbers (steps per millimetre, speeds, travel) go in configuration, never in code.
- A capture in `records/` is evidence: add new ones, do not edit old ones.
- Firmware images and flash backups (`*.bin`) are never committed. A backup contains the Wi-Fi
  name and password stored on the board.
