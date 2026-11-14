"""From the operator's numbers to the moves of one cutting pass.

The operator enters the tube's inner diameter, its outer diameter now and the outer
diameter wanted. After every pass the operator measures the outer diameter again, and the
next pass is planned from that measurement, not from what the machine thinks it removed.

The planner never uses absolute machine positions. It keeps one number, the *gap*: how far
the tool is parked from the tube surface. The gap is set by touching the tool to the surface
and backing off. After a pass that cut, the gap is known exactly, because the surface is
where the tool tip went. A pass is four relative moves: plunge, feed, retract, return.

The tube is turned by the X axis, whose unit is one revolution, so every cutting move names
how many turns happen during it. Moves use inverse-time feed (G93), where F is 1 / minutes,
so that turns and millimetres stay in step whatever their ratio.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

# A diameter is treated as reached when less than this much is left to remove from the radius.
DONE_WITHIN_MM = 0.005


@dataclass(frozen=True)
class CutSettings:
    """How a pass is cut. Values come from config/lathe.toml, never from code."""

    rpm: float  # tube speed while cutting
    feed_per_rev_mm: float  # how far the tool travels along the tube per turn
    max_depth_mm: float  # most that one pass may take off the radius
    first_depth_mm: float  # depth of the first, calibration pass
    clearance_mm: float  # how far the tool backs off the surface after the touch
    plunge_revs: float  # turns of the tube while the tool moves in to depth
    spinup_revs: float  # turns before the tool starts to move in
    y_toward_tube: int  # +1 or -1: the Y direction that moves the tool toward the tube
    z_cut_direction: int  # +1 or -1: the Z direction of the cutting stroke
    min_wall_mm: float  # refuse a target that would leave a thinner wall than this
    jog_feed_mm_per_min: float  # speed of the set-up moves the operator asks for

    def __post_init__(self) -> None:
        for name in ("rpm", "feed_per_rev_mm", "max_depth_mm", "first_depth_mm", "clearance_mm", "plunge_revs", "jog_feed_mm_per_min"):
            if getattr(self, name) <= 0:
                raise ValueError(f"{name} must be greater than zero")
        if self.spinup_revs < 0 or self.min_wall_mm < 0:
            raise ValueError("spinup_revs and min_wall_mm must not be negative")
        if self.y_toward_tube not in (1, -1) or self.z_cut_direction not in (1, -1):
            raise ValueError("y_toward_tube and z_cut_direction must be 1 or -1")


@dataclass(frozen=True)
class Job:
    """What the operator entered before the first pass. All in millimetres."""

    inner_d: float
    start_od: float
    target_od: float
    length: float  # length of tube to cut

    def problems(self, settings: CutSettings) -> list[str]:
        """Reasons this job must not be run, in the operator's terms. Empty means it may."""
        found = []
        if min(self.inner_d, self.start_od, self.target_od, self.length) <= 0:
            return ["All four values must be greater than zero."]
        if self.target_od >= self.start_od:
            found.append("The target diameter must be smaller than the current outer diameter.")
        if self.target_od <= self.inner_d:
            found.append("The target diameter must be larger than the inner diameter.")
        else:
            wall = (self.target_od - self.inner_d) / 2
            if wall < settings.min_wall_mm:
                found.append(
                    f"The target leaves a wall of {wall:.2f} mm, thinner than the {settings.min_wall_mm:.2f} mm allowed."
                )
        return found


@dataclass(frozen=True)
class Pass:
    """One planned pass."""

    depth_mm: float  # taken off the radius
    expected_od: float  # diameter the operator should measure afterwards
    gcode: tuple[str, ...]
    minutes: float  # cutting time, not counting acceleration


def next_depth(measured_od: float, target_od: float, settings: CutSettings, *, first: bool) -> float:
    """Depth of the next pass, or 0.0 when the target has been reached.

    The first pass is shallow on purpose: it is the one the calibration rests on. After
    it, what is left is split into equal passes no deeper than the limit, so the last pass
    is never a sliver.
    """
    remaining = (measured_od - target_od) / 2
    if remaining <= DONE_WITHIN_MM:
        return 0.0
    if first:
        return min(settings.first_depth_mm, remaining)
    passes_left = math.ceil(remaining / settings.max_depth_mm - 1e-9)
    return remaining / passes_left


def plan_pass(measured_od: float, depth_mm: float, gap_mm: float, length_mm: float, settings: CutSettings) -> Pass:
    """The moves of one pass, starting and ending with the tool parked ``gap_mm`` off the surface."""
    if depth_mm <= 0 or gap_mm < 0 or length_mm <= 0:
        raise ValueError("depth and length must be greater than zero and the gap must not be negative")

    y = settings.y_toward_tube * (gap_mm + depth_mm)
    z = settings.z_cut_direction * length_mm
    feed_revs = length_mm / settings.feed_per_rev_mm

    lines = ["G91", "G93"]
    if settings.spinup_revs:
        lines.append(_cut(settings.rpm, settings.spinup_revs))
    lines.append(_cut(settings.rpm, settings.plunge_revs, Y=y))
    lines.append(_cut(settings.rpm, feed_revs, Z=z))
    lines.append(_cut(settings.rpm, settings.plunge_revs, Y=-y))
    lines += ["G94", f"G0 Z{_mm(-z)}", "G90"]

    turns = settings.spinup_revs + 2 * settings.plunge_revs + feed_revs
    return Pass(
        depth_mm=depth_mm,
        expected_od=measured_od - 2 * depth_mm,
        gcode=tuple(lines),
        minutes=turns / settings.rpm,
    )


def gap_after_pass(gap_before_mm: float, depth_mm: float) -> float:
    """How far the parked tool is from the surface once a pass has cut.

    A pass that removed material leaves the surface exactly where the tool tip went, so the
    gap is now the distance the tool plunged, whatever the gap was believed to be before.
    That is what makes the first pass a calibration: an error in the touch-off is gone after
    it. How much came off is not needed here; the measured diameter decides the next depth.
    """
    return gap_before_mm + depth_mm


def pass_removed_material(od_before: float, od_measured: float) -> bool:
    """False when the measurement shows the tool never reached the tube.

    The gap is then still unknown, only known to be at least the plunge, and the operator
    should be told, because the next pass will again cut less than asked.
    """
    return od_before - od_measured > 2 * DONE_WITHIN_MM


def _cut(rpm: float, revs: float, **axes: float) -> str:
    words = "".join(f" {axis}{_mm(value)}" for axis, value in axes.items())
    return f"G1 X{revs:.3f}{words} F{rpm / revs:.4f}"


def _mm(value: float) -> str:
    return f"{value:.4f}"
