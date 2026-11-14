import re
from dataclasses import replace
from pathlib import Path

import pytest

from lathe import passes
from lathe.settings import load_cut_settings

SETTINGS = passes.CutSettings(
    rpm=60, feed_per_rev_mm=0.1, max_depth_mm=0.10, first_depth_mm=0.05, clearance_mm=1.0,
    plunge_revs=5, spinup_revs=2, y_toward_tube=1, z_cut_direction=1, min_wall_mm=0.4,
)  # fmt: skip


def moves(program):
    """Each G1 line as (turns, {axis: distance}, minutes)."""
    found = []
    for line in program:
        if line.startswith("G1"):
            words = dict((w[0], float(w[1:])) for w in line.split()[1:])
            found.append((words.pop("X"), {k: v for k, v in words.items() if k != "F"}, 1 / words["F"]))
    return found


# -- what the operator may ask for -----------------------------------------


def test_a_sensible_job_has_no_problems():
    assert passes.Job(inner_d=8.0, start_od=10.6, target_od=10.0, length=150).problems(SETTINGS) == []


@pytest.mark.parametrize(
    "job, phrase",
    [
        (passes.Job(8.0, 10.0, 10.2, 150), "smaller than the current"),
        (passes.Job(8.0, 10.0, 7.5, 150), "larger than the inner"),
        (passes.Job(8.0, 10.0, 8.6, 150), "wall of 0.30 mm"),
        (passes.Job(8.0, 10.0, 9.0, 0), "greater than zero"),
    ],
)
def test_a_job_that_must_not_run_says_why(job, phrase):
    assert any(phrase in problem for problem in job.problems(SETTINGS))


# -- how deep the next pass is ----------------------------------------------


def test_the_first_pass_is_the_shallow_calibration_pass():
    assert passes.next_depth(10.6, 10.0, SETTINGS, first=True) == pytest.approx(0.05)


def test_the_first_pass_never_takes_more_than_is_left():
    assert passes.next_depth(10.04, 10.0, SETTINGS, first=True) == pytest.approx(0.02)


def test_later_passes_split_what_is_left_evenly_under_the_limit():
    # 0.25 mm left on the radius, limit 0.10: three passes of 0.0833, not 0.10 + 0.10 + 0.05
    depth = passes.next_depth(10.5, 10.0, SETTINGS, first=False)

    assert depth == pytest.approx(0.25 / 3)
    assert depth <= SETTINGS.max_depth_mm


def test_exactly_one_limit_left_is_a_single_pass():
    assert passes.next_depth(10.2, 10.0, SETTINGS, first=False) == pytest.approx(0.10)


@pytest.mark.parametrize("measured", [10.0, 10.008, 9.97])
def test_no_further_pass_once_the_target_is_reached_or_passed(measured):
    assert passes.next_depth(measured, 10.0, SETTINGS, first=False) == 0.0


def test_passes_planned_from_each_measurement_reach_the_target():
    od, first, count = 10.6, True, 0
    while (depth := passes.next_depth(od, 10.0, SETTINGS, first=first)) > 0:
        od -= 2 * depth  # a machine that removes exactly what was asked
        first, count = False, count + 1

    assert od == pytest.approx(10.0, abs=2 * passes.DONE_WITHIN_MM)
    assert count == 4  # 0.05, then 0.25 in three equal passes


# -- the moves of one pass --------------------------------------------------


def test_a_pass_plunges_feeds_retracts_and_returns_to_where_it_started():
    planned = passes.plan_pass(measured_od=10.6, depth_mm=0.05, gap_mm=1.0, length_mm=150, settings=SETTINGS)
    spin, plunge, feed, retract = moves(planned.gcode)

    assert planned.gcode[:2] == ("G91", "G93") and planned.gcode[-3:] == ("G94", "G0 Z-150.0000", "G90")
    assert spin[1] == {}
    assert plunge[1] == {"Y": pytest.approx(1.05)}  # the gap plus the depth
    assert feed[1] == {"Z": pytest.approx(150)}
    assert retract[1] == {"Y": pytest.approx(-1.05)}
    assert planned.expected_od == pytest.approx(10.5)


