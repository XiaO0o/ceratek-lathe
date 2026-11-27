// PC tests: an in-memory stand-in for the controller. C++ port of lathe/sim.py.
// A move keeps the controller busy for a set number of status polls. The stop input behaves
// as FluidNC v4.1.1 was seen to on the bench: nothing is answered until a reset.
#pragma once

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/Link.h"

namespace lathe_test {

    using namespace lathe;

    class FakeClock : public Clock {
    public:
        double now() override { return _t; }
        void   sleep(double s) override { _t += s > 0 ? s : 0; }
        void   advance(double s) { _t += s; }

    private:
        double _t = 1000.0;
    };

    class SimulatedGrbl : public BytePort {
    public:
        SimulatedGrbl(std::string pins = "", bool lock_on_reset = false, int move_polls = 0) :
            _lock_on_reset(lock_on_reset), _move_polls(move_polls) {
            for (char c : pins) {
                this->pins.insert(c);
            }
            boot();
        }

        // -- test controls: what a person at the bench would do --
        void press(char pin) {
            pins.insert(pin);
            if (pin == 'E' && !_critical) {
                _critical = true;
                _busy     = 0;
                homed     = false;
                state     = "Alarm";
                say("[MSG:INFO: Stopped by estop_pin]");
                say("[MSG:INFO: ALARM: Hard Stop]");
                say("ALARM:13");
            }
        }
        void release(char pin) { pins.erase(pin); }
        void alarm(int code) {
            state = "Alarm";
            say("ALARM:" + std::to_string(code));
        }

        // -- the port surface --
        size_t in_waiting() override { return _tx.size(); }
        size_t read(uint8_t* buffer, size_t max) override {
            size_t n = _tx.size() < max ? _tx.size() : max;
            for (size_t i = 0; i < n; i++) {
                buffer[i] = static_cast<uint8_t>(_tx[i]);
            }
            _tx.erase(0, n);
            return n;
        }
        void write(const uint8_t* data, size_t size) override {
            for (size_t i = 0; i < size; i++) {
                char c = static_cast<char>(data[i]);
                if (silent) {
                    continue;
                }
                if (c == '?') {
                    polls++;
                    if (on_poll) {
                        on_poll(polls);
                    }
                    tick();
                    say(status_line());
                } else if (c == 0x18) {
                    _rx.clear();
                    _busy     = 0;
                    _critical = pins.count('E') > 0;  // a held stop switch survives the reset
                    boot();
                } else if (c == static_cast<char>(0x85)) {  // jog cancel: stops where it is
                    if (state == "Jog") {
                        state = "Idle";
                        _busy = 0;
                    }
                } else if (c == '!') {
                    if (state == "Run" || state == "Jog") {
                        state = "Hold";
                    }
                } else if (c == '~') {
                    if (state == "Hold") {
                        state = _busy ? "Run" : "Idle";
                    }
                } else if (c == '\n') {
                    std::string line = lathe::trim(_rx);
                    _rx.clear();
                    execute(line);
                } else if (c != '\r') {
                    _rx += c;
                }
            }
        }

        void say(const std::string& line) { _tx += line + "\r\n"; }
        void clear_output() { _tx.clear(); }

        std::string              state = "Idle";
        std::set<char>           pins;
        std::vector<std::string> received;
        std::vector<std::string> executed;
        std::vector<std::string> homings;  // $H lines, in order
        bool                     silent       = false;
        bool                     homed        = false;
        bool                     homing_fails = false;
        double                   pos[3]       = { 0, 0, 0 };  // machine position X, Y, Z
        double                   switch_a_z   = 1e9;          // where switch A (the probe) is touched on Z
        int                      polls        = 0;
        // test hooks: called before each status poll is answered; replaces execution of a line
        std::function<void(int)>                 on_poll;
        std::function<bool(const std::string&)> intercept;

