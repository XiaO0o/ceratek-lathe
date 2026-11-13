"""Command line for the bench tools: ``python -m lathe <command>``.

    ports     list the serial ports that could be the controller
    probe     bench test T2: read the firmware identity and every setting, save a record
    monitor   bench test T3: watch the controller's inputs while switches are pressed

Nothing here moves a motor or changes a setting.
"""

from __future__ import annotations

import argparse
import platform
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path

from . import __version__, grbl, ports, records
from .link import BAUD, GrblLink, GrblTimeout
from .sim import SimulatedGrbl

# Read-only queries. A firmware that lacks one answers "error:3", which is recorded as such.
PROBE_COMMANDS = ("$I", "$$", "$S", "$G", "$#", "$N", "$CMD")

BOOT_SECTION = "boot (after opening the port)"
# Whether opening the port restarts the ESP32 depends on the computer's serial
# driver. Our board on macOS does not restart, so there is no boot text to keep.
NO_RESTART = "(nothing: the controller did not restart when the port was opened)"


@dataclass(frozen=True)
class Timing:
    """How patient to be with the board. The real one needs seconds to boot."""

    quiet: float = 1.5
    max_wait: float = 10.0
    first_byte: float = 3.0
    ready_timeout: float = 2.0
    ready_attempts: int = 4
    command_timeout: float = 5.0


SIMULATOR_TIMING = Timing(
    quiet=0.02, max_wait=0.5, first_byte=0.05, ready_timeout=0.2, ready_attempts=1, command_timeout=0.5
)


@dataclass
class ProbeResult:
    sections: list[tuple[str, list[str]]]
    responding: bool
    status: grbl.Status | None = None


@dataclass
class MonitorSummary:
    polls: int = 0
    initial: frozenset[str] = frozenset()
    activations: dict[str, int] = field(default_factory=dict)


# -- T2: probe ------------------------------------------------------------


def wait_until_ready(link: GrblLink, timing: Timing) -> list[str] | None:
    """Wait for the boot text to end and the controller to answer an empty line.

    Returns the boot text, or None if the controller never answered.
    """
    boot = link.drain(quiet=timing.quiet, max_wait=timing.max_wait, first_byte=timing.first_byte)
    for _ in range(timing.ready_attempts):
        try:
            link.command("", timeout=timing.ready_timeout)
            return boot
        except GrblTimeout as late:
            boot += late.lines  # a slow boot keeps printing while we wait
    return None


def run_probe(link: GrblLink, commands=PROBE_COMMANDS, timing: Timing = Timing()) -> ProbeResult:
    """Ask the controller who it is and how it is configured. Read-only."""
    boot = wait_until_ready(link, timing)
    if boot is None:
        heard = [text for direction, text in link.transcript if direction == "<"]
        return ProbeResult([(BOOT_SECTION, heard)], responding=False)

    sections = [(BOOT_SECTION, boot or [NO_RESTART])]
    for command in commands:
        try:
            reply = link.command(command, timeout=timing.command_timeout)
            lines = reply.lines + ["ok" if reply.ok else reply.describe_error()]
        except GrblTimeout as timeout:
            lines = timeout.lines + ["(timed out waiting for the end of the reply)"]
        sections.append((command, lines))

    try:
        status = link.status()
        sections.append(("? (status report)", [status.raw]))
    except (GrblTimeout, ValueError):
        status = None
        sections.append(("? (status report)", ["(no status report)"]))
    return ProbeResult(sections, responding=True, status=status)


def _probe(args) -> int:
    link, label = _connect(args)
    try:
        print(f"Probing {label}; waiting for the controller to boot ...", flush=True)
        result = run_probe(link, PROBE_COMMANDS + tuple(args.extra), _timing(args))
    finally:
        link.close()

    now = datetime.now(timezone.utc)
    header = {
        "captured": now.strftime("%Y-%m-%dT%H:%M:%SZ"),
        "tool": f"lathe {__version__} probe on {platform.system()} {platform.machine()}, Python {platform.python_version()}",
        "port": "SIMULATOR - not a real board" if args.sim else label,
        "network details masked": "no" if args.raw else "yes",
    }
    text = records.render("Controller probe (bench test T2)", header, result.sections, redact=not args.raw)
    path = records.save(Path(args.out), "probe", text, now.astimezone())

    if not result.responding:
        print("The port opened but the controller did not answer.", file=sys.stderr)
        print("Check: is this the right port, and is another program (a G-code sender) holding it?", file=sys.stderr)
        print(f"What was heard is saved in {path}", file=sys.stderr)
        return 3

    lines_by_section = dict(result.sections)
    banner = next((line for line in lines_by_section[BOOT_SECTION] if line.lower().startswith("grbl")), None)
    identity = [line for line in lines_by_section["$I"] if line.startswith("[") and not line.startswith("[MSG")]
    settings = [line for line in lines_by_section["$$"] if line.startswith("$")]
    print("The controller answered.")
    print(f"  banner   : {banner or 'none (the controller did not restart when the port was opened)'}")
    print(f"  identity : {' '.join(identity) or '(nothing recognisable in the $I reply)'}")
    print(f"  settings : {len(settings)} numbered settings read")
    if result.status:
        print(f"  state    : {result.status.state}, active inputs: {_pin_list(result.status.pins)}")
    print(f"Record saved: {path}")
    return 0


# -- T3: monitor ----------------------------------------------------------