def test_the_tube_turns_at_the_set_speed_in_every_cutting_move():
    planned = passes.plan_pass(10.6, 0.05, 1.0, 150, SETTINGS)

    for turns, _, minutes in moves(planned.gcode):
        assert turns / minutes == pytest.approx(SETTINGS.rpm, rel=1e-3)


def test_the_feed_move_advances_by_the_feed_per_turn():
    planned = passes.plan_pass(10.6, 0.05, 1.0, 150, SETTINGS)
    turns, axes, _ = moves(planned.gcode)[2]

    assert axes["Z"] / turns == pytest.approx(SETTINGS.feed_per_rev_mm)
    assert planned.minutes == pytest.approx((2 + 5 + 1500 + 5) / 60)


def test_axis_directions_come_from_the_settings():
    mirrored = replace(SETTINGS, y_toward_tube=-1, z_cut_direction=-1)
    _, plunge, feed, retract = moves(passes.plan_pass(10.6, 0.05, 1.0, 150, mirrored).gcode)

    assert plunge[1]["Y"] < 0 and feed[1]["Z"] < 0 and retract[1]["Y"] > 0


def test_every_line_is_plain_gcode_the_controller_accepts():
    for line in passes.plan_pass(10.6, 0.05, 1.0, 150, SETTINGS).gcode:
        assert re.fullmatch(r"G\d+( [XYZF]-?\d+\.\d+)*", line), line


def test_impossible_pass_requests_are_refused():
    with pytest.raises(ValueError):
        passes.plan_pass(10.6, 0.0, 1.0, 150, SETTINGS)
    with pytest.raises(ValueError):
        passes.plan_pass(10.6, 0.05, -0.1, 150, SETTINGS)


# -- correcting from the measurement ----------------------------------------


def test_after_a_cut_the_gap_is_the_distance_the_tool_plunged():
    assert passes.gap_after_pass(1.0, depth_mm=0.05) == pytest.approx(1.05)


def test_a_measurement_that_did_not_change_means_the_tool_never_touched():
    assert passes.pass_removed_material(od_before=10.60, od_measured=10.56)
    assert not passes.pass_removed_material(od_before=10.60, od_measured=10.60)
    assert not passes.pass_removed_material(od_before=10.60, od_measured=10.595)


def test_a_machine_that_undercuts_still_converges_on_the_target():
    od, gap, first = 10.6, SETTINGS.clearance_mm, True
    true_gap = gap + 0.04  # the touch-off was 0.04 mm short of the surface: every cut starts shallow
    for _ in range(12):
        depth = passes.next_depth(od, 10.0, SETTINGS, first=first)
        if depth == 0:
            break
        plunge = moves(passes.plan_pass(od, depth, gap, 150, SETTINGS).gcode)[1][1]["Y"]
        removed = max(0.0, plunge - true_gap)  # what the tool really reaches
        new_od = od - 2 * removed
        gap, true_gap = passes.gap_after_pass(gap, depth), true_gap + removed
        od, first = new_od, False

    assert od == pytest.approx(10.0, abs=2 * passes.DONE_WITHIN_MM)
    assert od >= 10.0 - 2 * passes.DONE_WITHIN_MM  # never cut below the target


# -- settings ----------------------------------------------------------------


def test_the_shipped_settings_file_loads():
    settings = load_cut_settings(Path(__file__).parent.parent / "config" / "lathe.toml")

    assert settings.rpm > 0 and settings.y_toward_tube in (1, -1)


def test_a_misspelt_setting_is_an_error_not_a_silent_default(tmp_path):
    text = (Path(__file__).parent.parent / "config" / "lathe.toml").read_text().replace("rpm =", "rpn =")
    broken = tmp_path / "lathe.toml"
    broken.write_text(text)

    with pytest.raises(ValueError, match="rpn|rpm"):
        load_cut_settings(broken)


def test_settings_that_make_no_sense_are_refused():
    with pytest.raises(ValueError):
        replace(SETTINGS, rpm=0)
    with pytest.raises(ValueError):
        replace(SETTINGS, y_toward_tube=2)
