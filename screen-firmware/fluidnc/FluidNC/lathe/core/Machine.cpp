// C++ port of lathe/machine.py. See Machine.h.
#include "Machine.h"

#include <cstdio>
#include <cstdlib>

#include "Plan.h"

namespace lathe {

    bool is_busy_state(const std::string& state) { return state == "Run" || state == "Jog" || state == "Home" || state == "Hold"; }

    void Machine::recover() {
        if (stop_switch_pressed()) {
            throw MachineError("The stop switch is still pressed. Release it first.");
        }
        needs_homing = true;
        link.realtime(RT_SOFT_RESET);
        sleep(_reset_wait_s);
        double quiet = _reset_wait_s < 0.5 ? _reset_wait_s : 0.5;
        link.drain(quiet, _reset_wait_s + 1, _reset_wait_s);
        Status s = status();
        if (s.has_pin(STOP_PIN)) {
            throw MachineError("The stop switch is still pressed. Release it first.");
        }
        if (s.alarmed()) {
            command("$X", -1, "", true);  // the start-up lock; homing is still required
        }
    }

    void Machine::home(double timeout_s, const std::string& axes) {
        Status s = status();
        if (s.has_pin(STOP_PIN)) {
            throw MachineError("The stop switch is pressed. Release it and recover first.");
        }
        if (is_busy_state(s.state)) {
            throw MachineError("The machine is busy (" + s.state + ").");
        }
        // Homing only some axes ("Z") keeps the others' reference, so it can only follow a
        // full homing; it does not make a machine that needs homing ready.
        bool was_needed = needs_homing;
        needs_homing    = true;
        command("$H" + axes, timeout_s, "Homing", s.alarmed());
        needs_homing = axes.empty() ? false : was_needed;
    }

    void Machine::run(const std::vector<std::string>& lines, double timeout_s) {
        if (needs_homing) {
            throw MachineError("The machine has not been homed since it was started or stopped. Home it first.");
        }
        require_idle();
        for (const auto& line : lines) {
            command(line);
        }
        wait_until_idle(timeout_s);
    }

    void Machine::jog(char axis, double distance, double feed, double timeout_s) {
        require_idle();
        // Python: f"$J=G91 {axis}{distance:.4f} F{feed:g}"
        char f[32];
        std::snprintf(f, sizeof(f), "%g", feed);
        command(std::string("$J=G91 ") + axis + fixed(distance, 4) + " F" + f);
        wait_until_idle(timeout_s);
    }

    void Machine::jog_start(char axis, double target, double feed) {
        require_idle();
        char f[32];
        std::snprintf(f, sizeof(f), "%g", feed);
        command(std::string("$J=G90 G21 ") + axis + fixed(target, 4) + " F" + f);
    }

    void Machine::spin(double rpm, int direction) {
        require_idle();
        char f[32];
        std::snprintf(f, sizeof(f), "%g", rpm);
        // 100000 turns: hours at any speed
        command(std::string("$J=G91 G21 X") + (direction < 0 ? "-" : "") + "100000.0000 F" + f);
    }

    void Machine::jog_cancel(double timeout_s) {
        link.realtime(RT_JOG_CANCEL);
        wait_until_idle(timeout_s);
    }

    void Machine::require_idle() {
        Status s = status();
        if (s.has_pin(STOP_PIN)) {
            throw MachineError("The stop switch is pressed.");
        }
        if (s.alarmed()) {
            throw MachineError("The machine is in alarm. Recover and home it first.");
        }
        if (s.state != "Idle") {
            throw MachineError("The machine is busy (" + s.state + ").");
        }
    }

    void Machine::command(const std::string& line, double timeout_s, const std::string& what_in, bool from_alarm) {
        std::string what = what_in.empty() ? "Command '" + line + "'" : what_in;
        size_t      mark = link.unsolicited.size();
        link.send(line);
        double deadline = _clock.now() + (timeout_s >= 0 ? timeout_s : _reply_s);
        while (true) {
            Status                   s       = status();
            std::vector<std::string> replies = link.unsolicited.since(mark);
            for (const auto& r : replies) {
                if (r == "ok") {
                    return;
                }
            }
            // A stop comes first: once the controller is in alarm it refuses the rest of the
            // pass with error:9, and the operator needs the cause, not that symptom.
            bool new_alarm = false;
            for (const auto& r : replies) {
                if (starts_with_nocase(r, "ALARM:")) {
                    new_alarm = true;
                }
            }
            if (new_alarm || (s.alarmed() && !from_alarm)) {
                stopped(s, what);
            }
            for (const auto& r : replies) {
                if (starts_with_nocase(r, "error:")) {
                    std::string detail = trim(r.substr(6));
                    std::string code   = is_digits(detail) ? "error:" + detail + " (" + describe_error(std::atoi(detail.c_str())) + ")" : r;
                    throw MachineError(what + " was refused: " + code + ".");
                }
            }
            if (!is_busy_state(s.state) && _clock.now() >= deadline) {
                throw MachineError(what + " got no answer from the controller.");
            }
            sleep(_poll_s);
        }
    }

    void Machine::wait_until_idle(double timeout_s) {
        double deadline     = timeout_s < 0 ? -1 : _clock.now() + timeout_s;
        int    idle_in_a_row = 0;
        while (idle_in_a_row < 3) {  // a move may not have started at the very first look
            Status s = status();
            if (s.alarmed()) {
                stopped(s, "The move");
            }
            idle_in_a_row = s.state == "Idle" ? idle_in_a_row + 1 : 0;
            if (deadline >= 0 && _clock.now() >= deadline) {
                throw MachineError("The move did not finish in the time allowed.");
            }
            sleep(_poll_s);
        }
    }

    void Machine::stopped(const Status& s, const std::string& what) {
        needs_homing = true;
        std::string last_alarm;
        for (const auto& l : link.unsolicited.all()) {
            if (starts_with_nocase(l, "ALARM:")) {
                last_alarm = l;
            }
        }
        std::string reason = s.has_pin(STOP_PIN) ? "the stop switch was pressed" : "the controller raised an alarm";
        if (!last_alarm.empty() && !s.has_pin(STOP_PIN)) {
            std::string num = trim(last_alarm.substr(6));
            if (is_digits(num)) {
                int code = std::atoi(num.c_str());
                reason   = "alarm " + std::to_string(code) + ": " + describe_alarm(code);
            }
        }
        throw Stopped(what + " was stopped: " + reason + ". Recover and home before continuing.");
    }

}  // namespace lathe
