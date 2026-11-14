"""Running the lathe: homing, sending moves, and what happens around a stop.

This is where the stop and restart behaviour defined for the machine is enforced in software:

- A stop (the stop switch, a limit switch, or any other alarm) ends whatever was running
  and is reported to the caller as ``Stopped``.
- Nothing can be sent while the stop switch is still pressed.
- After any stop the machine must be recovered deliberately and then homed before it will
  cut again. The controller itself does not insist on homing after a reset, and a hard
  stop can lose steps, so the old position is not trusted.

Commands are never simply waited on. While the stop input has the controller locked it
answers neither ``ok`` nor ``error``, so every wait here also watches the status report.
"""

from __future__ import annotations

import time

from . import grbl
from .link import GrblLink

STOP_PIN = "E"
BUSY_STATES = ("Run", "Jog", "Home", "Hold")


class MachineError(RuntimeError):
    """The machine cannot do what was asked. The message is written for the operator."""


class Stopped(MachineError):
    """The machine stopped itself during an operation: stop switch, limit switch or alarm."""


class Machine:
    def __init__(self, link: GrblLink, *, poll_s: float = 0.1, reply_s: float = 5.0, reset_wait_s: float = 2.0) -> None:
        self.link = link
        self.needs_homing = True  # until a homing cycle has finished in this session
        self._poll_s = poll_s
        self._reply_s = reply_s
        self._reset_wait_s = reset_wait_s

    # -- looking ---------------------------------------------------------

    def status(self) -> grbl.Status:
        return self.link.status()

    def stop_switch_pressed(self) -> bool:
        return STOP_PIN in self.status().pins

    # -- restart after a stop --------------------------------------------

    def recover(self) -> None:
        """Clear a stop. Refused while the stop switch is pressed; homing is needed afterwards."""
        if self.stop_switch_pressed():
            raise MachineError("The stop switch is still pressed. Release it first.")
        self.needs_homing = True
        self.link.realtime(grbl.RT_SOFT_RESET)
        self._sleep(self._reset_wait_s)
        self.link.drain(quiet=min(0.5, self._reset_wait_s), max_wait=self._reset_wait_s + 1, first_byte=self._reset_wait_s)
        status = self.status()
        if STOP_PIN in status.pins:
            raise MachineError("The stop switch is still pressed. Release it first.")
        if status.alarmed:
            self._command("$X", from_alarm=True)  # the start-up lock; homing is still required below

    def home(self, timeout_s: float = 180.0) -> None:
        """Find the limit switches. The only way ``needs_homing`` is cleared."""
        status = self.status()
        if STOP_PIN in status.pins:
            raise MachineError("The stop switch is pressed. Release it and recover first.")
        if status.state in BUSY_STATES:
            raise MachineError(f"The machine is busy ({status.state}).")
        self.needs_homing = True
        self._command("$H", timeout_s=timeout_s, what="Homing", from_alarm=status.alarmed)
        self.needs_homing = False

    # -- moving ----------------------------------------------------------

    def run(self, lines, timeout_s: float | None = None) -> None:
        """Send the moves of a pass and wait until they have all been made."""
        if self.needs_homing:
            raise MachineError("The machine has not been homed since it was started or stopped. Home it first.")
        self._require_idle()
        for line in lines:
            self._command(line)
        self._wait_until_idle(timeout_s)

    def jog(self, axis: str, distance: float, feed: float, timeout_s: float | None = None) -> None:
        """A single relative move, for setting up and touching off. Allowed before homing."""
        self._require_idle()
        self._command(f"$J=G91 {axis}{distance:.4f} F{feed:g}")
        self._wait_until_idle(timeout_s)

    def hold(self) -> None:
        """Pause: slow down and stop without losing position. ``resume`` continues the move."""
        self.link.realtime(grbl.RT_FEED_HOLD)

    def resume(self) -> None:
        self.link.realtime(grbl.RT_CYCLE_START)

    # -- waiting, always with an eye on the status -----------------------

    def _require_idle(self) -> None:
        status = self.status()
        if STOP_PIN in status.pins:
            raise MachineError("The stop switch is pressed.")
        if status.alarmed:
            raise MachineError("The machine is in alarm. Recover and home it first.")
        if status.state != "Idle":
            raise MachineError(f"The machine is busy ({status.state}).")

    def _command(
        self, line: str, timeout_s: float | None = None, what: str | None = None, from_alarm: bool = False
    ) -> None:
        """Send a line and wait for its ``ok``, giving up at once if the machine stops.

        ``from_alarm`` is for the two commands that are sent while the controller is already
        in its start-up alarm (unlock and homing): the alarm state alone then means nothing,
        and only a new ``ALARM:`` line counts as a stop.
        """
        what = what or f"Command {line!r}"
        mark = len(self.link.unsolicited)
        self.link.send(line)
        deadline = time.monotonic() + (timeout_s if timeout_s is not None else self._reply_s)
        while True:
            status = self.status()
            replies = self.link.unsolicited[mark:]
            if "ok" in replies:
                return
            # A stop comes first: once the controller is in alarm it refuses the rest of the
            # pass with error:9, and the operator needs the cause, not that symptom.
            new_alarm = any(reply.upper().startswith("ALARM:") for reply in replies)
            if new_alarm or (status.alarmed and not from_alarm):
                self._stopped(status, what)
            for reply in replies:
                if reply.lower().startswith("error:"):
                    detail = reply[len("error:"):].strip()
                    code = f"error:{detail} ({grbl.describe_error(int(detail))})" if detail.isdigit() else reply
                    raise MachineError(f"{what} was refused: {code}.")
            if status.state not in BUSY_STATES and time.monotonic() >= deadline:
                raise MachineError(f"{what} got no answer from the controller.")
            self._sleep(self._poll_s)

    def _wait_until_idle(self, timeout_s: float | None) -> None:
        deadline = None if timeout_s is None else time.monotonic() + timeout_s
        idle_in_a_row = 0
        while idle_in_a_row < 3:  # a move may not have started at the very first look
            status = self.status()
            if status.alarmed:
                self._stopped(status, "The move")
            idle_in_a_row = idle_in_a_row + 1 if status.state == "Idle" else 0
            if deadline is not None and time.monotonic() >= deadline:
                raise MachineError("The move did not finish in the time allowed.")
            self._sleep(self._poll_s)

    def _stopped(self, status: grbl.Status, what: str) -> None:
        self.needs_homing = True
        alarms = [line for line in self.link.unsolicited if line.upper().startswith("ALARM:")]
        reason = "the stop switch was pressed" if STOP_PIN in status.pins else "the controller raised an alarm"
        if alarms and alarms[-1][6:].strip().isdigit() and STOP_PIN not in status.pins:
            code = int(alarms[-1][6:])
            reason = f"alarm {code}: {grbl.describe_alarm(code)}"
        raise Stopped(f"{what} was stopped: {reason}. Recover and home before continuing.")

    def _sleep(self, seconds: float) -> None:
        if seconds > 0:
            time.sleep(seconds)
