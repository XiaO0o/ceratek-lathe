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

    bool Session::step_ok(double step) {
        if (step >= CutSettings::MIN_DEPTH_STEP_MM && step <= CutSettings::MAX_DEPTH_STEP_MM) {
            return true;
        }
        ui.say("The step must be from 0.01 to 10 mm.");
        return false;
    }

    // 1. First the step per round for this tube (shown pre-filled with the one used last), then
    //    each round one step further toward the tube; OK keeps this depth.
    void Session::find_depth() {
        double step;
        do {
            step = ui.ask_value("Step per round toward the tube, mm (0.01 to 10)", _step, false);
        } while (!step_ok(step));
        _step        = step;
        _step_chosen = true;
        ui.say(format("Each round moves the tool %.3f mm further toward the tube.", _step));
        double depth = 0;
        while (true) {
            result.rounds++;
            depth += _step;
            ui.say(format("Round %d: the tool moves to %.3f mm toward the tube.", result.rounds, depth));
            machine.run({ "G21 G90", "G1 Y" + fixed(settings.y_toward_tube * depth, 3) + " F" + fixed(_move_feed, 1) });
            Choice c;
            while ((c = ui.choose_or_settings(format("Is this depth right? Deeper: %.3f mm further. Settings: step and speed.", _step),
                                              Labels { "Depth OK", "Deeper" })) == Choice::Settings) {
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
                double v = i == 0 ? ui.ask_value("Step per round toward the tube, mm (0.01 to 10)", step, false)
                                  : ui.ask_value("Speed of the steps and of the length measurement, mm/min", feed, true);
                if (i == 0 && !step_ok(v)) {
                    continue;
                }
                if (i == 1 && !(v > 0)) {
                    ui.say("The speed must be greater than zero.");
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
    //    switch A ends it there. Switch A here is planned (a message, no warning); any other
    //    stop is not, and ends the job like a stop anywhere else.
    void Session::find_length() {
        const double start  = z_now();
        result.start_z_mm   = start;
        const double a_z    = settings.z_cut_direction * settings.z_search_mm;
        std::string  travel = fixed(settings.jog_feed_mm_per_min, 1);
        while (true) {
            ui.say("Measuring the length: the tool moves slowly toward switch A.");
            size_t mark = machine.link.unsolicited.size();  // alarms after this are this measurement's
            machine.jog_start('Z', a_z, _move_feed);
            ui.offer("Stop here when the length is right. Measure again: back to the start.", Labels { "Stop here", "Measure again" });
            Choice c = Choice::None;
            bool   at_end  = false;  // switch A, or the farthest the measurement may go
            bool   alarmed = false;
            double z       = start;
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
                    at_end  = true;  // a switch: below, A ends the measurement, any other is a stop
                    alarmed = true;
                    break;
                }
                if (s.state == "Idle") {
                    at_end = true;  // the farthest it may go: as if switch A were there
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
            if (at_end) {
                if (alarmed) {
                    // FluidNC sets the alarm state a moment before it sends the ALARM line.
                    Status s    = machine.status();
                    int    code = machine.last_alarm(mark);
                    for (int i = 0; code < 0 && i < 10; i++) {
                        machine.pause(0.05);
                        s    = machine.status();
                        code = machine.last_alarm(mark);
                    }
                    // Alarm 18: switch A outside a probing move (probe_hard_limit). Without a
                    // number, A still pressed (Pn:P) and neither B nor C tells the same.
                    bool switch_a = code == 18 || (code < 0 && s.has_pin('P') && !s.has_pin('Y') && !s.has_pin('Z'));
                    if (!switch_a) {
                        machine.needs_homing = true;
                        throw Stopped("The length measurement was stopped: " + machine.stop_reason(s, mark) + "." +
                                      switch_note(s) + " Recover and home before continuing.");
                    }
                }
                // The cut must stop before switch A, or it touches A again with the tool at depth.
                double reach  = std::fabs(z - start);
                double margin = std::min(settings.switch_a_margin_mm, reach);
                result.length_mm = reach - margin;
                ui.say(format("The length ends at switch A: %.1f mm. The cut stops %.1f mm before it: %.1f mm.", reach, margin,
                              result.length_mm));
                if (alarmed) {
                    // Switch A stopped the jog: recover, and home so that the positions are true
                    // again before cutting.
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
            Choice c = ui.choose(format("Cut %.1f mm at depth %.3f mm, tube %.0f rev/min%s, feed %.0f mm/min?", result.length_mm,
                                        result.depth_mm, rpm, ui.rotation_on() ? "" : " (third axis OFF: the tube does not turn)", feed),
                                 Labels { "Start cut", "Change" });
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
        chosen.rotate              = ui.rotation_on();  // always on the screen: the tube turns in the cut, then stops
        ui.say(format(chosen.rotate ? "Cutting: the tube turns, the tool cuts %.1f mm at depth %.3f mm."
                                    : "Cutting (third axis off, the tube does not turn): the tool goes %.1f mm at depth %.3f mm.",
                      result.length_mm, result.depth_mm));
        machine.run(cut_gcode(depth_y, result.start_z_mm, result.length_mm, chosen));
        result.cut = true;
    }

    // 4. Back to the start corner. Reaching 0 ends it. The way back stops 2 mm short of switches
    //    B and C (their pull-off), so a switch touched on the way means lost steps: a warning.
    //    The cut is done all the same, and the position is found again by homing later.
    void Session::go_back() {
        ui.say("Back to the start corner.");
        try {
            machine.run(return_gcode(settings));
        } catch (const Stopped& e) {
            if (ui.aborted()) {
                throw Quit {};
            }
            report_stop(ui, std::string("On the way back to the start corner: ") + e.what() + switch_note(machine.status()) +
                                " The cut was finished; the position will be found again before the next move.");
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
            } catch (const Stopped& problem) {
                // A failed homing (switch not found, still pressed, another switch pressed) is a
                // warning; Stop during homing is the operator's and only a message.
                report_stop(ui, problem.what() + switch_note(machine.status()));
                if (!ui.confirm("Try again?")) {
                    throw Quit {};
                }
            } catch (const MachineError& problem) {
                ui.say(problem.what());
                if (!ui.confirm("Try again?")) {
                    throw Quit {};
                }
            }
        }
    }

    double locate(Machine& machine, const CutSettings& settings, Ui& ui) {
        // After a stop the controller may still be in alarm, and some alarms (the stop input,
        // switch A outside a probing move) refuse homing until a reset: recover first, so that
        // Locate alone brings the machine back (2026-11-27; it used to need Reset first).
        if (machine.status().alarmed()) {
            machine.recover();
        }
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
        // Reaching 0 ends it. 0 is 2 mm short of switches B and C (their pull-off), so a switch
        // touched on the way means lost steps: a warning. The length is measured all the same.
        ui.say("Back to the zero point (bottom left).");
        try {
            machine.run({ "G1 Z0.000 F" + travel, "G1 Y0.000 F" + travel });
            ui.say("Located: at the zero point.");
        } catch (const Stopped& e) {
            if (ui.stopped_on_purpose()) {
                throw;  // the operator's stop: the caller says so
            }
            ui.warn(std::string("On the way back to the zero point: ") + e.what() + switch_note(machine.status()) +
                    " The length was measured; the position is found again before the next move.");
            machine.recover();
        }
        return length;
    }

    std::string switch_note(const Status& s) {
        std::string names;
        auto        add = [&](char pin, const char* name) {
            if (s.has_pin(pin)) {
                names += names.empty() ? name : std::string(" and ") + name;
            }
        };
        add('Y', "B");
        add('Z', "C");
        add('P', "A");
        if (names.empty()) {
            return "";
        }
        return std::string(" Pressed now: switch ") + names + ".";
    }

    void report_stop(Ui& ui, const std::string& text) {
        if (ui.stopped_on_purpose()) {
            ui.say(text);
        } else {
            ui.warn(text);
        }
    }

}  // namespace lathe
