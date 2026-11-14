import pytest

from lathe.link import GrblLink
from lathe.machine import Machine, MachineError, Stopped
from lathe.sim import SimulatedGrbl

PASS = ["G91", "G93", "G1 X2.000 F30.0000", "G1 X5.000 Y1.0500 F12.0000", "G94", "G0 Z-150.0000", "G90"]


def machine(**board_options):
    board = SimulatedGrbl(move_polls=3, **board_options)
    link = GrblLink(board)
    link.drain(quiet=0.02, max_wait=0.5, first_byte=0.05)
    return Machine(link, poll_s=0, reply_s=0.2, reset_wait_s=0), board


def homed(**board_options):
    lathe, board = machine(**board_options)
    lathe.home()
    return lathe, board


class PressDuring:
    """Presses a switch on the simulated board after a number of status polls."""

    def __init__(self, lathe, board, pin, after_polls, release_after=None):
        self.count, real = 0, lathe.link.status

        def watched(*args, **kwargs):
            self.count += 1
            if self.count == after_polls:
                board.press(pin)
            if release_after and self.count == release_after:
                board.release(pin)
            return real(*args, **kwargs)

        lathe.link.status = watched


# -- homing ------------------------------------------------------------------


def test_a_new_session_must_home_before_it_may_cut():
    lathe, board = machine()

    with pytest.raises(MachineError, match="not been homed"):
        lathe.run(PASS)
    assert board.executed == []


def test_homing_from_the_start_up_alarm_clears_the_need_to_home():
    lathe, board = machine(lock_on_reset=True)
    lathe.link.realtime(b"\x18")  # the board boots locked, as FluidNC does with must_home
    lathe.link.drain(quiet=0.02, max_wait=0.5, first_byte=0.05)

    lathe.home()

    assert board.homed and not lathe.needs_homing
    assert lathe.status().state == "Idle"


def test_a_homing_cycle_that_never_finds_its_switch_is_reported_and_leaves_the_machine_unhomed():
    lathe, board = machine()
    board.homing_fails = True

    with pytest.raises(Stopped, match="alarm 9"):
        lathe.home()
    assert lathe.needs_homing


# -- running a pass ------------------------------------------------------------


def test_a_pass_is_sent_in_order_and_waited_out():
    lathe, board = homed()

    lathe.run(PASS)

    assert board.executed == [line for line in PASS if line.startswith(("G0 ", "G1 "))]
    assert lathe.status().state == "Idle"


def test_a_line_the_controller_refuses_stops_the_pass_with_the_reason():
    lathe, board = homed()

    with pytest.raises(MachineError, match=r"error:3"):
        lathe.run(["G91", "$BOGUS", "G1 X1.000 F60.0000"])
    assert board.executed == []


def test_jogging_is_allowed_before_homing_for_setting_up():
    lathe, board = machine()

    lathe.jog("Y", 1.0, 60)

    assert board.executed == ["$J=G91 Y1.0000 F60"]


# -- the stop switch -----------------------------------------------------------


def test_the_stop_switch_during_a_pass_ends_it_and_demands_homing():
    lathe, board = homed()
    PressDuring(lathe, board, "E", after_polls=6)

    with pytest.raises(Stopped, match="stop switch was pressed"):
        lathe.run(PASS)
    assert lathe.needs_homing


def test_nothing_can_be_done_while_the_stop_switch_is_held():
    lathe, board = homed()
    board.press("E")
    sent_before = len(board.received)

    for attempt in (lambda: lathe.run(PASS), lathe.home, lathe.recover, lambda: lathe.jog("Y", 1, 60)):
        with pytest.raises(MachineError, match="[Ss]top switch"):
            attempt()
    assert board.received[sent_before:] == []  # not even tried on the controller


def test_releasing_the_stop_switch_alone_does_not_bring_the_machine_back():
    lathe, board = homed()
    board.press("E")
    board.release("E")

    with pytest.raises(MachineError):
        lathe.run(PASS)
    assert board.executed == []


def test_after_a_stop_recover_then_home_is_the_only_way_back_to_cutting():
    lathe, board = homed()
    board.press("E")
    board.release("E")

    lathe.recover()
    with pytest.raises(MachineError, match="not been homed"):
        lathe.run(PASS)  # recovered, but the old position is not trusted
    lathe.home()
    lathe.run(PASS)

    assert board.executed[-1] == "G0 Z-150.0000"


def test_a_stop_during_homing_is_reported_as_a_stop():
    lathe, board = machine()
    PressDuring(lathe, board, "E", after_polls=3)

    with pytest.raises(Stopped):
        lathe.home()
    assert lathe.needs_homing


# -- other alarms and pauses ----------------------------------------------------


def test_a_limit_switch_alarm_during_a_pass_is_named_and_demands_homing():
    lathe, board = homed()
    real = lathe.link.status
    polls = iter(range(100))

    def watched(*args, **kwargs):
        if next(polls) == 5:
            board.alarm(1)
        return real(*args, **kwargs)

    lathe.link.status = watched

    with pytest.raises(Stopped, match="alarm 1: hard limit"):
        lathe.run(PASS)
    assert lathe.needs_homing


def test_a_pause_keeps_the_pass_alive_and_resume_finishes_it():
    lathe, board = homed()
    real = lathe.link.status
    polls = iter(range(1000))

    def watched(*args, **kwargs):
        n = next(polls)
        if n == 5:
            lathe.hold()
        if n == 40:
            lathe.resume()
        return real(*args, **kwargs)

    lathe.link.status = watched

    lathe.run(PASS)

    assert not lathe.needs_homing  # a pause is not a stop: position is kept
    assert lathe.status().state == "Idle"


def test_a_controller_that_stops_answering_is_reported_not_waited_on_forever():
    lathe, board = homed()
    real_execute = board._execute
    board._execute = lambda line: None if line.startswith("G1") else real_execute(line)

    with pytest.raises(MachineError, match="no answer"):
        lathe.run(PASS)