    private:
        static std::string upper(std::string s) {
            for (auto& c : s) {
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            return s;
        }
        void boot() {
            state = (_lock_on_reset || _critical) ? "Alarm" : "Idle";
            say("");
            say("Grbl 1.3a ['$' for help]");
            say("[MSG:Mode=AP:SSID=SIM_NET:IP=192.168.4.1:MAC=AA-BB-CC-DD-EE-FF]");
            if (_lock_on_reset) {
                say("[MSG:'$H'|'$X' to unlock]");
            }
        }
        void execute(const std::string& line) {
            received.push_back(line);
            if (intercept && intercept(line)) {
                return;
            }
            std::string u = upper(line);
            if (_critical) {
                if (u == "$X") {
                    say("[MSG:ERR: Reset to continue]");
                }
                return;  // FluidNC answers neither ok nor error in this alarm
            }
            if (u.empty()) {
                say("ok");
            } else if (u == "$I") {
                say("[VER:1.3a.SIM:]");
                say("[OPT:PHS]");
                say("ok");
            } else if (u == "$X") {
                if (state == "Alarm") {
                    state = "Idle";
                    say("[MSG:Caution: Unlocked]");
                }
                say("ok");
            } else if (u == "$H" || u == "$HX" || u == "$HY" || u == "$HZ") {
                homings.push_back(line);
                _homing_axes = u.size() > 2 ? u.substr(2) : "XYZ";
                state        = "Home";
                _busy = _move_polls > 1 ? _move_polls : 1;
            } else if (u.rfind("$J=", 0) == 0) {
                if (state == "Alarm") {
                    say("error:9");
                } else {
                    executed.push_back(line);
                    bool absolute = u.find("G90") != std::string::npos;
                    if (!absolute) {
                        move(u, true);
                    }
                    start("Jog");
                    if (absolute) {
                        _busy = 100000;  // a long jog: runs until cancelled; tests move pos themselves
                    }
                    say("ok");
                }
            } else if (!u.empty() && u[0] == '$') {
                say("error:3");
            } else if (state == "Alarm") {
                say("error:9");
            } else {
                if (u.rfind("G38.2 ", 0) == 0) {
                    executed.push_back(line);
                    double target = word(u, 'Z', pos[2]);
                    if (target >= switch_a_z) {
                        pos[2] = switch_a_z;
                        start("Run");
                        say("[PRB:0.000,0.000,0.000:1]");
                        say("ok");
                    } else {
                        pos[2] = target;
                        state  = "Alarm";
                        say("ALARM:5");
                    }
                    return;
                }
                if (u.rfind("G0 ", 0) == 0 || u.rfind("G1 ", 0) == 0) {
                    executed.push_back(line);
                    move(u, false);
                    start("Run");
                }
                say("ok");
            }
        }
        static double word(const std::string& u, char letter, double otherwise) {
            for (size_t i = 0; i + 1 < u.size(); i++) {
                if (u[i] == letter && (i == 0 || u[i - 1] == ' ' || u[i - 1] == '=')) {
                    return std::atof(u.c_str() + i + 1);
                }
            }
            return otherwise;
        }
        void move(const std::string& u, bool relative) {
            const char axes[3] = { 'X', 'Y', 'Z' };
            for (int a = 0; a < 3; a++) {
                double v = word(u, axes[a], relative ? 0 : pos[a]);
                pos[a]   = relative ? pos[a] + v : v;
            }
        }
        std::string _homing_axes = "XYZ";
        void start(const std::string& s) {
            if (_move_polls) {
                state = s;
                _busy += _move_polls;
            }
        }
        void tick() {
            if (!_busy || state == "Hold") {
                return;
            }
            _busy -= 1;
            if (_busy) {
                return;
            }
            if (state != "Home") {
                state = "Idle";
            } else if (homing_fails) {
                state = "Alarm";
                say("[MSG:INFO: ALARM: Homing Fail Approach]");
                say("ALARM:9");
            } else {
                state = "Idle";
                homed = true;
                for (char a : _homing_axes) {
                    pos[a - 'X'] = 0;
                }
                say("[MSG:Homed]");
                say("ok");
            }
        }
        std::string status_line() {
            char mp[64];
            std::snprintf(mp, sizeof(mp), "%.3f,%.3f,%.3f", pos[0], pos[1], pos[2]);
            std::string r = "<" + state + "|MPos:" + mp + "|FS:0,0";
            if (!pins.empty()) {
                r += "|Pn:";
                for (char c : pins) {
                    r += c;
                }
            }
            return r + ">";
        }

        bool        _lock_on_reset;
        int         _move_polls;
        int         _busy     = 0;
        bool        _critical = false;
        std::string _rx;
        std::string _tx;
    };

}  // namespace lathe_test