def run_monitor(link: GrblLink, *, interval=0.2, duration=None, max_polls=None, emit=print, on_poll=None) -> MonitorSummary:
    """Poll the status report and report every change of state or active inputs.

    Ctrl-C ends the watch normally, so the summary of what was seen is kept.
    """
    summary = MonitorSummary()
    start = time.monotonic()
    previous: tuple[str, frozenset[str]] | None = None
    misses = 0
    try:
        while True:
            if max_polls is not None and summary.polls >= max_polls:
                break
            if duration is not None and time.monotonic() - start >= duration:
                break
            if on_poll is not None:
                on_poll(summary.polls)
            summary.polls += 1

            try:
                status = link.status()
            except (GrblTimeout, ValueError):
                misses += 1
                if misses >= 3:
                    raise GrblTimeout("the controller stopped answering status requests") from None
                continue
            misses = 0

            stamp = f"{time.monotonic() - start:7.2f}s"
            if previous is None:
                summary.initial = status.pins
                emit(f"{stamp}  state={status.state:<6} active inputs: {_pin_list(status.pins)}")
            elif (status.state, status.pins) != previous:
                went_on, went_off = status.pins - previous[1], previous[1] - status.pins
                for pin in went_on:
                    summary.activations[pin] = summary.activations.get(pin, 0) + 1
                changes = [f"+{pin}" for pin in sorted(went_on)] + [f"-{pin}" for pin in sorted(went_off)]
                emit(f"{stamp}  state={status.state:<6} active inputs: {_pin_list(status.pins):<8} {' '.join(changes)}")
            previous = (status.state, status.pins)

            if interval:
                time.sleep(interval)
    except KeyboardInterrupt:
        pass
    return summary


def _monitor(args) -> int:
    link, label = _connect(args)
    try:
        print(f"Connecting to {label}; waiting for the controller to boot ...", flush=True)
        if wait_until_ready(link, _timing(args)) is None:
            print("The port opened but the controller did not answer.", file=sys.stderr)
            return 3
        print("Watching the inputs. Press and release each switch in turn; Ctrl-C to stop.")
        print("A letter is listed while the firmware considers that input active (+ went on, - went off).")
        try:
            summary = run_monitor(
                link, interval=args.interval, duration=args.duration, emit=lambda text: print(text, flush=True)
            )
        except GrblTimeout as timeout:
            print(f"Stopped: {timeout}", file=sys.stderr)
            return 3
    finally:
        link.close()

    print(f"\n{summary.polls} status polls.")
    print(f"  active at the start : {_pin_list(summary.initial)}")
    switched = ", ".join(f"{pin} ({count}x)" for pin, count in sorted(summary.activations.items()))
    print(f"  switched on         : {switched or 'none'}")
    for pin in sorted(summary.initial | set(summary.activations)):
        print(f"  {pin} = {grbl.describe_pin(pin)}")
    return 0


# -- shared ---------------------------------------------------------------


def _ports(args) -> int:
    candidates = ports.scan()
    if not candidates:
        print("No serial ports found.")
        return 2
    for candidate in candidates:
        mark = "*" if candidate.chip else " "
        print(f"{mark} {candidate.label}  {candidate.description}".rstrip())
    print("* = USB-serial adapter (the MKS DLC32 shows up as a CH340)")
    return 0


def _connect(args) -> tuple[GrblLink, str]:
    if args.sim:
        return GrblLink(SimulatedGrbl()), "the simulator (no hardware)"
    candidate = ports.choose(args.port)
    return GrblLink.open_serial(candidate.device, args.baud), candidate.label


def _timing(args) -> Timing:
    return SIMULATOR_TIMING if args.sim else Timing()


def _pin_list(pins) -> str:
    return "".join(sorted(pins)) or "none"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="lathe", description="Bench tools for the Ceratek lathe controller.")
    parser.add_argument("--version", action="version", version=f"lathe {__version__}")
    commands = parser.add_subparsers(dest="command", required=True)

    commands.add_parser("ports", help="list the serial ports that could be the controller").set_defaults(run=_ports)

    connection = argparse.ArgumentParser(add_help=False)
    connection.add_argument("--port", help="serial device, e.g. /dev/cu.usbserial-110 or COM5 (default: auto-detect)")
    connection.add_argument("--baud", type=int, default=BAUD, help=f"baud rate (default {BAUD})")
    connection.add_argument("--sim", action="store_true", help="talk to the built-in simulator instead of a board")

    probe = commands.add_parser("probe", parents=[connection], help="T2: read firmware identity and settings")
    probe.add_argument("--out", default="records", help="folder for the record file (default: records)")
    probe.add_argument("--raw", action="store_true", help="do not mask network details in the record")
    probe.add_argument("--extra", action="append", default=[], metavar="CMD", help="one more read-only command to send")
    probe.set_defaults(run=_probe)

    monitor = commands.add_parser("monitor", parents=[connection], help="T3: watch the limit/probe inputs live")
    monitor.add_argument("--interval", type=float, default=0.2, help="seconds between polls (default 0.2)")
    monitor.add_argument("--duration", type=float, help="stop after this many seconds (default: until Ctrl-C)")
    monitor.set_defaults(run=_monitor)

    args = parser.parse_args(argv)
    try:
        return args.run(args)
    except ports.PortError as problem:
        print(problem, file=sys.stderr)
        return 2
    except GrblTimeout as timeout:
        print(f"The controller stopped answering: {timeout}", file=sys.stderr)
        return 3
    except OSError as problem:  # includes serial.SerialException: port busy, unplugged, no permission
        print(f"Serial port problem: {problem}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
