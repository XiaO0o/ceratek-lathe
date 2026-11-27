// PC tests for the lathe screen's core logic: the controller link and machine rules (ports of
// the ceratek-lathe repository's tests), and the group's pass plan (2026-11-24).
//
//     ./run.sh          (builds with g++ and runs everything)

#include <cmath>
#include <cstdio>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "SimGrbl.h"
#include "core/Grbl.h"
#include "core/Link.h"
#include "core/Machine.h"
#include "core/Plan.h"
#include "core/Session.h"

using namespace lathe;
using namespace lathe_test;

// ---------------------------------------------------------------- tiny test framework

struct TestCase {
    const char*           name;
    std::function<void()> fn;
};
static std::vector<TestCase>& tests() {
    static std::vector<TestCase> t;
    return t;
}
struct Register {
    Register(const char* n, std::function<void()> f) { tests().push_back({ n, f }); }
};
#define TEST(name)                                     \
    static void         name();                        \
    static Register     reg_##name(#name, name);       \
    static void         name()

struct Failure {
    std::string what;
};
#define CHECK(cond)                                                                                  \
    do {                                                                                             \
        if (!(cond)) {                                                                               \
            throw Failure { std::string(__FILE__ ":") + std::to_string(__LINE__) + ": " #cond };     \
        }                                                                                            \
    } while (0)
#define CHECK_EQ(a, b)                                                                                             \
    do {                                                                                                           \
        auto _a = (a);                                                                                             \
        auto _b = (b);                                                                                             \
        if (!(_a == _b)) {                                                                                         \
            std::ostringstream _m;                                                                                 \
            _m << __FILE__ << ":" << __LINE__ << ": " #a " == " #b "\n      got: " << _a << "\n expected: " << _b; \
            throw Failure { _m.str() };                                                                            \
        }                                                                                                          \
    } while (0)
#define CHECK_NEAR(a, b, tol) CHECK(std::fabs((a) - (b)) <= (tol))

template <typename E, typename F>
static std::string expect_throw(F f) {
    try {
        f();
    } catch (const E& e) {
        return e.what();
    }
    throw Failure { "expected an exception that was not thrown" };
}
static bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

// A clock where every look at it takes a little time, as on a real computer.
class TestClock : public Clock {
public:
    double now() override { return _t += 0.0001; }
    void   sleep(double s) override { _t += s > 0 ? s : 0; }

private:
    double _t = 1000.0;
};

// ---------------------------------------------------------------- test_grbl.py

TEST(test_idle_report_from_a_three_axis_grbl_board) {
    Status s;
    CHECK(parse_status("<Idle|MPos:0.000,-1.250,10.000|FS:0,0|Pn:XZ|WCO:0.000,0.000,0.000>", s));
    CHECK_EQ(s.state, std::string("Idle"));
    CHECK_EQ(s.substate, -1);
    CHECK(s.has_mpos && s.mpos.size() == 3 && s.mpos[1] == -1.25 && s.mpos[2] == 10.0);
    CHECK(s.has_feed && s.feed == 0.0);
    CHECK_EQ(s.pins, std::string("XZ"));
    CHECK(!s.alarmed());
}

TEST(test_report_without_pin_field_means_no_input_is_active) {
    Status s;
    CHECK(parse_status("<Run|MPos:1.000,2.000,3.000|FS:300,0>", s));
    CHECK(s.pins.empty());
}

TEST(test_hold_substate_and_work_position) {
    Status s;
    CHECK(parse_status("<Hold:0|WPos:5.000,0.000,0.000|Bf:15,128|F:120>\r\n", s));
    CHECK_EQ(s.state, std::string("Hold"));
    CHECK_EQ(s.substate, 0);
    CHECK_EQ(s.feed, 120.0);
}

TEST(test_alarm_report_with_the_fluidnc_estop_pin) {
    Status s;
    CHECK(parse_status("<Alarm|MPos:0.000,0.000,0.000|FS:0,0|Pn:E>", s));
    CHECK(s.alarmed());
    CHECK(s.has_pin('E'));
}

TEST(test_lines_that_are_not_a_valid_report_are_rejected) {
    const char* lines[] = { "ok", "", "<>", "<Idle|MPos:0.000,abc,0.000>", "<Hold:x|MPos:0.000,0.000,0.000>",
                            "<12|MPos:0.000,0.000,0.000>", "Idle|MPos:0.000,0.000,0.000>" };
    for (const char* l : lines) {
        Status s;
        CHECK(!parse_status(l, s));
    }
}

TEST(test_codes_are_described_and_unknown_ones_fall_back_to_the_number) {
    CHECK(contains(describe_alarm(1), "hard limit"));
    CHECK(contains(describe_alarm(9), "switch not found"));
    CHECK(contains(describe_alarm(13), "e-stop"));       // FluidNC's own numbers (src/Alarm.h)
    CHECK(contains(describe_alarm(18), "switch A"));
    CHECK_EQ(describe_alarm(99), std::string("firmware-specific alarm 99"));
    CHECK(contains(describe_error(9), "alarm or jog"));
    CHECK_EQ(describe_error(152), std::string("firmware-specific error 152"));
    CHECK_EQ(describe_pin('Y'), std::string("Y limit input"));
    CHECK_EQ(describe_pin('7'), std::string("input '7'"));
}

TEST(test_every_status_line_in_the_bench_records_parses) {
    const char* lines[] = {
        "<Idle|MPos:0.000,0.000,0.000|FS:0,0|Pn:P|WCO:0.000,0.000,0.000>",
        "<Alarm|MPos:0.000,0.000,0.000|FS:0,0>",
        "<Home|MPos:53.222,31.725,0.000|FS:300,0>",
        "<Alarm|MPos:53.222,0.000,0.000|FS:0,0|Pn:Y|Ov:100,100,100>",
        "<Alarm|MPos:21.720,31.795,0.000|FS:0,0|Pn:E>",
        "<Run|MPos:2.288,0.060,0.000|FS:61,0|Ov:100,100,100>",
        "<Run|MPos:0.000,0.000,0.000|FS:14,0|WCO:0.000,0.000,0.000>",
    };
    for (const char* l : lines) {
        Status s;
        CHECK(parse_status(l, s));
    }
}

// ---------------------------------------------------------------- test_link.py (the parts the machine uses)

struct Booted {
    TestClock     clock;
    SimulatedGrbl board;
    GrblLink      link;
    Booted(std::string pins = "", bool lock = false, int polls = 0) : board(pins, lock, polls), link(board, clock) {
        link.drain(0.02, 0.5, 0.05);
    }
};

TEST(test_command_returns_the_lines_before_ok) {
    Booted b;
    Reply  r = b.link.command("$I", 0.5);
    CHECK(r.ok);
    CHECK(r.lines.at(0).rfind("[VER:", 0) == 0);
    CHECK(b.board.received == std::vector<std::string> { "$I" });
}

TEST(test_error_reply_carries_the_code_and_a_description) {
    Booted b;
    Reply  r = b.link.command("$NOPE", 0.5);
    CHECK(!r.ok);
    CHECK_EQ(r.error_code(), 3);
    CHECK_EQ(r.describe_error(), "error:3 (" + describe_error(3) + ")");
}

TEST(test_status_shows_pressed_inputs_and_follows_release) {
    Booted b;
    CHECK(b.link.status(0.5).pins.empty());
    b.board.press('Y');
    b.board.press('P');
    CHECK_EQ(b.link.status(0.5).pins, std::string("PY"));
    b.board.release('Y');
    CHECK_EQ(b.link.status(0.5).pins, std::string("P"));
}

TEST(test_gcode_is_locked_out_while_in_alarm_until_unlock) {
    Booted b;
    b.board.alarm(1);
    CHECK_EQ(b.link.command("G0 X1", 0.5).error_code(), 9);
    CHECK(b.link.status(0.5).alarmed());
    CHECK(b.link.command("$X", 0.5).ok);
    CHECK(b.link.command("G0 X1", 0.5).ok);
    CHECK(b.link.unsolicited.contains_since(0, "ALARM:1"));
}

TEST(test_silent_board_raises_a_timeout_instead_of_hanging) {
    Booted b;
    b.board.silent = true;
    expect_throw<GrblTimeout>([&] { b.link.command("$I", 0.05); });
    expect_throw<GrblTimeout>([&] { b.link.status(0.05); });
}

TEST(test_late_answer_to_a_timed_out_command_is_not_taken_for_the_next_reply) {
    Booted b;
    b.board.silent = true;
    expect_throw<GrblTimeout>([&] { b.link.command("$$", 0.05); });
    b.board.silent = false;
    b.board.say("$0=10");
    b.board.say("ok");
    Reply r = b.link.command("$I", 0.5);
    CHECK(r.lines.at(0).rfind("[VER:", 0) == 0);
    CHECK(b.link.unsolicited.contains_since(0, "$0=10"));
}

// ---------------------------------------------------------------- test_machine.py

static const std::vector<std::string> PASS = { "G91", "G93", "G1 X2.000 F30.0000", "G1 X5.000 Y1.0500 F12.0000", "G94", "G0 Z-150.0000", "G90" };

struct Rig {
    TestClock     clock;
    SimulatedGrbl board;
    GrblLink      link;
    Machine       lathe;
    Rig(bool lock = false) : board("", lock, 3), link(board, clock), lathe(link, clock, 0, 0.2, 0) { link.drain(0.02, 0.5, 0.05); }
    std::vector<std::string> g01() {
        std::vector<std::string> v;
        for (auto& l : PASS) {
            if (l.rfind("G0 ", 0) == 0 || l.rfind("G1 ", 0) == 0) {
                v.push_back(l);
            }
        }
        return v;
    }
};

TEST(test_a_new_session_must_home_before_it_may_cut) {
    Rig r;
    CHECK(contains(expect_throw<MachineError>([&] { r.lathe.run(PASS); }), "not been homed"));
    CHECK(r.board.executed.empty());
}

TEST(test_homing_from_the_start_up_alarm_clears_the_need_to_home) {
    Rig r(true);
    r.link.realtime(RT_SOFT_RESET);
    r.link.drain(0.02, 0.5, 0.05);
    r.lathe.home();
    CHECK(r.board.homed && !r.lathe.needs_homing);
    CHECK_EQ(r.lathe.status().state, std::string("Idle"));
}

TEST(test_a_homing_cycle_that_never_finds_its_switch_is_reported_and_leaves_the_machine_unhomed) {
    Rig r;
    r.board.homing_fails = true;
    CHECK(contains(expect_throw<Stopped>([&] { r.lathe.home(); }), "alarm 9"));
    CHECK(r.lathe.needs_homing);
}

TEST(test_a_pass_is_sent_in_order_and_waited_out) {
    Rig r;
    r.lathe.home();
    r.lathe.run(PASS);
    CHECK(r.board.executed == r.g01());
    CHECK_EQ(r.lathe.status().state, std::string("Idle"));
}

TEST(test_a_line_the_controller_refuses_stops_the_pass_with_the_reason) {
    Rig r;
    r.lathe.home();
    CHECK(contains(expect_throw<MachineError>([&] { r.lathe.run({ "G91", "$BOGUS", "G1 X1.000 F60.0000" }); }), "error:3"));
    CHECK(r.board.executed.empty());
}

TEST(test_jogging_is_allowed_before_homing_for_setting_up) {
    Rig r;
    r.lathe.jog('Y', 1.0, 60);
    CHECK(r.board.executed == std::vector<std::string> { "$J=G91 Y1.0000 F60" });
}

TEST(test_the_stop_switch_during_a_pass_ends_it_and_demands_homing) {
    Rig r;
    r.lathe.home();
    int start      = r.board.polls;
    r.board.on_poll = [&](int n) {
        if (n - start == 6) {
            r.board.press('E');
        }
    };
    CHECK(contains(expect_throw<Stopped>([&] { r.lathe.run(PASS); }), "stop switch was pressed"));
    CHECK(r.lathe.needs_homing);
}

TEST(test_nothing_can_be_done_while_the_stop_switch_is_held) {
    Rig r;
    r.lathe.home();
    r.board.press('E');
    size_t before = r.board.received.size();
    std::vector<std::function<void()>> attempts = { [&] { r.lathe.run(PASS); }, [&] { r.lathe.home(); }, [&] { r.lathe.recover(); },
                                                    [&] { r.lathe.jog('Y', 1, 60); } };
    for (auto& a : attempts) {
        CHECK(contains(expect_throw<MachineError>(a), "top switch"));
    }
    CHECK_EQ(r.board.received.size(), before);  // not even tried on the controller
}

TEST(test_releasing_the_stop_switch_alone_does_not_bring_the_machine_back) {
    Rig r;
    r.lathe.home();
    r.board.press('E');
    r.board.release('E');
    expect_throw<MachineError>([&] { r.lathe.run(PASS); });
    CHECK(r.board.executed.empty());
}

TEST(test_after_a_stop_recover_then_home_is_the_only_way_back_to_cutting) {
    Rig r;
    r.lathe.home();
    r.board.press('E');
    r.board.release('E');
    r.lathe.recover();
    CHECK(contains(expect_throw<MachineError>([&] { r.lathe.run(PASS); }), "not been homed"));
    r.lathe.home();
    r.lathe.run(PASS);
    CHECK_EQ(r.board.executed.back(), std::string("G0 Z-150.0000"));
}

TEST(test_a_stop_during_homing_is_reported_as_a_stop) {
    Rig r;
    int start       = r.board.polls;
    r.board.on_poll = [&](int n) {
        if (n - start == 3) {
            r.board.press('E');
        }
    };
    expect_throw<Stopped>([&] { r.lathe.home(); });
    CHECK(r.lathe.needs_homing);
}

TEST(test_a_limit_switch_alarm_during_a_pass_is_named_and_demands_homing) {
    Rig r;
    r.lathe.home();
    int start       = r.board.polls;
    r.board.on_poll = [&](int n) {
        if (n - start == 6) {
            r.board.alarm(1);
        }
    };
    CHECK(contains(expect_throw<Stopped>([&] { r.lathe.run(PASS); }), "alarm 1: hard limit"));
    CHECK(r.lathe.needs_homing);
}

TEST(test_a_pause_keeps_the_pass_alive_and_resume_finishes_it) {
    Rig r;
    r.lathe.home();
    int start       = r.board.polls;
    r.board.on_poll = [&](int n) {
        // the realtime bytes go straight to the board, as hold() and resume() do
        if (n - start == 6) {
            uint8_t b = '!';
            r.board.write(&b, 1);
        }
        if (n - start == 41) {
            uint8_t b = '~';
            r.board.write(&b, 1);
        }
    };
    r.lathe.run(PASS);
    CHECK(!r.lathe.needs_homing);  // a pause is not a stop: position is kept
    CHECK_EQ(r.lathe.status().state, std::string("Idle"));
}

TEST(test_a_controller_that_stops_answering_is_reported_not_waited_on_forever) {
    Rig r;
    r.lathe.home();
    r.board.intercept = [](const std::string& line) { return line.rfind("G1", 0) == 0; };
    CHECK(contains(expect_throw<MachineError>([&] { r.lathe.run(PASS); }), "no answer"));
}

TEST(test_an_old_alarm_line_waiting_in_the_input_is_not_taken_for_a_new_stop) {
    // On the controller, FluidNC's broadcast lines pile up while the operator thinks. An alarm
    // that was already dealt with must not stop the next, unrelated command.
    Rig r;
    r.lathe.home();
    r.board.say("ALARM:1");  // left over from before; the machine is Idle now
    r.lathe.jog('Y', 1.0, 60);
    CHECK(r.board.executed.back() == "$J=G91 Y1.0000 F60");
    CHECK(!r.lathe.needs_homing);
}

// ---------------------------------------------------------------- test_session.py

class Script : public Ui {
public:
    explicit Script(std::vector<std::string> answers) : answers(std::move(answers)) {}
    void        say(const std::string& text) override { said.push_back(text); }
    // warnings are kept apart from messages: the screen beeps for these only
    void        warn(const std::string& text) override { warned.push_back(text); }
    bool        on_purpose = false;  // as after the main-page button or Stop during homing
    bool        stopped_on_purpose() override { return on_purpose; }
    std::vector<std::string> warned;
    bool        warned_of(const std::string& phrase) const {
        for (auto& l : warned) {
            if (contains(l, phrase)) {
                return true;
            }
        }
        return false;
    }
    bool        rotate = true;  // the screen always turns the tube in the cut; a test may not
    bool        rotation_on() override { return rotate; }
    double      ask_number(const std::string& prompt) override { return std::stod(next(prompt)); }
    bool        confirm(const std::string& prompt) override { return next(prompt) == "y"; }
    // "<prev>" stands for the keypad's Prev button.
    double ask_value(const std::string& prompt, double initial, bool can_go_back) override {
        initials.push_back(initial);
        std::string a = next(prompt);
        if (a == "<prev>") {
            CHECK(can_go_back);
            throw GoBack {};
        }
        return std::stod(a);
    }
    std::vector<double> initials;
    // OK / Return: choose() takes "ok" / "return" from the answers; while offered, poll_choice()
    // takes the next of `choices` at each look ("wait": nothing pressed yet). What each step's
    // two buttons say is kept in `buttons` as "ok | back".
    Choice choose(const std::string& prompt, const Labels& labels) override {
        buttons.push_back(labels.ok + " | " + labels.back);
        return next(prompt) == "ok" ? Choice::Ok : Choice::Return;
    }
    Choice choose_or_settings(const std::string& prompt, const Labels& labels) override {
        buttons.push_back(labels.ok + " | " + labels.back);
        std::string a = next(prompt);
        return a == "ok" ? Choice::Ok : a == "settings" ? Choice::Settings : Choice::Return;
    }
    void   offer(const std::string& prompt, const Labels& labels) override {
        buttons.push_back(labels.ok + " | " + labels.back);
        asked.push_back(prompt);
        offered++;
    }
    std::vector<std::string> buttons;
    Choice poll_choice() override {
        if (choices.empty()) {
            return Choice::None;
        }
        std::string c = choices.front();
        choices.erase(choices.begin());
        if (on_poll_choice) {
            on_poll_choice(c);
        }
        return c == "ok" ? Choice::Ok : c == "return" ? Choice::Return : c == "end" ? Choice::End : Choice::None;
    }
    void                                    withdraw() override { withdrawn++; }
    std::vector<std::string>                choices;
    std::function<void(const std::string&)> on_poll_choice;
    int                                     offered = 0, withdrawn = 0;
    bool        heard(const std::string& phrase) const {
        for (auto& l : said) {
            if (contains(l, phrase)) {
                return true;
            }
        }
        return false;
    }
    std::vector<std::string>                answers, said, asked;
    std::function<void(const std::string&)> on_ask;

private:
    std::string next(const std::string& prompt) {
        asked.push_back(prompt);
        if (on_ask) {
            on_ask(prompt);
        }
        if (answers.empty()) {
            throw Quit {};
        }
        std::string a = answers.front();
        answers.erase(answers.begin());
        return a;
    }
};

// depth step 1, cut feed 60, Y toward tube +1, Z toward A +1, jog/return 120, Z search 250, tube 60 rpm
static const CutSettings SETTINGS = { 1.0, 60, 1, 1, 120, 250, 60 };

struct SessionRig {
    TestClock     clock;
    SimulatedGrbl board;
    GrblLink      link;
    Machine       machine;
    Script        ui;
    Session       session;
    SessionRig(std::vector<std::string> answers, std::vector<std::string> choices = {}) :
        board("", false, 2), link(board, clock), machine(link, clock, 0, 0.2, 0), ui(std::move(answers)), session(machine, SETTINGS, ui) {
        ui.choices       = std::move(choices);
        board.switch_a_z = 180;  // switch A sits 180 mm along the tube from the start corner
        link.drain(0.02, 0.5, 0.05);
        // the length measurement: each look at the controller finds the tool 10 mm further
        ui.on_poll_choice = [this](const std::string&) {
            if (board.state == "Jog") {
                board.pos[2] += 10;
            }
        };
    }
    std::vector<std::string> lines_with(const std::string& part) {
        std::vector<std::string> v;
        for (auto& l : board.executed) {
            if (contains(l, part)) {
                v.push_back(l);
            }
        }
        return v;
    }
};

TEST(test_cut_moves_turn_the_tube_while_cutting_the_length_at_the_depth) {
    auto g = cut_gcode(3.0, 0.0, 60.0, SETTINGS);
    // 60 mm at 60 mm/min = 1 minute, 60 turns in that minute: feed sqrt(60^2 + 60^2) per minute
    // 2 mm back from the depth (not to Y 0, next to switch B), back to the start, in again, cut
    std::vector<std::string> want = { "G21 G90", "G1 Y1.000 F60.0", "G1 Z0.000 F60.0", "G1 Y3.000 F60.0",
                                      "G91",     "G1 X60.000 Z60.000 F84.9", "G90" };
    CHECK_EQ(g.size(), want.size());
    for (size_t i = 0; i < want.size(); i++) {
        CHECK_EQ(g[i], want[i]);
    }
}

TEST(test_each_round_steps_further_until_ok_then_the_length_then_the_cut_then_back) {
    // home; step 1 mm; depth: Return, Return, OK; length: nothing pressed twice, then OK
    SessionRig r({ "y", "1", "return", "return", "ok", "60", "60", "ok" }, { "wait", "wait", "ok" });
    CutResult  res = r.session.run();
    CHECK(r.board.homed);
    CHECK_EQ(res.rounds, 3);
    CHECK_NEAR(res.depth_mm, 3.0, 1e-9);
    auto steps = r.lines_with("G1 Y");
    CHECK(contains(steps.at(1), "G1 Y1.000"));  // after the move to the corner: 1, 2, 3 mm
    CHECK(contains(steps.at(2), "G1 Y2.000"));
    CHECK(contains(steps.at(3), "G1 Y3.000"));
    CHECK_NEAR(res.length_mm, 30.0, 1e-9);  // three looks at 10 mm each before OK
    CHECK(res.cut);
    CHECK_EQ(r.lines_with("G1 X").size(), size_t(1));  // the cut turns the tube
    CHECK(contains(r.lines_with("G1 X").at(0), "Z30.000"));
    CHECK_EQ(r.board.executed.back(), std::string("G1 Z0.000 F120.0"));  // back at the start corner
    CHECK(r.ui.heard("Finished: depth 3.000 mm (3 rounds), length 30.0 mm."));
}

TEST(test_each_step_names_its_buttons_for_what_they_do) {
    // 2026-11-27: "Deeper" rather than "Return" for one step further, and so on
    SessionRig r({ "y", "1", "return", "ok", "60", "60", "ok" }, { "wait", "ok" });
    r.session.run();
    std::vector<std::string> want = { "Depth OK | Deeper", "Depth OK | Deeper", "Stop here | Measure again", "Start cut | Change" };
    CHECK(r.ui.buttons == want);
}

TEST(test_return_during_the_length_goes_back_to_its_start_and_measures_again) {
    SessionRig r({ "y", "1", "ok", "60", "60", "ok" }, { "wait", "return", "wait", "ok" });
    CutResult  res = r.session.run();
    CHECK_EQ(r.lines_with("$J=G90").size(), size_t(2));        // measured twice
    CHECK(!r.lines_with("G1 Z0.000 F120.0").empty());          // back to the start in between
    CHECK_NEAR(res.length_mm, 20.0, 1e-9);                     // the second measurement
}

TEST(test_the_length_measurement_stops_at_the_far_end_it_is_given) {
    // after Locate the screen passes the measured C-to-A length as the farthest point
    CutSettings located = SETTINGS;
    located.z_search_mm = 30;
    SessionRig r({ "y", "1", "ok", "60", "60", "ok" }, { "wait", "wait", "wait", "wait", "wait" });
    Session    s(r.machine, located, r.ui);
    r.ui.on_poll_choice = [&](const std::string&) {
        if (r.board.state == "Jog") {
            r.board.pos[2] += 10;
            if (r.board.pos[2] >= 30) {  // the jog reaches its end and stops
                r.board.pos[2] = 30;
                r.board.state  = "Idle";
            }
        }
    };
    CutResult res = s.run();
    CHECK(contains(r.lines_with("$J=G90").at(0), "Z30.0000"));
    CHECK_NEAR(res.length_mm, 29.0, 1e-9);  // the far end counts as switch A: the cut stops 1 mm short
    CHECK(contains(r.lines_with("G1 X").at(0), "Z29.000"));
    CHECK(r.ui.warned.empty());
}

TEST(test_the_machine_waits_for_the_cutting_values_before_cutting) {
    // length OK, then: speed 120, feed 30, Return (change), speed 90, feed 30, OK
    SessionRig r({ "y", "1", "ok", "120", "30", "return", "90", "30", "ok" }, { "ok" });
    CutResult  res = r.session.run();
    CHECK_NEAR(res.tube_rpm, 90.0, 1e-9);
    CHECK_NEAR(res.cut_feed_mm_per_min, 30.0, 1e-9);
    CHECK_EQ(r.ui.initials.at(1), 60.0);  // the speed starts from config.yaml's value (0 is the step)
    CHECK_EQ(r.ui.initials.at(3), 120.0); // asked again: shows what was typed
    // 10 mm at 30 mm/min = 1/3 min, at 90 rev/min = 30 turns
    CHECK(contains(r.lines_with("G1 X").at(0), "X30.000 Z10.000"));
}

TEST(test_the_x_test_run_is_one_jog_short_enough_for_fluidnc_and_is_refused_in_alarm) {
    Rig r;
    r.lathe.spin(30);
    // At most 1000 turns: one jog of 100000 turns crawled and could not be stopped (2026-11-27)
    CHECK_EQ(r.board.executed.back(), std::string("$J=G91 G21 X1000.0000 F30"));
    r.lathe.jog_cancel();
    r.lathe.spin(10, -1);  // the X test runs it backward too
    CHECK_EQ(r.board.executed.back(), std::string("$J=G91 G21 X-1000.0000 F10"));
    r.lathe.jog_cancel();
    CHECK_EQ(r.lathe.status().state, std::string("Idle"));

    Rig locked(true);  // start-up alarm: the tube may not turn before homing
    CHECK(contains(expect_throw<MachineError>([&] { locked.lathe.spin(30); }), "alarm"));
}

static double gword(const std::string& line, char letter) {
    size_t i = line.find(std::string(" ") + letter);
    return i == std::string::npos ? 0 : std::atof(line.c_str() + i + 2);
}

TEST(test_a_cut_of_more_than_1000_turns_is_sent_as_equal_moves_that_add_up) {
    CutSettings slow         = SETTINGS;
    slow.cut_feed_mm_per_min = 2.0;
    slow.tube_rpm            = 200.0;
    // 50 mm at 2 mm/min = 25 minutes, at 200 rev/min = 5000 turns: five moves of 1000 turns
    auto g = cut_gcode(3.0, 0.0, 50.0, slow);
    CHECK_EQ(g.at(4), std::string("G91"));
    CHECK_EQ(g.back(), std::string("G90"));
    CHECK_EQ(g.size(), size_t(4 + 1 + 5 + 1));
    for (size_t i = 5; i < 10; i++) {
        CHECK_EQ(g[i], std::string("G1 X1000.000 Z10.000 F200.0"));
    }

    // 10.001 mm at 1 mm/min and 150 rev/min = 1500.15 turns: two moves, to the thousandth
    slow.cut_feed_mm_per_min = 1.0;
    slow.tube_rpm            = 150.0;
    for (int dir : { 1, -1 }) {
        slow.z_cut_direction = dir;
        auto   h             = cut_gcode(3.0, 0.0, 10.001, slow);
        double x = 0, z = 0;
        int    moves = 0;
        for (auto& l : h) {
            if (l.rfind("G1 X", 0) == 0) {
                moves++;
                CHECK(gword(l, 'X') <= MAX_TURNS_PER_MOVE);
                x += gword(l, 'X');
                z += gword(l, 'Z');
            }
        }
        CHECK_EQ(moves, 2);
        CHECK_NEAR(x, 1500.15, 1e-9);
        CHECK_NEAR(z, dir * 10.001, 1e-9);
    }
}

TEST(test_with_the_third_axis_off_the_cut_runs_without_turning_the_tube) {
    CutSettings off = SETTINGS;
    off.rotate      = false;
    auto g = cut_gcode(3.0, 0.0, 60.0, off);
    CHECK_EQ(g.at(5), std::string("G1 Z60.000 F60.0"));
    // a shallow depth backs off half of it, never to 0
    CHECK_EQ(cut_gcode(1.0, 0.0, 60.0, off).at(1), std::string("G1 Y0.500 F60.0"));

    SessionRig r({ "y", "1", "ok", "60", "60", "ok" }, { "ok" });
    r.ui.rotate   = false;
    CutResult res = r.session.run();
    CHECK(res.cut);
    CHECK(r.lines_with("G1 X").empty());
}

TEST(test_settings_on_the_depth_question_change_the_step_and_the_speed) {
    // round 1 (1 mm): Settings -> step 0.5, speed 30; then Return twice, OK: 1 + 0.5 + 0.5 = 2 mm
    SessionRig r({ "y", "1", "settings", "0.5", "30", "return", "return", "ok", "60", "60", "ok" }, { "ok" });
    CutResult  res = r.session.run();
    CHECK_EQ(res.rounds, 3);
    CHECK_NEAR(res.depth_mm, 2.0, 1e-9);
    auto steps = r.lines_with("G1 Y");
    CHECK(contains(steps.at(2), "G1 Y1.500 F30.0"));            // the new step at the new speed
    CHECK(contains(r.lines_with("$J=G90").at(0), "F30"));       // the length measurement too
    CHECK(r.ui.heard("Step 0.500 mm, speed 30 mm/min."));
}

TEST(test_switch_a_ends_the_length_there_and_the_cut_follows) {
    SessionRig r({ "y", "1", "ok", "60", "60", "ok" }, { "wait", "wait", "wait" });
    r.ui.on_poll_choice = [&](const std::string&) {
        if (r.board.state == "Jog") {
            r.board.pos[2] += 10;
            if (r.board.pos[2] >= 20) {  // switch A: FluidNC stops with alarm 18 (probe_hard_limit)
                r.board.state = "Alarm";
                r.board.say("ALARM:18");
            }
        }
    };
    CutResult res = r.session.run();
    CHECK_NEAR(res.length_mm, 19.0, 1e-9);  // 1 mm short of where A was touched
    CHECK(r.ui.heard("The length ends at switch A: 20.0 mm. The cut stops 1.0 mm before it: 19.0 mm."));
    CHECK_EQ(r.board.homings.size(), size_t(2));  // homed again before cutting
    CHECK(res.cut);
    CHECK(contains(r.lines_with("G1 X").at(0), "Z19.000"));
    CHECK(r.ui.warned.empty());  // switch A here is planned: no warning, no beep
}

TEST(test_without_an_alarm_number_switch_a_is_known_by_its_input) {
    SessionRig r({ "y", "1", "ok", "60", "60", "ok" }, { "wait", "wait", "wait" });
    r.ui.on_poll_choice = [&](const std::string&) {
        if (r.board.state == "Jog") {
            r.board.pos[2] += 10;
            if (r.board.pos[2] >= 20) {
                r.board.state = "Alarm";  // the ALARM line was lost; A is still pressed
                r.board.press('P');
            }
        }
    };
    r.board.on_poll = [&](int) {
        if (r.board.homings.size() == 2) {
            r.board.release('P');  // homing moved the tool off switch A
        }
    };
    CutResult res = r.session.run();
    CHECK(res.cut);
    CHECK(r.ui.warned.empty());
}

TEST(test_another_switch_during_the_length_is_a_stop_not_the_end_of_the_length) {
    SessionRig r({ "y", "1", "ok", "60", "60", "ok" }, { "wait", "wait", "wait" });
    r.ui.on_poll_choice = [&](const std::string&) {
        if (r.board.state == "Jog") {
            r.board.pos[2] += 10;
            if (r.board.pos[2] >= 20) {  // switch C (hard limit), not A
                r.board.press('Z');
                r.board.alarm(1);
            }
        }
    };
    std::string what = expect_throw<Stopped>([&] { r.session.run(); });
    CHECK(contains(what, "The length measurement was stopped: alarm 1"));
    CHECK(contains(what, "Pressed now: switch C."));
    CHECK(r.lines_with("G1 X").empty());  // nothing was cut
    CHECK(r.machine.needs_homing);
}

TEST(test_each_tube_starts_by_asking_the_step_shown_with_the_one_used_last) {
    // the step offered first is config.yaml's (1 mm); 0 and 20 are refused; 0.25 is taken
    SessionRig r({ "y", "0", "20", "0.25", "return", "ok", "60", "60", "ok" }, { "ok" });
    CutResult  res = r.session.run();
    CHECK_EQ(r.ui.initials.at(0), 1.0);
    CHECK(contains(r.ui.asked.at(1), "Step per round toward the tube"));
    CHECK(r.ui.heard("The step must be from 0.01 to 10 mm."));
    CHECK_EQ(res.rounds, 2);
    CHECK_NEAR(res.depth_mm, 0.5, 1e-9);
    auto steps = r.lines_with("G1 Y");
    CHECK(contains(steps.at(1), "G1 Y0.250"));
    CHECK(contains(steps.at(2), "G1 Y0.500"));
    CHECK_NEAR(r.session.depth_step(), 0.25, 1e-12);  // what the screen keeps for the next tube

    // the next tube: the screen passes the step used last as the first value shown
    CutSettings next = SETTINGS;
    next.depth_step_mm = r.session.depth_step();
    SessionRig r2({ "0.25", "ok", "60", "60", "ok" }, { "ok" });
    r2.machine.home();
    Session s2(r2.machine, next, r2.ui);
    s2.run();
    CHECK_EQ(r2.ui.initials.at(0), 0.25);
}

TEST(test_ending_the_session_during_the_length_stops_the_tool) {
    SessionRig r({ "y", "1", "ok" }, { "wait", "end" });
    bool       quit = false;
    try {
        r.session.run();
    } catch (const Quit&) {
        quit = true;
    }
    CHECK(quit);
    CHECK_EQ(r.board.state, std::string("Idle"));  // the jog was cancelled
    CHECK(r.lines_with("G1 X").empty());           // nothing was cut
}

TEST(test_a_switch_on_the_way_back_is_a_warning_and_the_session_ends_as_back) {
    SessionRig r({ "y", "1", "ok", "60", "60", "ok" }, { "ok" });
    r.board.intercept = [&](const std::string& line) {
        if (line == "G1 Z0.000 F120.0" && !r.lines_with("G1 X").empty()) {
            r.board.state = "Alarm";  // switch C touched before Z reached 0
            r.board.press('Z');
            r.board.say("ALARM:1");
            return true;
        }
        return false;
    };
    CutResult res = r.session.run();
    CHECK(res.cut);
    CHECK_EQ(r.ui.warned.size(), size_t(1));
    CHECK(r.ui.warned_of("On the way back to the start corner"));
    CHECK(r.ui.warned_of("alarm 1"));
    CHECK(r.ui.warned_of("Pressed now: switch C."));
}

TEST(test_a_stop_the_operator_made_is_a_message_not_a_warning) {
    SessionRig r({ "y", "1", "ok", "60", "60", "ok" }, { "ok" });
    r.ui.on_purpose   = true;  // e.g. Stop pressed on the status bar
    r.board.intercept = [&](const std::string& line) {
        if (line == "G1 Z0.000 F120.0" && !r.lines_with("G1 X").empty()) {
            r.board.alarm(3);
            return true;
        }
        return false;
    };
    r.session.run();
    CHECK(r.ui.warned.empty());
    CHECK(r.ui.heard("On the way back to the start corner"));
}

TEST(test_a_failed_homing_is_a_warning_and_may_be_tried_again) {
    // homing fails (switch not found), the operator declines to try again
    SessionRig r({ "y", "n" });
    r.board.homing_fails = true;
    bool quit            = false;
    try {
        r.session.run();
    } catch (const Quit&) {
        quit = true;
    }
    CHECK(quit);
    CHECK(r.ui.warned_of("alarm 9"));
    CHECK(contains(r.ui.asked.back(), "Try again?"));
}

TEST(test_the_pressed_switches_are_named_from_the_inputs) {
    Status s;
    CHECK_EQ(switch_note(s), std::string(""));
    s.pins = "Z";
    CHECK_EQ(switch_note(s), std::string(" Pressed now: switch C."));
    s.pins = "PY";
    CHECK_EQ(switch_note(s), std::string(" Pressed now: switch B and A."));
}

TEST(test_the_newest_alarm_after_a_mark_is_found) {
    Rig r;
    r.lathe.status();
    size_t mark = r.lathe.link.unsolicited.size();
    CHECK_EQ(r.lathe.last_alarm(mark), -1);
    r.board.alarm(1);
    r.board.say("ALARM:18");
    r.lathe.status();
    CHECK_EQ(r.lathe.last_alarm(mark), 18);
    CHECK(contains(r.lathe.stop_reason(r.lathe.status(), mark), "alarm 18: switch A"));
    CHECK_EQ(r.lathe.last_alarm(r.lathe.link.unsolicited.size()), -1);  // nothing newer
}

TEST(test_declining_to_home_ends_the_session_before_anything_moves) {
    SessionRig r({ "n" });
    bool       quit = false;
    try {
        r.session.run();
    } catch (const Quit&) {
        quit = true;
    }
    CHECK(quit);
    CHECK(r.board.executed.empty() && !r.board.homed);
}

TEST(test_a_machine_homed_by_locate_is_not_homed_again_by_the_job) {
    SessionRig r({ "1", "ok", "60", "60", "ok" }, { "ok" });
    r.machine.home();
    r.session.run();
    for (auto& p : r.ui.asked) {
        CHECK(!contains(p, "start corner now"));
    }
    CHECK_EQ(r.board.homings.size(), size_t(1));
}

TEST(test_settings_that_make_no_sense_are_refused) {
    CHECK(SETTINGS.problem().empty());
    CutSettings s = SETTINGS;
    s.depth_step_mm = 0;
    CHECK(!s.problem().empty());
    s = SETTINGS;
    s.cut_feed_mm_per_min = -1;
    CHECK(!s.problem().empty());
    s = SETTINGS;
    s.z_cut_direction = 0;
    CHECK(!s.problem().empty());
    s = SETTINGS;
    s.tube_rpm = 0;
    CHECK(!s.problem().empty());
    s = SETTINGS;
    s.switch_a_margin_mm = -1;
    CHECK(!s.problem().empty());
}

TEST(test_locating_homes_measures_c_to_a_and_goes_back_to_the_zero_point) {
    SessionRig r({});
    r.board.pos[1] = 40;  // somewhere in the middle before locating
    r.board.pos[2] = 120;
    double length  = locate(r.machine, SETTINGS, r.ui);
    CHECK_NEAR(length, 180.0, 1e-9);                     // switch A sits 180 mm from C in the rig
    CHECK_EQ(r.board.homings.size(), size_t(1));         // homed once: B, then C
    CHECK_EQ(r.board.executed.size(), size_t(4));
    CHECK_EQ(r.board.executed.at(0), std::string("G1 Y5.000 F120.0"));  // clear of switch B while Z travels
    CHECK(contains(r.board.executed.at(1), "G38.2 Z250.000"));
    CHECK_EQ(r.board.executed.at(2), std::string("G1 Z0.000 F120.0"));  // back: stops at 0
    CHECK_EQ(r.board.executed.at(3), std::string("G1 Y0.000 F120.0"));
    CHECK_NEAR(r.board.pos[1], 0.0, 1e-9);
    CHECK_NEAR(r.board.pos[2], 0.0, 1e-9);
    CHECK(!r.machine.needs_homing);
    CHECK(r.ui.heard("Switch A is 180.0 mm from switch C."));
}

TEST(test_locating_after_a_stop_recovers_the_controller_first) {
    // The stop input leaves FluidNC refusing homing until a reset; Locate used to need the
    // Reset button first (2026-11-27).
    SessionRig r({});
    r.board.press('E');
    r.board.release('E');
    double length = locate(r.machine, SETTINGS, r.ui);
    CHECK_NEAR(length, 180.0, 1e-9);
    CHECK_EQ(r.board.homings.size(), size_t(1));
    CHECK(!r.machine.needs_homing);
}

TEST(test_a_switch_on_the_way_back_from_locating_is_a_warning_and_the_length_is_kept) {
    SessionRig r({});
    r.board.intercept = [&](const std::string& line) {
        if (line == "G1 Z0.000 F120.0") {
            r.board.press('Z');  // switch C touched before Z reached 0: steps were lost
            r.board.alarm(1);
            return true;
        }
        return false;
    };
    double length = locate(r.machine, SETTINGS, r.ui);
    CHECK_NEAR(length, 180.0, 1e-9);
    CHECK(r.ui.warned_of("On the way back to the zero point"));
    CHECK(r.ui.warned_of("Pressed now: switch C."));
    CHECK(r.machine.needs_homing);
}

TEST(test_locating_stops_with_an_alarm_when_switch_a_is_never_touched) {
    SessionRig r({});
    r.board.switch_a_z = 1e9;
    bool stopped       = false;
    try {
        locate(r.machine, SETTINGS, r.ui);
    } catch (const Stopped&) {
        stopped = true;
    }
    CHECK(stopped);
}

// ---------------------------------------------------------------- main

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    int failed = 0;
    for (auto& t : tests()) {
        try {
            t.fn();
            std::printf("  ok    %s\n", t.name);
        } catch (const Failure& f) {
            failed++;
            std::printf("  FAIL  %s\n        %s\n", t.name, f.what.c_str());
        } catch (const std::exception& e) {
            failed++;
            std::printf("  FAIL  %s\n        unexpected exception: %s\n", t.name, e.what());
        } catch (...) {
            failed++;
            std::printf("  FAIL  %s\n        unexpected exception\n", t.name);
        }
    }
    std::printf("%zu tests, %d failed\n", tests().size(), failed);
    return failed ? 1 : 0;
}
