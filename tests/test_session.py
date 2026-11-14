import pytest

from lathe import cli, passes
from lathe.link import GrblLink
from lathe.machine import Machine
from lathe.session import Quit, Session
from lathe.sim import SimulatedGrbl

SETTINGS = passes.CutSettings(
    rpm=60, feed_per_rev_mm=0.1, max_depth_mm=0.10, first_depth_mm=0.05, clearance_mm=1.0,
    plunge_revs=5, spinup_revs=2, y_toward_tube=1, z_cut_direction=1, min_wall_mm=0.4, jog_feed_mm_per_min=120,
)  # fmt: skip

YES, NO = "y", "n"
JOB = [8.0, 10.2, 10.0, 150]  # inner, outer now, target, length
TOUCH = ["y 0.5", "done", YES]  # one move, then "it touches"


class Script:
    """A ui that answers from a list and remembers everything it was told."""

    def __init__(self, answers, on_ask=None):
        self.answers, self.said, self.asked, self.on_ask = list(answers), [], [], on_ask

    def say(self, text):
        self.said.append(text)

    def _next(self, prompt):
        self.asked.append(prompt)
        if self.on_ask:
            self.on_ask(prompt)
        if not self.answers:
            raise Quit
        return self.answers.pop(0)

    def ask(self, prompt):
        return str(self._next(prompt))

    def ask_number(self, prompt):
        return float(self._next(prompt))

    def confirm(self, prompt):
        return self._next(prompt) == YES

    def heard(self, phrase):
        return any(phrase in line for line in self.said)


def session(answers, **board_options):
    board = SimulatedGrbl(move_polls=2, **board_options)
    link = GrblLink(board)
    link.drain(quiet=0.02, max_wait=0.5, first_byte=0.05)
    ui = Script(answers)
    return Session(Machine(link, poll_s=0, reply_s=0.2, reset_wait_s=0), SETTINGS, ui), board, ui


def cutting_moves(board):
    return [line for line in board.executed if line.startswith("G1 ") and " Z" in line]


def test_a_whole_job_from_homing_to_target():
    # 10.2 -> 10.0: a 0.05 mm calibration pass, then the remaining 0.05 mm
    run, board, ui = session([YES, *JOB, YES, *TOUCH, YES, 10.10, YES, 10.00])

    history = run.run()

    assert board.homed
    assert [round(done.depth_mm, 3) for done in history] == [0.05, 0.05]
    assert [done.measured_od for done in history] == [10.10, 10.00]
    assert len(cutting_moves(board)) == 2
    assert ui.heard("Target reached") and ui.heard("wall 1.00 mm")
    assert ui.answers == []  # every scripted answer was asked for


def test_the_touch_moves_use_the_operators_directions_and_the_tool_backs_off():
    run, board, ui = session([YES, *JOB, YES, "y 0.5", "y -0.1", "z 5", "done", YES, NO])

    run.run()

    jogs = [line for line in board.executed if line.startswith("$J=")]
    assert jogs == ["$J=G91 Y0.5000 F120", "$J=G91 Y-0.1000 F120", "$J=G91 Z5.0000 F120", "$J=G91 Y-1.0000 F120"]
    assert cutting_moves(board) == []  # the operator declined the pass


def test_the_second_pass_is_planned_from_the_measurement_not_the_request():
    # the first pass was asked to leave 10.10 but the operator measured 10.14
    run, board, ui = session([YES, *JOB, YES, *TOUCH, YES, 10.14, YES, 10.00])

    history = run.run()

    assert history[1].depth_mm == pytest.approx(0.07)  # (10.14 - 10.00) / 2
    plunge = [line for line in board.executed if line.startswith("G1 ") and " Y" in line][2]
    assert " Y1.1200 " in plunge  # gap 1.05 after the first cut, plus 0.07


def test_values_that_cannot_be_cut_are_explained_and_asked_again():
    run, board, ui = session([YES, 8.0, 10.2, 10.4, 150, *JOB, YES, *TOUCH, NO])

    run.run()

    assert ui.heard("must be smaller than the current outer diameter")
    assert ui.heard("Please enter the values again")


def test_a_pass_that_did_not_touch_the_tube_is_pointed_out():
    run, board, ui = session([YES, *JOB, YES, *TOUCH, YES, 10.20, NO])

    run.run()

    assert ui.heard("did not reach the tube")


def test_a_typing_mistake_in_a_touch_move_is_not_sent_to_the_machine():
    run, board, ui = session([YES, *JOB, YES, "x 5", "y", "y abc", "done", YES, NO])

    run.run()

    assert sum(ui.said.count("Not understood. Examples: y 0.1   y -1   z 5   done") for _ in [0]) == 3
    assert [line for line in board.executed if line.startswith("$J=")] == ["$J=G91 Y-1.0000 F120"]


def test_a_measurement_larger_than_before_has_to_be_confirmed():
    run, board, ui = session([YES, *JOB, YES, *TOUCH, YES, 11.10, NO, 10.10, NO])

    history = run.run()

    assert any("larger than before" in prompt for prompt in ui.asked)
    assert history[0].measured_od == 10.10


def test_the_stop_switch_during_a_pass_leads_back_through_release_homing_and_touch():
    answers = [YES, *JOB, YES, *TOUCH, YES,  # ... the pass starts and is stopped
               YES,          # "is the stop switch released?"
               YES,          # "home now?"
               *TOUCH,       # touch off again
               10.20,        # measured where the tool touched
               NO]           # decline the next pass
    run, board, ui = session(answers)
    real, polls = run.machine.link.status, iter(range(10_000))
    armed = {"on": False}

    def watched(*args, **kwargs):
        if armed["on"] and next(polls) == 4:
            board.press("E")
        return real(*args, **kwargs)

    def on_ask(prompt):
        if prompt == "Start this pass?":
            armed["on"] = True
        if "stop switch released" in prompt:
            board.release("E")

    run.machine.link.status, ui.on_ask = watched, on_ask

    history = run.run()

    assert ui.heard("stop switch was pressed")
    assert ui.heard("may be cut along only part of its length")
    assert ui.said.count("Homing finished.") == 2  # at the start and again after the stop
    assert history == []
    assert not run.machine.needs_homing


def test_declining_to_home_ends_the_session_before_anything_moves():
    run, board, ui = session([NO])

    with pytest.raises(Quit):
        run.run()
    assert board.executed == [] and not board.homed


def test_the_run_command_works_end_to_end_on_the_simulator(monkeypatch, capsys):
    typed = iter(["y", "8", "10,2", "10", "20", "y", "y 0.5", "done", "y", "y", "10.1", "y", "10.0"])
    monkeypatch.setattr("builtins.input", lambda prompt="": next(typed))

    assert cli.main(["run", "--sim"]) == 0

    out = capsys.readouterr().out
    assert "SIMULATION" in out and "Target reached" in out and "Final outer diameter 10.00 mm" in out
