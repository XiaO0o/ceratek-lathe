"""An in-memory stand-in for the controller.

It answers like a Grbl 1.1 board for the parts of the protocol the tools use,
so the software can be developed, demonstrated and tested with no hardware
attached. It exposes the same read / write surface as ``serial.Serial``.

Motion is simulated only as far as the control software can tell from outside: a move keeps
the controller busy for a set number of status polls, and nothing is known about positions.
The stop input behaves as FluidNC v4.1.1 was seen to on the bench (docs/bench-tests.md).
"""

from __future__ import annotations

DEFAULT_SETTINGS = {
    0: "10",
    1: "25",
    3: "1",
    5: "1",
    20: "0",
    21: "0",
    22: "0",
    100: "80.000",
    101: "80.000",
    102: "80.000",
    110: "6000.000",
    111: "6000.000",
    112: "6000.000",
    120: "500.000",
    121: "500.000",
    122: "500.000",
}

_BANNER = "Grbl 1.3a ['$' for help]"
# Shaped like the network line a real board prints, so record redaction is exercised.
_NETWORK_LINE = "[MSG:Mode=AP:SSID=SIM_NET:IP=192.168.4.1:MAC=AA-BB-CC-DD-EE-FF]"


class SimulatedGrbl:
    def __init__(
        self,
        *,
        pins: str = "",
        settings: dict[int, str] | None = None,
        lock_on_reset: bool = False,
        move_polls: int = 0,
    ) -> None:
        self.timeout = 0
        self.is_open = True
        self.state = "Idle"
        self.pins = set(pins)
        self.settings = dict(DEFAULT_SETTINGS if settings is None else settings)
        self.received: list[str] = []  # every command line the "board" was sent
        self.silent = False  # True: swallow everything, to exercise timeouts
        self.executed: list[str] = []  # motion lines the "board" accepted
        self.homed = False
        self.homing_fails = False  # True: the next homing cycle never finds its switch
        self._lock_on_reset = lock_on_reset
        self._move_polls = move_polls  # status polls each move keeps the board busy
        self._busy = 0
        self._critical = False  # stopped by the stop input: nothing is answered until a reset
        self._rx = bytearray()
        self._tx = bytearray()
        self._boot()

    # -- test controls: what a person at the bench would do --------------

    def press(self, pin: str) -> None:
        self.pins.add(pin)
        if pin == "E" and not self._critical:
            self._critical = True
            self._busy = 0
            self.homed = False
            self.state = "Alarm"
            self._say("[MSG:INFO: Stopped by estop_pin]", "[MSG:INFO: ALARM: Hard Stop]", "ALARM:13")

    def release(self, pin: str) -> None:
        self.pins.discard(pin)

    def alarm(self, code: int) -> None:
        self.state = "Alarm"
        self._say(f"ALARM:{code}")

    # -- the serial.Serial surface the link uses -------------------------

    @property
    def in_waiting(self) -> int:
        return len(self._tx)

    def read(self, size: int = 1) -> bytes:
        chunk = bytes(self._tx[:size])
        del self._tx[:size]
        return chunk

    def write(self, data: bytes) -> int:
        for byte in data:
            if self.silent:
                continue
            if byte == ord("?"):
                self._tick()
                self._say(self._status())
            elif byte == 0x18:
                self._rx.clear()
                self._busy = 0
                self._critical = "E" in self.pins  # a held stop switch survives the reset
                self._boot()
            elif byte == ord("!"):
                if self.state in ("Run", "Jog"):
                    self.state = "Hold"
            elif byte == ord("~"):
                if self.state == "Hold":
                    self.state = "Run" if self._busy else "Idle"
            elif byte == ord("\n"):
                line = self._rx.decode("ascii", errors="replace").strip()
                self._rx.clear()
                self._execute(line)
            elif byte != ord("\r"):
                self._rx.append(byte)
        return len(data)

    def flush(self) -> None:
        pass

    def reset_input_buffer(self) -> None:
        self._tx.clear()

    def close(self) -> None:
        self.is_open = False

    # -- behaviour -------------------------------------------------------

    def _boot(self) -> None:
        self.state = "Alarm" if self._lock_on_reset or self._critical else "Idle"
        self._say("")
        self._say(_BANNER)
        self._say(_NETWORK_LINE)
        if self._lock_on_reset:
            self._say("[MSG:'$H'|'$X' to unlock]")

    def _execute(self, line: str) -> None:
        self.received.append(line)
        upper = line.upper()
        if self._critical:
            if upper == "$X":
                self._say("[MSG:ERR: Reset to continue]")
            return  # FluidNC answers neither ok nor error in this alarm
        if upper == "":
            self._say("ok")
        elif upper == "$I":
            self._say("[VER:1.3a.SIM:]", "[OPT:PHS]", "[MSG:Using machine:Simulator]", "ok")
        elif upper == "$$":
            self._say(*(f"${number}={value}" for number, value in sorted(self.settings.items())), "ok")
        elif upper == "$G":
            self._say("[GC:G0 G54 G17 G21 G90 G94 M5 M9 T0 F0 S0]", "ok")
        elif upper == "$#":
            self._say("[G54:0.000,0.000,0.000]", "[G92:0.000,0.000,0.000]", "[PRB:0.000,0.000,0.000:0]", "ok")
        elif upper == "$N":
            self._say("$N0=", "$N1=", "ok")
        elif upper == "$X":
            if self.state == "Alarm":
                self.state = "Idle"
                self._say("[MSG:Caution: Unlocked]")
            self._say("ok")
        elif upper in ("$H", "$HX", "$HY", "$HZ"):
            self.state, self._busy = "Home", max(self._move_polls, 1)
        elif upper.startswith("$J="):
            if self.state == "Alarm":
                self._say("error:9")
            else:
                self.executed.append(line)
                self._start("Jog")
                self._say("ok")
        elif upper.startswith("$"):
            self._say("error:3")
        elif self.state == "Alarm":
            self._say("error:9")
        else:
            if upper.startswith(("G0 ", "G1 ")):
                self.executed.append(line)
                self._start("Run")
            self._say("ok")

    def _start(self, state: str) -> None:
        if self._move_polls:
            self.state = state
            self._busy += self._move_polls

    def _tick(self) -> None:
        """One status poll's worth of time passes."""
        if not self._busy or self.state == "Hold":
            return
        self._busy -= 1
        if self._busy:
            return
        if self.state != "Home":
            self.state = "Idle"
        elif self.homing_fails:
            self.state = "Alarm"
            self._say("[MSG:INFO: ALARM: Homing Fail Approach]", "ALARM:9")
        else:
            self.state, self.homed = "Idle", True
            self._say("[MSG:Homed]", "ok")

    def _status(self) -> str:
        report = f"<{self.state}|MPos:0.000,0.000,0.000|FS:0,0"
        if self.pins:
            report += "|Pn:" + "".join(sorted(self.pins))
        return report + ">"

    def _say(self, *lines: str) -> None:
        for line in lines:
            self._tx += line.encode("ascii") + b"\r\n"
