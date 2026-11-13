"""Finding the controller's USB serial port on macOS, Windows or Linux."""

from __future__ import annotations

from dataclasses import dataclass

# USB vendor ids of the common USB-serial bridge chips. The MKS DLC32 uses a CH340.
USB_SERIAL_CHIPS = {
    0x1A86: "WCH CH340/CH9102",
    0x10C4: "Silicon Labs CP210x",
    0x0403: "FTDI",
}

_NOT_A_CONTROLLER = ("bluetooth", "debug-console")


class PortError(RuntimeError):
    """No usable port, or no way to choose between several."""


@dataclass(frozen=True)
class Candidate:
    device: str
    description: str = ""
    usb_id: str = ""  # "1a86:7523", empty when the port is not a USB device
    chip: str | None = None  # known USB-serial bridge family, else None

    @property
    def label(self) -> str:
        details = ", ".join(part for part in (self.chip, f"USB {self.usb_id}" if self.usb_id else "") if part)
        return f"{self.device} ({details})" if details else self.device


def scan(comports=None) -> list[Candidate]:
    """List serial ports that could be the controller, likeliest first."""
    if comports is None:
        from serial.tools import list_ports  # deferred: tests inject their own list

        comports = list_ports.comports()

    found = []
    for port in comports:
        if any(word in port.device.lower() for word in _NOT_A_CONTROLLER):
            continue
        vid, pid = getattr(port, "vid", None), getattr(port, "pid", None)
        found.append(
            Candidate(
                device=port.device,
                description=getattr(port, "description", "") or "",
                usb_id=f"{vid:04x}:{pid:04x}" if vid is not None and pid is not None else "",
                chip=USB_SERIAL_CHIPS.get(vid),
            )
        )
    return sorted(found, key=lambda c: (c.chip is None, c.device))


def choose(explicit: str | None = None, comports=None) -> Candidate:
    """Pick the controller's port, or explain why that is not possible."""
    if explicit:
        return Candidate(device=explicit)

    candidates = scan(comports)
    likely = [c for c in candidates if c.chip]
    if len(likely) == 1:
        return likely[0]
    if not likely:
        message = (
            "No USB-serial adapter found. Check that the board is plugged in with a data-capable "
            "USB cable (and, on a USB-C only computer, through an adapter that carries data)."
        )
        if candidates:
            others = "".join(f"\n  {c.label}" for c in candidates)
            message += f"\nOther serial ports seen:{others}\nIf one of these is the board, pass it with --port."
        raise PortError(message)
    listing = "".join(f"\n  {c.label}" for c in likely)
    raise PortError(f"More than one USB-serial adapter found; pick one with --port:{listing}")
