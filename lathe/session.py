"""The operator's guided sequence for refining one tube.

    make the machine ready (release the stop switch, home)
    enter the four dimensions
    touch the tool to the tube
    repeat: show the next pass -> cut -> measure and enter the diameter
    summary

The operator types numbers and answers yes or no; no machine codes. Everything the operator
sees and types goes through a small ``ui`` object, so the same sequence runs in a terminal
and in the automated tests.
"""

from __future__ import annotations

from dataclasses import dataclass, field

from . import passes
from .machine import Machine, MachineError, Stopped


class Quit(Exception):
    """The operator chose to end the session."""


@dataclass
class PassRecord:
    number: int
    depth_mm: float
    od_before: float
    expected_od: float
    measured_od: float

    @property
    def removed_mm(self) -> float:
        """What really came off the diameter."""
        return self.od_before - self.measured_od


@dataclass
class Session:
    machine: Machine
    settings: passes.CutSettings
    ui: object
    history: list[PassRecord] = field(default_factory=list)

    def run(self) -> list[PassRecord]:
        ui = self.ui
        self._make_ready()
        job = self._ask_job()
        od, gap, first = job.start_od, self._touch_off(), True

        while True:
            depth = passes.next_depth(od, job.target_od, self.settings, first=first)
            if depth == 0:
                ui.say(f"Target reached: the outer diameter is {od:.2f} mm (target {job.target_od:.2f} mm).")
                break
            planned = passes.plan_pass(od, depth, gap, job.length, self.settings)
            ui.say(
                f"Pass {len(self.history) + 1}: takes {2 * depth:.3f} mm off the diameter. "
                f"Expected afterwards: {planned.expected_od:.2f} mm. About {planned.minutes:.1f} min."
            )
            if not ui.confirm("Start this pass?"):
                ui.say("No further pass was started.")
                break

            try:
                self.machine.run(planned.gcode, timeout_s=planned.minutes * 60 + 120)
            except Stopped as stop:
                ui.say(str(stop))
                ui.say("The pass was not completed, so the tube may be cut along only part of its length.")
                self._make_ready()
                gap, first = self._touch_off(), True
                od = self._ask_measured(od, "Measure the outer diameter where the tool touched and enter it")
                continue

            ui.say("Pass finished. The tube is not turning and the tool is clear.")
            measured = self._ask_measured(od, "Measure the outer diameter and enter it")
            if not passes.pass_removed_material(od, measured):
                ui.say("The diameter did not change: the tool did not reach the tube. The next pass goes deeper.")
            self.history.append(PassRecord(len(self.history) + 1, depth, od, planned.expected_od, measured))
            gap, od, first = passes.gap_after_pass(gap, depth), measured, False

        self._summary(job)
        return self.history

    # -- steps -----------------------------------------------------------

    def _make_ready(self) -> None:
        """Get from any state to homed and idle, with the operator's consent to every move."""
        ui = self.ui
        while True:
            try:
                if self.machine.stop_switch_pressed():
                    ui.say("The stop switch is pressed. Release it.")
                    if not ui.confirm("Is the stop switch released?"):
                        raise Quit
                    continue
                status = self.machine.status()
                if not status.alarmed and not self.machine.needs_homing:
                    return
                ui.say("The machine has to find its reference position. The tool moves back first, then along the tube.")
                if not ui.confirm("Is everything clear of the tool? Home now?"):
                    raise Quit
                if status.alarmed:
                    self.machine.recover()
                self.machine.home()
                ui.say("Homing finished.")
                return
            except MachineError as problem:
                ui.say(str(problem))
                if not ui.confirm("Try again?"):
                    raise Quit from None

    def _ask_job(self) -> passes.Job:
        ui = self.ui
        while True:
            job = passes.Job(
                inner_d=ui.ask_number("Inner diameter of the tube, mm"),
                start_od=ui.ask_number("Outer diameter now, mm"),
                target_od=ui.ask_number("Target outer diameter, mm"),
                length=ui.ask_number("Length to cut, mm"),
            )
            problems = job.problems(self.settings)
            if not problems:
                wall = (job.target_od - job.inner_d) / 2
                ui.say(f"This removes {job.start_od - job.target_od:.2f} mm from the diameter and leaves a {wall:.2f} mm wall.")
                if ui.confirm("Are these values right?"):
                    return job
                continue
            for problem in problems:
                ui.say(problem)
            ui.say("Please enter the values again.")

    def _touch_off(self) -> float:
        """Let the operator bring the tool to the tube surface. Returns the gap afterwards."""
        ui, cfg = self.ui, self.settings
        ui.say(
            "Bring the tool tip to the start of the cut, just touching the tube. "
            "Type 'y 0.5' to move 0.5 mm toward the tube, 'y -0.5' away from it, "
            "'z 5' along the tube, 'z -5' back. Use small steps near the tube. Type 'done' when it touches."
        )
        directions = {"y": cfg.y_toward_tube, "z": cfg.z_cut_direction}
        while True:
            words = ui.ask("Move").lower().split()
            if words == ["done"]:
                if ui.confirm("Is the tool tip just touching the tube surface?"):
                    break
                continue
            try:
                axis, distance = words[0], float(words[1])
                sign = directions[axis]
            except (IndexError, ValueError, KeyError):
                ui.say("Not understood. Examples: y 0.1   y -1   z 5   done")
                continue
            try:
                self.machine.jog(axis.upper(), sign * distance, cfg.jog_feed_mm_per_min)
            except Stopped as stop:
                ui.say(str(stop))
                self._make_ready()
                ui.say("Start the touch again from here.")
            except MachineError as problem:
                ui.say(f"That move was not made. {problem}")
        self.machine.jog("Y", -cfg.y_toward_tube * cfg.clearance_mm, cfg.jog_feed_mm_per_min)
        ui.say(f"The tool has backed off {cfg.clearance_mm:g} mm.")
        return cfg.clearance_mm

    def _ask_measured(self, od_before: float, prompt: str) -> float:
        ui = self.ui
        while True:
            measured = ui.ask_number(f"{prompt}, mm")
            if measured <= 0:
                ui.say("The diameter must be greater than zero.")
            elif measured > od_before + 0.05 and not ui.confirm(
                f"{measured:.2f} mm is larger than before the pass ({od_before:.2f} mm). Is that right?"
            ):
                continue
            else:
                return measured

    def _summary(self, job: passes.Job) -> None:
        ui = self.ui
        if not self.history:
            ui.say("No pass was completed.")
            return
        ui.say("Pass  asked   removed  measured   (mm on the diameter)")
        for done in self.history:
            ui.say(f"{done.number:>4}  {2 * done.depth_mm:>5.3f}  {done.removed_mm:>7.3f}  {done.measured_od:>8.2f}")
        final = self.history[-1].measured_od
        ui.say(f"Final outer diameter {final:.2f} mm; target {job.target_od:.2f} mm; wall {(final - job.inner_d) / 2:.2f} mm.")


class Console:
    """The terminal version of the ``ui`` object. Typing 'q' at any question ends the session."""

    def say(self, text: str) -> None:
        print(text, flush=True)

    def ask(self, prompt: str) -> str:
        answer = input(f"{prompt}: ").strip()
        if answer.lower() == "q":
            raise Quit
        return answer

    def ask_number(self, prompt: str) -> float:
        while True:
            answer = self.ask(prompt).replace(",", ".")
            try:
                return float(answer)
            except ValueError:
                print("Please type a number, for example 10.25")

    def confirm(self, prompt: str) -> bool:
        while True:
            answer = self.ask(f"{prompt} (y/n)").lower()
            if answer in ("y", "yes"):
                return True
            if answer in ("n", "no"):
                return False
