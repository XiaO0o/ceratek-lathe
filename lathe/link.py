"""One conversation with the controller over a serial-like port."""

from __future__ import annotations

import time
from collections import deque
from dataclasses import dataclass, field

from . import grbl

BAUD = 115200  # fixed by both firmwares on the MKS DLC32

_IDLE_SLEEP = 0.002  # pause when the port had nothing, so idle loops stay cheap
_MAX_LINE = 2048  # longer than any Grbl line; guards against boot noise without a newline


class GrblTimeout(TimeoutError):
    """The controller did not answer in time."""

    def __init__(self, message: str, lines: list[str] | None = None) -> None:
        super().__init__(message)
        self.lines = lines or []


@dataclass
class Reply:
    """Everything the controller sent in answer to one command line."""

    command: str
    lines: list[str] = field(default_factory=list)
    error: str | None = None  # text after "error:"; None when the controller said "ok"

    @property
    def ok(self) -> bool:
        return self.error is None

    @property
    def error_code(self) -> int | None:
        return int(self.error) if self.error is not None and self.error.isdigit() else None

    def describe_error(self) -> str:
        if self.error is None:
            return ""
        code = self.error_code
        return f"error:{code} ({grbl.describe_error(code)})" if code is not None else f"error: {self.error}"


class GrblLink:
    """Line-oriented conversation with a Grbl-family controller.

    ``port`` is anything with pyserial's read / write / in_waiting / close
    surface, so the same code drives the real board and ``lathe.sim``.
    """

    def __init__(self, port) -> None:
        self.port = port
        # Everything sent (">") and received ("<"), in order: the raw material for test records.
        self.transcript: list[tuple[str, str]] = []
        # Lines that arrived without being the answer to anything (boot text, alarms, messages).
        self.unsolicited: list[str] = []
        self._buffer = bytearray()
        self._pending: deque[str] = deque()

    @classmethod
    def open_serial(cls, device: str, baud: int = BAUD) -> "GrblLink":
        import serial  # deferred: the simulator and the tests do not need pyserial

        return cls(serial.Serial(device, baud, timeout=0.05, write_timeout=2.0))

    def close(self) -> None:
        self.port.close()

    # -- sending ---------------------------------------------------------

    def realtime(self, byte: bytes) -> None:
        """Send a real-time command byte (see ``lathe.grbl.RT_*``)."""
        self.port.write(byte)
        self.transcript.append((">", _printable(byte)))

    def send(self, text: str) -> None:
        """Send one line without waiting for its answer.

        For commands that answer only when they finish (homing) or may not answer at all
        (anything sent while the stop input has the controller locked). The caller watches
        the status report instead; whatever does come back ends up in ``unsolicited``.
        """
        self._set_aside_unsolicited()
        self.port.write(text.encode("ascii") + b"\n")
        self.transcript.append((">", text))

    def command(self, text: str, timeout: float = 3.0) -> Reply:
        """Send one line and collect the reply up to its ``ok`` or ``error:``."""
        self._set_aside_unsolicited()
        self.port.write(text.encode("ascii") + b"\n")
        self.transcript.append((">", text))

        deadline = time.monotonic() + timeout
        lines: list[str] = []
        while (line := self._next_line(deadline)) is not None:
            if line == "ok":
                return Reply(text, lines)
            if line.lower().startswith("error:"):
                return Reply(text, lines, error=line[len("error:"):].strip())
            lines.append(line)
        raise GrblTimeout(f"no 'ok' or 'error' within {timeout:g} s after {text!r}", lines)

    def status(self, timeout: float = 1.0) -> grbl.Status:
        """Ask for a real-time status report and parse it."""
        self.realtime(grbl.RT_STATUS)
        deadline = time.monotonic() + timeout
        while (line := self._next_line(deadline)) is not None:
            if grbl.is_status_report(line):
                return grbl.parse_status(line)
            self.unsolicited.append(line)
        raise GrblTimeout(f"no status report within {timeout:g} s")

    # -- receiving -------------------------------------------------------

    def drain(self, quiet: float = 1.5, max_wait: float = 10.0, first_byte: float = 3.0) -> list[str]:
        """Collect what the controller sends on its own, until it falls silent.

        Opening the USB port resets the ESP32, so this is how the boot banner
        is captured. Gives up after ``first_byte`` seconds if nothing arrives
        at all (the board did not reset), and never waits longer than ``max_wait``.
        """
        lines: list[str] = []
        start = time.monotonic()
        last_data: float | None = None
        while True:
            now = time.monotonic()
            if now - start >= max_wait:
                break
            if last_data is None and now - start >= first_byte:
                break
            if last_data is not None and now - last_data >= quiet:
                break
            if self._pump():
                last_data = time.monotonic()
            while self._pending:
                lines.append(self._pending.popleft())
        self._flush_partial_line()
        while self._pending:
            lines.append(self._pending.popleft())
        return lines

    def _next_line(self, deadline: float) -> str | None:
        while True:
            if self._pending:
                return self._pending.popleft()
            if time.monotonic() >= deadline:
                return None
            self._pump()

    def _set_aside_unsolicited(self) -> None:
        # A late answer to an earlier, timed-out command must not be mistaken
        # for the answer to the next one.
        while getattr(self.port, "in_waiting", 0):
            self._pump()
        self.unsolicited.extend(self._pending)
        self._pending.clear()

    def _pump(self) -> int:
        """Move bytes from the port into complete lines. Returns the byte count."""
        waiting = getattr(self.port, "in_waiting", 0)
        chunk = self.port.read(waiting or 1)
        if not chunk:
            time.sleep(_IDLE_SLEEP)
            return 0
        self._buffer += chunk
        while (cut := self._buffer.find(b"\n")) >= 0:
            self._accept(bytes(self._buffer[:cut]))
            del self._buffer[: cut + 1]
        if len(self._buffer) > _MAX_LINE:
            self._flush_partial_line()
        return len(chunk)

    def _flush_partial_line(self) -> None:
        if self._buffer:
            self._accept(bytes(self._buffer))
            self._buffer.clear()

    def _accept(self, raw: bytes) -> None:
        text = raw.decode("ascii", errors="replace").strip()
        text = "".join(ch if ch.isprintable() else "�" for ch in text)
        if text:
            self.transcript.append(("<", text))
            self._pending.append(text)


def _printable(data: bytes) -> str:
    return "".join(chr(b) if 32 <= b < 127 else f"<0x{b:02X}>" for b in data)
