import pytest

from lathe import grbl
from lathe.link import GrblLink, GrblTimeout
from lathe.sim import SimulatedGrbl

FAST = dict(quiet=0.02, max_wait=0.5, first_byte=0.05)


def booted(**sim_options):
    """A link to a simulated board whose boot text has already been read."""
    board = SimulatedGrbl(**sim_options)
    link = GrblLink(board)
    link.drain(**FAST)
    return link, board


def test_boot_text_is_collected_after_opening_the_port():
    link = GrblLink(SimulatedGrbl())

    boot = link.drain(**FAST)

    assert boot[0].startswith("Grbl ")
    assert any("SSID" in line for line in boot)


def test_drain_gives_up_quickly_when_the_board_stays_silent():
    board = SimulatedGrbl()
    board.reset_input_buffer()  # as if the board did not reset when the port opened

    assert GrblLink(board).drain(**FAST) == []


def test_command_returns_the_lines_before_ok():
    link, board = booted()

    reply = link.command("$I", timeout=0.5)

    assert reply.ok and reply.error_code is None
    assert reply.lines[0].startswith("[VER:")
    assert board.received == ["$I"]


def test_settings_dump_is_one_line_per_setting():
    link, board = booted(settings={0: "10", 100: "400.000"})

    assert link.command("$$", timeout=0.5).lines == ["$0=10", "$100=400.000"]


def test_error_reply_carries_the_code_and_a_description():
    link, _ = booted()

    reply = link.command("$NOPE", timeout=0.5)

    assert not reply.ok
    assert reply.error_code == 3
    assert reply.describe_error() == f"error:3 ({grbl.ERRORS[3]})"


def test_verbose_error_text_is_kept_when_the_firmware_sends_words_instead_of_a_code():
    class Wordy(SimulatedGrbl):
        def _execute(self, line):
            self._say("error: Bad number format")

    link = GrblLink(Wordy())
    link.drain(**FAST)

    reply = link.command("G1 Xabc", timeout=0.5)

    assert reply.error == "Bad number format"
    assert reply.error_code is None
    assert reply.describe_error() == "error: Bad number format"


def test_status_shows_pressed_inputs_and_follows_release():
    link, board = booted()

    assert link.status(timeout=0.5).pins == frozenset()
    board.press("Y")
    board.press("P")
    assert link.status(timeout=0.5).pins == frozenset("YP")
    board.release("Y")
    assert link.status(timeout=0.5).pins == frozenset("P")


def test_gcode_is_locked_out_while_in_alarm_until_unlock():
    link, board = booted()
    board.alarm(1)

    assert link.command("G0 X1", timeout=0.5).error_code == 9
    assert link.status(timeout=0.5).alarmed
    assert link.command("$X", timeout=0.5).ok
    assert link.command("G0 X1", timeout=0.5).ok
    assert "ALARM:1" in link.unsolicited


def test_soft_reset_reboots_into_alarm_when_the_firmware_requires_homing():
    link, board = booted(lock_on_reset=True)

    link.realtime(grbl.RT_SOFT_RESET)
    boot = link.drain(**FAST)

    assert boot[0].startswith("Grbl ")
    assert link.status(timeout=0.5).alarmed


def test_silent_board_raises_a_timeout_instead_of_hanging():
    link, board = booted()
    board.silent = True

    with pytest.raises(GrblTimeout):
        link.command("$I", timeout=0.05)
    with pytest.raises(GrblTimeout):
        link.status(timeout=0.05)


def test_late_answer_to_a_timed_out_command_is_not_taken_for_the_next_reply():
    link, board = booted()
    board.silent = True
    with pytest.raises(GrblTimeout):
        link.command("$$", timeout=0.05)
    board.silent = False
    board._say("$0=10", "ok")  # the old answer finally arrives

    reply = link.command("$I", timeout=0.5)

    assert reply.lines[0].startswith("[VER:")
    assert "$0=10" in link.unsolicited


def test_transcript_records_both_directions_in_order():
    link, _ = booted()
    start = len(link.transcript)

    link.command("$G", timeout=0.5)
    link.status(timeout=0.5)

    directions = [direction for direction, _ in link.transcript[start:]]
    texts = [text for _, text in link.transcript[start:]]
    assert directions == [">", "<", "<", ">", "<"]
    assert texts[0] == "$G" and texts[2] == "ok" and texts[3] == "?"


def test_boot_noise_and_split_lines_are_reassembled():
    class Dribble(SimulatedGrbl):
        """Hands bytes over a few at a time and starts with line noise, like a real UART."""

        def read(self, size=1):
            return super().read(min(size, 3))

    board = Dribble()
    board._tx[:0] = b"\x00\xfe\x80garbage\r\n"
    link = GrblLink(board)

    boot = link.drain(**FAST)

    assert "garbage" in boot[0] and "\x00" not in boot[0]
    assert any(line.startswith("Grbl ") for line in boot)
    assert link.command("$I", timeout=0.5).ok
