import pytest

from lathe import grbl


def test_idle_report_from_a_three_axis_grbl_board():
    status = grbl.parse_status("<Idle|MPos:0.000,-1.250,10.000|FS:0,0|Pn:XZ|WCO:0.000,0.000,0.000>")

    assert status.state == "Idle"
    assert status.substate is None
    assert status.mpos == (0.0, -1.25, 10.0)
    assert status.wpos is None
    assert status.feed == 0.0
    assert status.pins == frozenset("XZ")
    assert status.fields["WCO"] == "0.000,0.000,0.000"
    assert not status.alarmed


def test_report_without_pin_field_means_no_input_is_active():
    assert grbl.parse_status("<Run|MPos:1.000,2.000,3.000|FS:300,0>").pins == frozenset()


def test_hold_substate_and_work_position():
    status = grbl.parse_status("<Hold:0|WPos:5.000,0.000,0.000|Bf:15,128|F:120>\r\n")

    assert (status.state, status.substate) == ("Hold", 0)
    assert status.wpos == (5.0, 0.0, 0.0)
    assert status.feed == 120.0


def test_alarm_report_with_the_fluidnc_estop_pin():
    status = grbl.parse_status("<Alarm|MPos:0.000,0.000,0.000|FS:0,0|Pn:E>")

    assert status.alarmed
    assert status.pins == frozenset("E")


@pytest.mark.parametrize(
    "line",
    [
        "ok",
        "",
        "<>",
        "<Idle|MPos:0.000,abc,0.000>",  # corrupted number
        "<Hold:x|MPos:0.000,0.000,0.000>",  # corrupted substate
        "<12|MPos:0.000,0.000,0.000>",  # state is not a word
        "Idle|MPos:0.000,0.000,0.000>",  # start of the report was lost
    ],
)
def test_lines_that_are_not_a_valid_report_are_rejected(line):
    with pytest.raises(ValueError):
        grbl.parse_status(line)


def test_codes_are_described_and_unknown_ones_fall_back_to_the_number():
    assert "hard limit" in grbl.describe_alarm(1)
    assert "9" in grbl.describe_alarm(9) or "switch not found" in grbl.describe_alarm(9)
    assert grbl.describe_alarm(13) == "firmware-specific alarm 13"
    assert "alarm or jog" in grbl.describe_error(9)
    assert grbl.describe_error(152) == "firmware-specific error 152"
    assert grbl.describe_pin("Y") == "Y limit input"
    assert grbl.describe_pin("7") == "input '7'"
