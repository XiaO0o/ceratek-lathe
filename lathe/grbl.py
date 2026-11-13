"""What the controller says, turned into Python values.

The lathe's controller (MKS DLC32) runs a Grbl-family firmware. The stock MKS
firmware and FluidNC both speak the Grbl 1.1 serial protocol, so everything in
this module applies to either one.
"""

from __future__ import annotations

from dataclasses import dataclass, field

# Real-time commands: single bytes the controller acts on immediately, even in
# the middle of a move. They are never queued and never answered with "ok".
RT_STATUS = b"?"
RT_FEED_HOLD = b"!"
RT_CYCLE_START = b"~"
RT_SOFT_RESET = b"\x18"

# Alarm codes 1-9 are defined by Grbl 1.1 and shared by both firmwares. Higher
# numbers are firmware-specific and are described by number only.
ALARMS = {
    1: "hard limit triggered; position is lost, re-home",
    2: "soft limit: target is outside the machine travel",
    3: "reset while moving; position is lost, re-home",
    4: "probe fail: probe was already triggered before the cycle",
    5: "probe fail: no contact within the programmed travel",
    6: "homing fail: reset during the cycle",
    7: "homing fail: safety door opened during the cycle",
    8: "homing fail: switch still triggered after pull-off",
    9: "homing fail: switch not found within the search distance",
}

# Error codes defined by Grbl 1.1. Both firmwares add their own above these.
ERRORS = {
    1: "expected a command letter",
    2: "bad number format",
    3: "invalid or unsupported '$' command",
    4: "negative value not allowed",
    5: "homing is not enabled in the settings",
    6: "step pulse time too short",
    7: "settings storage read failed; defaults restored",
    8: "command only allowed when idle",
    9: "locked out: controller is in alarm or jog state",
    10: "soft limits need homing enabled",
    11: "line too long",
    12: "setting exceeds the maximum step rate",
    13: "safety door open",
    14: "startup line too long",
    15: "jog target exceeds machine travel",
    16: "invalid jog command",
    17: "laser mode needs a PWM spindle",
    20: "unsupported or invalid G-code command",
    21: "two commands from the same modal group",
    22: "feed rate not set",
    23: "command needs an integer value",
    24: "two commands both need axis words",
    25: "repeated G-code word",
    26: "axis words missing",
    27: "invalid line number",
    28: "missing P or L value",
    29: "work coordinate system not supported",
    30: "G53 needs G0 or G1",
    31: "axis words with no motion mode active",
    32: "arc has no axis words in the plane",
    33: "invalid motion target",
    34: "arc radius error",
    35: "arc is missing its offset words",
    36: "unused G-code words in the line",
    37: "tool length offset on the wrong axis",
    38: "tool number too large",
}

# Letters that can appear in the "Pn:" field of a status report.
PIN_NAMES = {
    "X": "X limit input",
    "Y": "Y limit input",
    "Z": "Z limit input",
    "P": "probe input",
    "D": "safety door input",
    "H": "feed hold input",
    "R": "reset input",
    "S": "cycle start input",
    "E": "e-stop input (FluidNC)",
    "F": "fault input (FluidNC)",
}


def describe_alarm(code: int) -> str:
    return ALARMS.get(code, f"firmware-specific alarm {code}")


def describe_error(code: int) -> str:
    return ERRORS.get(code, f"firmware-specific error {code}")


def describe_pin(letter: str) -> str:
    return PIN_NAMES.get(letter, f"input '{letter}'")


@dataclass(frozen=True)
class Status:
    """One real-time status report, e.g. ``<Idle|MPos:0.000,0.000,0.000|FS:0,0|Pn:Y>``."""

    raw: str
    state: str
    substate: int | None = None
    mpos: tuple[float, ...] | None = None
    wpos: tuple[float, ...] | None = None
    feed: float | None = None
    pins: frozenset[str] = frozenset()
    fields: dict[str, str] = field(default_factory=dict)

    @property
    def alarmed(self) -> bool:
        return self.state == "Alarm"


def is_status_report(line: str) -> bool:
    text = line.strip()
    return len(text) >= 3 and text.startswith("<") and text.endswith(">")


def parse_status(line: str) -> Status:
    """Parse a ``<...>`` status report. Raises ValueError if it is not one."""
    text = line.strip()
    if not is_status_report(text):
        raise ValueError(f"not a status report: {line!r}")

    head, *rest = text[1:-1].split("|")
    state, _, sub = head.partition(":")
    if not state.isalpha():
        raise ValueError(f"malformed status report: {line!r}")

    fields: dict[str, str] = {}
    for part in rest:
        key, _, value = part.partition(":")
        if key:
            fields[key] = value

    try:
        substate = int(sub) if sub else None
        mpos = _floats(fields.get("MPos"))
        wpos = _floats(fields.get("WPos"))
        speeds = _floats(fields.get("FS") or fields.get("F"))
    except ValueError as exc:
        raise ValueError(f"malformed status report: {line!r}") from exc

    return Status(
        raw=text,
        state=state,
        substate=substate,
        mpos=mpos,
        wpos=wpos,
        feed=speeds[0] if speeds else None,
        pins=frozenset(fields.get("Pn", "")),
        fields=fields,
    )


def _floats(value: str | None) -> tuple[float, ...] | None:
    if not value:
        return None
    return tuple(float(item) for item in value.split(","))
