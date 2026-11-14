"""Reading the cutting parameters from config/lathe.toml."""

from __future__ import annotations

import tomllib
from pathlib import Path

from .passes import CutSettings

DEFAULT_PATH = Path("config/lathe.toml")


def load_cut_settings(path: Path = DEFAULT_PATH) -> CutSettings:
    """Load and check the file. A missing or misspelt entry is an error, not a default."""
    with open(path, "rb") as handle:
        data = tomllib.load(handle)
    try:
        return CutSettings(**data["cut"], **data["axes"])
    except KeyError as missing:
        raise ValueError(f"{path}: section {missing} is missing") from None
    except TypeError as problem:
        raise ValueError(f"{path}: {problem}") from None
