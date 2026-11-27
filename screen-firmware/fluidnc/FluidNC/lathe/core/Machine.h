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
        // The tube (X) turns without end at rpm until jog_cancel(). Needs Idle, no alarm.
        void spin(double rpm, int direction = 1);
        void hold() { link.realtime(RT_FEED_HOLD); }
        void pause(double s) { sleep(s); }  // wait a little between status looks
        void resume() { link.realtime(RT_CYCLE_START); }

        GrblLink& link;
        bool      needs_homing = true;  // until a homing cycle has finished in this session

    private:
        void require_idle();
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
    };

    bool is_busy_state(const std::string& state);

}  // namespace lathe
