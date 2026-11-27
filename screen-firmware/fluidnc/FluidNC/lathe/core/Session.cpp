// See Session.h.
#include "Session.h"

#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace lathe {

    std::string format(const char* fmt, ...) {
        char    buf[256];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        return buf;
    }



    CutResult Session::run() {
        result     = CutResult {};
        _step      = settings.depth_step_mm;
        _move_feed = settings.cut_feed_mm_per_min;
        auto check = [&]() {
            if (ui.aborted()) {
                throw Quit {};
            }
        };
        make_ready();
        check();
        // From the start corner (where Locate left the tool; go there if it was moved since).
        machine.run(return_gcode(settings));
        find_depth();
        check();
        find_length();
        check();
        cut_parameters();
        check();
        cut();
        check();
        go_back();
        ui.say(format("Finished: depth %.3f mm (%d round%s), length %.1f mm.", result.depth_mm, result.rounds,
                      result.rounds == 1 ? "" : "s", result.length_mm));
        return result;
    }

    double Session::z_now() {
        Status s = machine.status();
        if (!s.has_mpos || s.mpos.size() < 3) {
            throw MachineError("The controller did not report its position.");
        }
        return s.mpos[2];
    }

    // 1. Each round one step further toward the tube; OK keeps this depth.
    void Session::find_depth() {
        double depth = 0;
        while (true) {
            result.rounds++;
            depth += _step;
            ui.say(format("Round %d: the tool moves to %.3f mm toward the tube.", result.rounds, depth));
            machine.run({ "G21 G90", "G1 Y" + fixed(settings.y_toward_tube * depth, 3) + " F" + fixed(_move_feed, 1) });
            Choice c;
            while ((c = ui.choose_or_settings(format("Is this position right? OK: yes, measure the length. Return: no, %.3f mm "
                                                     "further. Settings: change the step and the speed.",
                                                     _step))) == Choice::Settings) {
                movement_settings();  // nothing moves; the same question again
            }
            if (c == Choice::Ok) {
                result.depth_mm = depth;
                return;
            }
        }
    }

    // The movement settings, from the depth question: the step per round and the speed of the
    // steps and of the length measurement. They hold for the rest of this tube.
    void Session::movement_settings() {
        double step = _step, feed = _move_feed;
        int    i    = 0;
        while (i < 2) {
            try {
                double v = i == 0 ? ui.ask_value("Step per round toward the tube, mm", step, false)
                                  : ui.ask_value("Speed of the steps and of the length measurement, mm/min", feed, true);
                if (!(v > 0) || (i == 0 && v > 10.0)) {
                    ui.say(i == 0 ? "The step must be more than 0 and at most 10 mm." : "The speed must be greater than zero.");
                    continue;
                }
                (i == 0 ? step : feed) = v;
                i++;
            } catch (const GoBack&) {
                i = 0;
            }
        }
        _step      = step;
        _move_feed = feed;
        ui.say(format("Step %.3f mm, speed %.0f mm/min.", _step, _move_feed));
    }

    // 2. Slowly along the tube toward switch A; OK fixes the length, Return starts it again,
    //    switch A ends it there.
    void Session::find_length() {
        const double start  = z_now();
        result.start_z_mm   = start;
        const double a_z    = settings.z_cut_direction * settings.z_search_mm;
        std::string  travel = fixed(settings.jog_feed_mm_per_min, 1);
        while (true) {
            ui.say("Measuring the length: the tool moves slowly toward switch A.");
            machine.jog_start('Z', a_z, _move_feed);
            ui.offer("OK: the length is this long. Return: not right, back to the start and measure again.");
            Choice c = Choice::None;
            bool   at_a = false;
            double z    = start;
            while (true) {
                c = ui.poll_choice();
                if (c != Choice::None) {
                    break;
                }
                Status s = machine.status();
                if (s.has_mpos && s.mpos.size() >= 3) {
                    z = s.mpos[2];
                }
                if (s.alarmed()) {
                    at_a = true;  // switch A (or another switch): the measurement ends there
                    break;
                }
                if (s.state == "Idle") {
                    at_a = true;  // the farthest it may go: as if switch A were there
                    break;
                }
                machine.pause(0.1);
            }
            ui.withdraw();
            if (ui.aborted()) {
                throw Quit {};  // the main-page button: its reset stopped the tool, nothing more
            }
            if (c == Choice::End) {
                machine.jog_cancel();
                throw Quit {};
            }
            if (at_a) {
                result.length_mm = std::fabs(z - start);
                ui.say(format("The length ends at switch A: %.1f mm.", result.length_mm));
                Status s = machine.status();
                if (s.alarmed()) {
                    // A switch touched while moving is a stop: recover, and home so that the
                    // positions are true again before cutting.
                    machine.recover();
                    machine.home();
                }
                return;
            }
            machine.jog_cancel();
            if (c == Choice::Ok) {
                result.length_mm = std::fabs(z_now() - start);
                ui.say(format("Length: %.1f mm.", result.length_mm));
                return;
            }
            // Return: back to the start of the length and measure again
            ui.say("Back to the start of the length.");
            machine.run({ "G21 G90", "G1 Z" + fixed(start, 3) + " F" + travel });
        }
    }

    // Before cutting, the machine waits while the operator sets the cut: the tube's speed and
    // the cutting feed, starting from config.yaml's values; OK starts the cut, Return goes
    // through the values again.
    void Session::cut_parameters() {
        double rpm  = settings.tube_rpm;
        double feed = settings.cut_feed_mm_per_min;
        ui.say(format("Depth %.3f mm and length %.1f mm are set. Now the cutting values.", result.depth_mm, result.length_mm));
        while (true) {
            int i = 0;
            while (i < 2) {
                try {
                    double v = i == 0 ? ui.ask_value("Tube speed, rev/min", rpm, false)
                                      : ui.ask_value("Cutting feed along the tube, mm/min", feed, true);
                    if (!(v > 0)) {
                        ui.say("The value must be greater than zero.");
                        continue;
                    }
                    (i == 0 ? rpm : feed) = v;
                    i++;
                } catch (const GoBack&) {
                    i = 0;
                }
            }
            Choice c = ui.choose(format("Cut %.1f mm at depth %.3f mm, tube %.0f rev/min (third axis %s), feed %.0f mm/min? "
                                        "OK: start cutting. Return: change the values.",
                                        result.length_mm, result.depth_mm, rpm, ui.rotation_on() ? "on" : "OFF", feed));
            if (c == Choice::Ok) {
                result.tube_rpm            = rpm;
                result.cut_feed_mm_per_min = feed;
                return;
            }
        }
    }

    // 3. The tube turns while the tool cuts the length at the depth.
    void Session::cut() {
        double      depth_y = settings.y_toward_tube * result.depth_mm;
        CutSettings chosen  = settings;
        chosen.tube_rpm            = result.tube_rpm;
        chosen.cut_feed_mm_per_min = result.cut_feed_mm_per_min;
        chosen.rotate              = ui.rotation_on();  // as the switch is when the cut starts
        ui.say(format(chosen.rotate ? "Cutting: the tube turns, the tool cuts %.1f mm at depth %.3f mm."
                                    : "Cutting (third axis off, the tube does not turn): the tool goes %.1f mm at depth %.3f mm.",
                      result.length_mm, result.depth_mm));
        machine.run(cut_gcode(depth_y, result.start_z_mm, result.length_mm, chosen));
        result.cut = true;
    }

    // 4. Back to the start corner. Reaching 0 ends it; a switch touched on the way stops it,
    //    which also counts as being back (the position is then found again by homing later).
    void Session::go_back() {
        ui.say("Back to the start corner.");
        try {
            machine.run(return_gcode(settings));
        } catch (const Stopped&) {
            if (ui.aborted()) {
                throw Quit {};
            }
            ui.say("Stopped at a switch on the way back. The position will be found again before the next move.");
        }
    }

    void Session::make_ready() {
        while (true) {
            try {
                if (machine.stop_switch_pressed()) {
                    ui.say("The stop switch is pressed. Release it.");
                    if (!ui.confirm("Is the stop switch released?")) {
                        throw Quit {};
                    }
                    continue;
                }
                Status status = machine.status();
                if (!status.alarmed() && !machine.needs_homing) {
                    return;
                }
                ui.say("The machine goes to the start corner first (switches B and C): the tool moves back from the tube, then along it.");
                if (!ui.confirm("Is everything clear of the tool? Go to the start corner now?")) {
                    throw Quit {};
                }
                if (status.alarmed()) {
                    machine.recover();
                }
                machine.home();
                ui.say("At the start corner.");
                return;
            } catch (const MachineError& problem) {
                ui.say(problem.what());
                if (!ui.confirm("Try again?")) {
                    throw Quit {};
                }
            }
        }
    }

    double locate(Machine& machine, const CutSettings& settings, Ui& ui) {
        ui.say("Locating: the tool goes back to switch B, then along the tube to switch C.");
        machine.home();
        ui.say("At switch C. Now along the tube to switch A, to measure the length.");
        // While Z travels, keep the tool a little toward the tube, clear of switch B.
        const double clear_y = settings.y_toward_tube * 5.0;
        std::string  travel  = fixed(settings.jog_feed_mm_per_min, 1);
        double       a_z     = settings.z_cut_direction * settings.z_search_mm;
        machine.run({ "G21 G90", "G1 Y" + fixed(clear_y, 3) + " F" + travel, "G38.2 Z" + fixed(a_z, 3) + " F" + travel });
        Status s = machine.status();
        if (!s.has_mpos || s.mpos.size() < 3) {
            throw MachineError("The controller did not report its position at switch A.");
        }
        double length = std::fabs(s.mpos[2]);
        ui.say(format("Switch A is %.1f mm from switch C.", length));
        // Back to the zero point by finding switch C again: the tool goes back along the tube
        // until switch C is touched, wherever that is (2026-11-24: a plain move back went past
        // C when steps were lost, and switch C touched during a plain move is a hard-limit alarm).
        // Back to the zero point: Z to 0 (Y stays clear of switch B meanwhile), then Y to 0.
        // Reaching 0 ends it; a switch touched on the way stops it, which also ends it.
        ui.say("Back to the zero point (bottom left).");
        try {
            machine.run({ "G1 Z0.000 F" + travel, "G1 Y0.000 F" + travel });
            ui.say("Located: at the zero point.");
        } catch (const Stopped&) {
            ui.say("Stopped at a switch on the way back: located, the position is found again before the next move.");
            machine.recover();
        }
        return length;
    }

}  // namespace lathe
