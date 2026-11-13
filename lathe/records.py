"""Test records: plain-text captures of what the controller reported.

Records are committed to the repository as test evidence, so network details
the controller prints (Wi-Fi names, addresses, passwords) are masked first.
The masking is best effort: read a record before committing it.
"""

from __future__ import annotations

import re
from datetime import datetime
from pathlib import Path

# A whole setting line such as "$Sta/SSID=..." or "$AP/Password=...".
_NETWORK_SETTING = re.compile(r"(?i)^(\$[\w/]*(?:ssid|password|passphrase)[\w/]*=).*$")

_INLINE = (
    (re.compile(r"\b(?:[0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}\b"), "<mac>"),
    (re.compile(r"\b\d{1,3}(?:\.\d{1,3}){3}\b"), "<ip>"),
    (re.compile(r"(?i)\b(ssid\s*[=:]\s*)[^:\]\s]+"), r"\1<ssid>"),
    (re.compile(r"(?i)\b(pass(?:word|phrase)?\s*[=:]\s*)[^:\]\s]+"), r"\1<password>"),
    (re.compile(r"(?i)\b(connecting(?: to)? |connected with |connected to )[^\]]+"), r"\1<ssid>"),
    (re.compile(r"(?i)\b(access point )[^\]]+?( started)"), r"\1<ssid>\2"),
)


def redact_line(line: str) -> str:
    setting = _NETWORK_SETTING.match(line)
    if setting:
        return setting.group(1) + "<redacted>"
    for pattern, replacement in _INLINE:
        line = pattern.sub(replacement, line)
    return line


def render(title: str, header: dict[str, str], sections: list[tuple[str, list[str]]], *, redact: bool = True) -> str:
    """Lay a capture out as a small, diff-friendly text document."""
    out = [f"# {title}"]
    out += [f"# {key}: {value}" for key, value in header.items()]
    out.append("")
    for name, lines in sections:
        out.append(f"== {name} ==")
        out += [redact_line(line) if redact else line for line in lines]
        out.append("")
    return "\n".join(out)


def save(directory: Path, name: str, text: str, when: datetime) -> Path:
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / f"{when:%Y%m%d-%H%M%S}_{name}.txt"
    path.write_text(text, encoding="utf-8", newline="\n")
    return path
