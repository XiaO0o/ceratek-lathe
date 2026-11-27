// Ceratek lathe screen: running the lathe -- homing, sending moves, and what happens around a stop.
// C++ port of lathe/machine.py. The rules are the ones bench-tested with the laptop software:
//
// - A stop (the stop switch, a limit switch, or any other alarm) ends whatever was running
//   and is reported to the caller as Stopped.
// - Nothing can be sent while the stop switch is still pressed.
// - After any stop the machine must be recovered deliberately and then homed before it will
//   cut again. The controller does not insist on homing after a reset, and a hard stop can
//   lose steps, so the old position is not trusted.
//
// Commands are never simply waited on: while the stop input has the controller locked it
// answers neither ok nor error, so every wait also watches the status report.
#pragma once

#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "Link.h"

namespace lathe {

    constexpr char STOP_PIN = 'E';

    class MachineError : public std::runtime_error {
    public:
        explicit MachineError(const std::string& message) : std::runtime_error(message) {}
    };

    class Stopped : public MachineError {
    public:
        explicit Stopped(const std::string& message) : MachineError(message) {}
    };

    class Machine {
    public:
        Machine(GrblLink& link, Clock& clock, double poll_s = 0.1, double reply_s = 5.0, double reset_wait_s = 2.0) :
            link(link), _clock(clock), _poll_s(poll_s), _reply_s(reply_s), _reset_wait_s(reset_wait_s) {}

        Status status() { return link.status(); }
        bool   stop_switch_pressed() { return status().has_pin(STOP_PIN); }

        void recover();
        void home(double timeout_s = 180.0, const std::string& axes = "");  // axes: "" all, or e.g. "Z"
        // timeout_s < 0: no limit
        void run(const std::vector<std::string>& lines, double timeout_s = -1);
        void jog(char axis, double distance, double feed, double timeout_s = -1);
        // A jog to an absolute position that runs on while the caller watches status(); end it
        // early with jog_cancel(), which keeps the position.
        void jog_start(char axis, double target, double feed);
        void jog_cancel(double timeout_s = 10.0);
        // The tube (X) turning on its own (X: ON, and the X test's Run), in jogs of at most
        // MAX_TURNS_PER_MOVE turns (Plan.h says why). spin() sends the first one and needs Idle,
        // no alarm. keep_spinning(), called a few times a second, sends the next one once less
        // than half a jog is left, so the tube turns on without a stop. It is false when X no
        // longer turns: a jog cancel, a feed hold, an alarm, or the end of the last jog.
        void spin(double rpm, int direction = 1);
        bool keep_spinning();
        // Ends a jog and waits until the controller has stopped (Idle, or stopped by an alarm).
        // If it still moves after timeout_s, before_reset() is called and the controller is
        // reset, which stops every motor at once. False then: the position is lost, home again.
        bool stop_jog(double timeout_s, const std::function<void()>& before_reset = {});
        // A soft reset: every motor stops at once, the position is lost (homing needed). Unlike
        // recover() it does not unlock the controller.
        void reset();
        void hold() { link.realtime(RT_FEED_HOLD); }
        void pause(double s) { sleep(s); }  // wait a little between status looks
        void resume() { link.realtime(RT_CYCLE_START); }

        // The number of the newest "ALARM:n" the controller sent after `mark` (a position in
        // link.unsolicited, see LineLog::size()), or -1 if it sent none.
        int last_alarm(size_t mark = 0) const;
        // Why the controller stopped, in words ("alarm 1: hard limit ...", "the stop switch was
        // pressed"), from the status and the newest alarm after `mark`.
        std::string stop_reason(const Status& s, size_t mark = 0) const;

        GrblLink& link;
        bool      needs_homing = true;  // until a homing cycle has finished in this session

    private:
        void require_idle();
        void send_spin_jog();
        void command(const std::string& line, double timeout_s = -1, const std::string& what = "", bool from_alarm = false);
        void wait_until_idle(double timeout_s);
        [[noreturn]] void stopped(const Status& status, const std::string& what);
        void sleep(double s) {
            if (s > 0) {
                _clock.sleep(s);
            }
        }

        Clock& _clock;
        double _poll_s;
        double _reply_s;
        double _reset_wait_s;
        // the tube turning on its own: speed, direction, and the X (turns) where the jogs sent
        // so far end
        double _spin_rpm = 0;
        int    _spin_dir = 1;
        double _spin_end = 0;
    };

    bool is_busy_state(const std::string& state);

}  // namespace lathe
