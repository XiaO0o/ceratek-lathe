// Ceratek lathe screen: screens and the job task. See TouchUi.h.
#include "TouchUi.h"

#include "LatheScreen.h"
#include "Logging.h"
#include "Config.h"  // Y_AXIS, Z_AXIS

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <nvs.h>

namespace lathe {

    namespace {
        // The keypad's Prev button answers with this; no typed number can look like it.
        const char* const PREV_ANSWER = "<prev>";

        // RGB565 colours
        constexpr uint16_t BLACK      = 0x0000;
        constexpr uint16_t WHITE      = 0xFFFF;
        constexpr uint16_t GREY       = 0x8410;
        constexpr uint16_t DARK       = 0x2104;
        constexpr uint16_t BAR        = 0x18E3;
        constexpr uint16_t GREEN      = 0x07E0;
        constexpr uint16_t DARK_GREEN = 0x0320;
        constexpr uint16_t CYAN       = 0x07FF;
        constexpr uint16_t YELLOW     = 0xFFE0;
        constexpr uint16_t RED        = 0xF800;
        constexpr uint16_t DIM_RED    = 0xA000;  // older warnings in the message list
        constexpr uint16_t DARK_RED   = 0x6000;
        constexpr uint16_t BLUE_SEL   = 0x001F;

        constexpr int W     = 480;  // landscape
        constexpr int H     = 320;
        constexpr int BAR_H = 40;


        // Manual page: larger steps, for setting up away from the tube.
        const double      MANUAL_STEPS[4]      = { 0.1, 1.0, 5.0, 10.0 };
        const double      XTEST_RPM[4]         = { 2, 10, 30, 60 };
        const char* const XTEST_RPM_NAMES[4]   = { "2", "10", "30", "60" };
        const char* const MANUAL_STEP_NAMES[4] = { "0.1", "1", "5", "10" };

        constexpr size_t LOG_KEEP = 40;

        // A warning: a message that starts with this is drawn red, and the buzzer sounds
        // WARNING_BEEPS times. Nothing else sounds (no click on the buttons).
        const char* const WARNING_PREFIX = "WARNING: ";
        constexpr int     WARNING_BEEPS  = 3;
        constexpr int     BEEP_ON_MS     = 200;
        constexpr int     BEEP_OFF_MS    = 150;

        const char* TOUCH_HELP_PYTHON = "Bring the tool tip to the start of the cut, just touching the tube. Type";
        const char* TOUCH_HELP_SCREEN =
            "Bring the tool tip to the start of the cut, just touching the tube. Pick a step, then use the arrows. "
            "Use small steps near the tube. Press Done when it touches.";

        class Lock {
        public:
            explicit Lock(SemaphoreHandle_t m) : _m(m) { xSemaphoreTake(_m, portMAX_DELAY); }
            ~Lock() { xSemaphoreGive(_m); }

        private:
            SemaphoreHandle_t _m;
        };

        bool busy(const std::string& state) { return is_busy_state(state); }
    }  // namespace

    TouchUi::TouchUi(LatheScreen& owner) : _owner(owner) {
        _mutex        = xSemaphoreCreateMutex();
        _answer_ready = xSemaphoreCreateBinary();
        _commands     = xQueueCreate(4, sizeof(Command));
    }

    void TouchUi::start() {
        // Both on core 0 at priority 1, next to FluidNC's polling task. Core 1 runs FluidNC's
        // motion loop at priority 2 and is left alone.
        xTaskCreatePinnedToCore(screen_task, "lathescr", 8192, this, 1, nullptr, 0);
        xTaskCreatePinnedToCore(job_task, "lathejob", 12288, this, 1, nullptr, 0);
    }

    // ------------------------------------------------------------------ Ui (job task)

    void TouchUi::say(const std::string& text) {
        Lock l(_mutex);
        if (text.rfind(TOUCH_HELP_PYTHON, 0) == 0) {
            _log.push_back(TOUCH_HELP_SCREEN);  // the Python text explains typing; the screen has arrows
        } else {
            _log.push_back(text);
        }
        while (_log.size() > LOG_KEEP) {
            _log.pop_front();
        }
        _version++;
    }

    std::string TouchUi::wait_answer(Ask kind, const std::string& prompt, double initial, bool can_go_back) {
        {
            Lock l(_mutex);
            _ask         = kind;
            _prompt      = prompt;
            _initial     = initial;
            _can_go_back = can_go_back;
            _question++;
            _answer.clear();
            _quit = false;
            _version++;
            if (_abort) {
                _ask = Ask::None;
                throw Quit {};  // back to the main page: no more questions
            }
        }
        xSemaphoreTake(_answer_ready, portMAX_DELAY);
        std::string a;
        bool        quit;
        {
            Lock l(_mutex);
            a    = _answer;
            quit = _quit;
            _ask = Ask::None;
            // The operator has seen what an own stop (Stop during homing) did: a stop after
            // this answer, as in a homing tried again, is a new one.
            _operator_stop = false;
            _version++;
        }
        if (quit) {
            throw Quit {};
        }
        return a;
    }

    double TouchUi::ask_number(const std::string& prompt) {
        while (true) {
            std::string a   = wait_answer(Ask::Number, prompt);
            char*       end = nullptr;
            double      v   = std::strtod(a.c_str(), &end);
            if (!a.empty() && end == a.c_str() + a.size()) {
                return v;
            }
            say("Please type a number, for example 10.25");
        }
    }

    double TouchUi::ask_value(const std::string& prompt, double initial, bool can_go_back) {
        while (true) {
            std::string a = wait_answer(Ask::Number, prompt, initial, can_go_back);
            if (a == PREV_ANSWER) {
                throw GoBack {};
            }
            char*  end = nullptr;
            double v   = std::strtod(a.c_str(), &end);
            if (!a.empty() && end == a.c_str() + a.size()) {
                return v;
            }
            say("Please type a number, for example 10.25");
        }
    }

    bool TouchUi::confirm(const std::string& prompt) { return wait_answer(Ask::Confirm, prompt) == "y"; }

    Choice TouchUi::choose(const std::string& prompt, const Labels& labels) {
        {
            Lock l(_mutex);
            _with_settings = false;
            _labels        = labels;
        }
        std::string a = wait_answer(Ask::Choice, prompt);
        return a == "ok" ? Choice::Ok : Choice::Return;
    }

    Choice TouchUi::choose_or_settings(const std::string& prompt, const Labels& labels) {
        {
            Lock l(_mutex);
            _with_settings = true;
            _labels        = labels;
        }
        std::string a = wait_answer(Ask::Choice, prompt);
        {
            Lock l(_mutex);
            _with_settings = false;
        }
        return a == "ok" ? Choice::Ok : a == "settings" ? Choice::Settings : Choice::Return;
    }

    void TouchUi::offer(const std::string& prompt, const Labels& labels) {
        Lock l(_mutex);
        _ask          = Ask::Choice;
        _prompt       = prompt;
        _labels       = labels;
        _offered      = true;
        _offer_answer = Choice::None;
        _version++;
    }

    Choice TouchUi::poll_choice() {
        Lock   l(_mutex);
        Choice c      = _offer_answer;
        _offer_answer = Choice::None;
        return c;
    }

    bool TouchUi::aborted() {
        Lock l(_mutex);
        return _abort;
    }

    void TouchUi::warn(const std::string& text) {
        say(WARNING_PREFIX + text);
        Lock l(_mutex);
        _beep_request = WARNING_BEEPS;
        _stop_handled = true;  // the alarm now showing has had its warning
    }

    bool TouchUi::stopped_on_purpose() {
        Lock l(_mutex);
        return _abort || _operator_stop;
    }

    void TouchUi::withdraw() {
        Lock l(_mutex);
        _offered      = false;
        _offer_answer = Choice::None;
        _ask          = Ask::None;
        _version++;
    }

    // ------------------------------------------------------------------ motor speeds

    namespace {
        const char* SPEED_NS = "ceratek";
    }

    // The speeds from the Speeds page, kept in flash; config.yaml's values until one is saved.
    void TouchUi::load_speeds() {
        const CutSettings cfg = _owner.cut_settings();
        double            rpm = cfg.tube_rpm, feed = cfg.jog_feed_mm_per_min;
        nvs_handle_t      h;
        if (nvs_open(SPEED_NS, NVS_READONLY, &h) == ESP_OK) {
            uint32_t v;
            if (nvs_get_u32(h, "tube_rpm_x10", &v) == ESP_OK && v > 0) {
                rpm = v / 10.0;
            }
            if (nvs_get_u32(h, "move_feed_x10", &v) == ESP_OK && v > 0) {
                feed = v / 10.0;
            }
            if (nvs_get_u32(h, "depth_step_um", &v) == ESP_OK) {
                double step = v / 1000.0;
                if (step >= CutSettings::MIN_DEPTH_STEP_MM && step <= CutSettings::MAX_DEPTH_STEP_MM) {
                    _depth_step = step;
                }
            }
            nvs_close(h);
        }
        _tube_rpm  = rpm;
        _move_feed = feed;
    }

    // The step per round of the job that just ended, kept in flash so the next tube offers it
    // first. Called with nothing moving (flash writes pause the processor's cache).
    void TouchUi::remember_depth_step(double step) {
        if (!(step >= CutSettings::MIN_DEPTH_STEP_MM && step <= CutSettings::MAX_DEPTH_STEP_MM) ||
            std::fabs(step - _depth_step) < 1e-9) {
            return;
        }
        _depth_step = step;
        if (machine_moving()) {
            return;  // kept until power-off only
        }
        nvs_handle_t h;
        if (nvs_open(SPEED_NS, NVS_READWRITE, &h) != ESP_OK) {
            return;
        }
        nvs_set_u32(h, "depth_step_um", uint32_t(step * 1000 + 0.5));
        nvs_commit(h);
        nvs_close(h);
    }

    void TouchUi::save_speeds() {
        nvs_handle_t h;
        if (nvs_open(SPEED_NS, NVS_READWRITE, &h) != ESP_OK) {
            return;
        }
        nvs_set_u32(h, "tube_rpm_x10", uint32_t(_tube_rpm * 10 + 0.5));
        nvs_set_u32(h, "move_feed_x10", uint32_t(_move_feed * 10 + 0.5));
        nvs_commit(h);
        nvs_close(h);
    }

    // config.yaml's settings with the speeds of the Speeds page and the step used last.
    CutSettings TouchUi::settings_now() {
        CutSettings cfg         = _owner.cut_settings();
        cfg.tube_rpm            = _tube_rpm;
        cfg.jog_feed_mm_per_min = _move_feed;
        if (_depth_step > 0) {
            cfg.depth_step_mm = _depth_step;
        }
        return cfg;
    }

    // ------------------------------------------------------------------ job task

    void TouchUi::job_task(void* self) { static_cast<TouchUi*>(self)->job_loop(); }

    void TouchUi::job_loop() {
        ChannelPort port(_owner.inbox(), _owner.outbox());
        RtosClock   clock;
        // One link and one machine for everything the screen does, so that homing done on the
        // manual page counts for the next job, and a stop anywhere demands homing everywhere.
        GrblLink link(port, clock);
        Machine  machine(link, clock);
        _machine = &machine;  // lives as long as this task
        link.drain(0.3, 2.0, 0.3);  // whatever FluidNC said at start-up
        load_speeds();

        // Safety net for motor heating: whenever the screen goes back to waiting (a job ended
        // in any way, or a manual operation finished), switch the motors off if config.yaml
        // says they are released when idle (idle_ms below 255). See LatheScreen::release_motors.
        // Nothing moves; the next move enables the motors again.
        auto release_motors = [&]() { _owner.release_motors(); };

        // A stop that reached the screen: a WARNING (red, three beeps) unless the operator made
        // it. The switches pressed now are named, unless the text already does.
        auto tell_stop = [&](const std::string& text) {
            std::string note;
            if (text.find("Pressed now") == std::string::npos) {
                try {
                    note = switch_note(machine.status());
                } catch (const std::exception&) {
                }
            }
            report_stop(*this, text + note);
        };
        // An alarm still showing when an operation ends has been dealt with here: the idle
        // watch on the screen task must not warn about it a second time.
        auto mark_stop_handled = [&]() {
            try {
                if (machine.status().alarmed()) {
                    Lock l(_mutex);
                    _stop_handled = true;
                }
            } catch (const std::exception&) {
            }
        };

        while (true) {
            Command c;
            xQueueReceive(_commands, &c, portMAX_DELAY);
            {
                Lock l(_mutex);
                _operator_stop = false;  // a new operation: an earlier own stop no longer counts
            }
            if (!_owner.motion_allowed()) {
                Lock l(_mutex);  // the buttons are greyed out too; this is the second lock
                _busy = false;
                _version++;
                continue;
            }

            if (c.what == Do::XTest) {
                // The X motor alone, for finding why it does not turn: one turn either way, or
                // turning until Stop. In the start-up alarm the controller is unlocked first
                // (Y and Z still need Locate before any cut).
                try {
                    if (machine.status().alarmed()) {
                        machine.recover();
                        say("X test: controller unlocked (Y and Z still need Locate).");
                    }
                    Status before = machine.status();
                    if (before.state != "Idle") {
                        throw MachineError("X is still moving (" + before.state + "). Press Stop first.");
                    }
                    double x0     = before.has_mpos && !before.mpos.empty() ? before.mpos[0] : 0;
                    if (std::fabs(c.distance) <= 1.0) {
                        say(format("X test: %+.0f turn at %.0f rev/min...", c.distance, c.feed));
                        machine.jog('X', c.distance, c.feed);
                        Status after = machine.status();
                        double x1    = after.has_mpos && !after.mpos.empty() ? after.mpos[0] : x0;
                        say(format("Done: the controller counted %+.2f turn. Did the shaft make one full turn?", x1 - x0));
                    } else {
                        machine.spin(c.feed, c.distance < 0 ? -1 : 1);
                        say(format("X test: turning %s at %.0f rev/min. Stop ends it.", c.distance < 0 ? "backward" : "forward", c.feed));
                    }
                } catch (const Stopped& e) {
                    tell_stop(std::string("X test: ") + e.what());
                } catch (const std::exception& e) {
                    say(std::string("X test: ") + e.what());
                }
                mark_stop_handled();
                release_motors();
                Lock l(_mutex);
                _busy = false;
                _version++;
                continue;
            }

            if (c.what == Do::Speeds) {
                {
                    Lock l(_mutex);
                    _running = true;
                    _manual  = false;
                    _version++;
                }
                try {
                    const double x_max  = _owner.max_rate(X_AXIS);
                    const double yz_max = std::min(_owner.max_rate(Y_AXIS), _owner.max_rate(Z_AXIS));
                    double       rpm = _tube_rpm, feed = _move_feed;
                    int          i   = 0;
                    while (i < 2) {
                        try {
                            double v = i == 0 ? ask_value(format("Tube speed (X), rev/min. At most %.0f.", x_max), rpm, false)
                                              : ask_value(format("Travel speed of Y and Z, mm/min. At most %.0f.", yz_max), feed, true);
                            double top = i == 0 ? x_max : yz_max;
                            if (!(v > 0) || (top > 0 && v > top)) {
                                say(format("The value must be more than 0 and at most %.0f.", top));
                                continue;
                            }
                            (i == 0 ? rpm : feed) = v;
                            i++;
                        } catch (const GoBack&) {
                            i = 0;
                        }
                    }
                    _tube_rpm  = rpm;
                    _move_feed = feed;
                    save_speeds();
                    say(format("Saved: tube %.0f rev/min, Y and Z travel %.0f mm/min.", rpm, feed));
                } catch (const Quit&) {
                    say("Speeds not changed.");
                }
                {
                    Lock l(_mutex);
                    _running = false;
                    _busy    = false;
                    _ask     = Ask::None;
                    _version++;
                }
                continue;
            }

            if (c.what == Do::Job) {
                {
                    Lock l(_mutex);
                    _running = true;
                    _manual  = false;
                    _log.clear();
                    _version++;
                }
                say("New tube. Follow the questions.");
                // After Locate, switch A is where it was measured: the length measurement goes
                // no farther than that. The step per round offered first is the one used last.
                CutSettings cfg = settings_now();
                {
                    Lock l(_mutex);
                    if (_located && _z_length > 0) {
                        cfg.z_search_mm = _z_length;
                    }
                }
                Session session(machine, cfg, *this);
                try {
                    session.run();
                    say("Session finished.");
                } catch (const Quit&) {
                    say("Session ended by the operator.");
                } catch (const Stopped& e) {
                    // planned switch touches never get here: only stops nobody planned (or the operator's)
                    tell_stop(std::string("Session stopped: ") + e.what());
                } catch (const std::exception& e) {
                    say(std::string("Session stopped: ") + e.what());
                } catch (...) {
                    say("Session stopped by an unexpected error.");
                }
                mark_stop_handled();
                remember_depth_step(session.depth_step());
                release_motors();
                {
                    Lock l(_mutex);
                    _running = false;
                    _busy    = false;
                    _ask     = Ask::None;
                    _version++;
                }
                continue;
            }

            // One manual operation, under the same rules as in the guided job.
            try {
                switch (c.what) {
                    case Do::Locate: {
                        double length = locate(machine, settings_now(), *this);
                        Lock   l(_mutex);
                        _located  = true;
                        _z_length = length;
                        break;
                    }
                    case Do::Home:
                        if (machine.status().alarmed()) {
                            machine.recover();  // after a stop, as Locate does
                        }
                        say("Going to the start corner (switches B and C): the tool moves back first, then along the tube.");
                        machine.home();
                        say("At the start corner.");
                        break;
                    case Do::Recover: {
                        machine.recover();
                        {
                            Lock l(_mutex);
                            _located = false;  // the position is lost (Reset used to say so)
                            _version++;
                        }
                        _want_map_reset = true;
                        say("Recovered: the position is lost. Press Locate before cutting.");
                        break;
                    }
                    case Do::Jog: {
                        // Y and Z move at the set-up feed. (The turning tube axis is added later.)
                        // Once the position is known, a move stops at 0 and at the far end
                        // (Y: its travel; Z: switch A as measured by Locate, else its travel).
                        const CutSettings cfg      = settings_now();
                        double            distance = c.distance;
                        bool              located;
                        double            z_length;
                        {
                            Lock l(_mutex);
                            located  = _located;
                            z_length = _z_length;
                        }
                        if (!machine.needs_homing) {
                            Status s = machine.status();
                            if (s.has_mpos && s.mpos.size() >= 3) {
                                int    i     = c.axis == 'Y' ? 1 : 2;
                                int    sign  = c.axis == 'Y' ? cfg.y_toward_tube : cfg.z_cut_direction;
                                double far   = c.axis == 'Y' ? _owner.travel(Y_AXIS)
                                                             : (located && z_length > 0 ? z_length : _owner.travel(Z_AXIS));
                                double lo    = std::min(0.0, sign * far), hi = std::max(0.0, sign * far);
                                double to    = std::max(lo, std::min(hi, s.mpos[i] + distance));
                                distance     = to - s.mpos[i];
                            }
                        }
                        if (std::fabs(distance) < 0.0005) {
                            say(format("%c is at its end: not moved.", c.axis));
                            break;
                        }
                        machine.jog(c.axis, distance, cfg.jog_feed_mm_per_min);
                        say(format("Moved %c %+.3f mm.", c.axis, distance));
                        break;
                    }
                    default:
                        break;
                }
            } catch (const Stopped& e) {
                // a switch during a manual move, a failed homing or Locate (not Stop or Main)
                tell_stop(e.what());
            } catch (const std::exception& e) {
                say(e.what());
            }
            mark_stop_handled();
            release_motors();
            {
                Lock l(_mutex);
                _busy = false;
                _version++;
            }
        }
    }

    // From the screen task: hand one job or manual operation to the job task.
    void TouchUi::send_command(Do what, char axis, double distance, double feed) {
        {
            Lock l(_mutex);
            _abort = false;
        }
        {
            Lock l(_mutex);
            if (_running || _busy) {
                return;  // one thing at a time
            }
            _busy = true;  // grey the buttons at once, before the job task picks it up
            _version++;
        }
        Command c { what, axis, distance, feed };
        if (xQueueSend(_commands, &c, 0) != pdTRUE) {
            Lock l(_mutex);
            _busy = false;
            _version++;
        }
    }

    // ------------------------------------------------------------------ screen task

    void TouchUi::screen_task(void* self) { static_cast<TouchUi*>(self)->screen_loop(); }

    bool TouchUi::machine_moving() {
        LiveStatus live = _owner.live();
        return live.valid && busy(live.status.state);
    }

    void TouchUi::bring_up_display() {
        DisplayPins pins;
        pins.sck       = _owner._sck.getNative(Pin::Capabilities::Output | Pin::Capabilities::Native);
        pins.mosi      = _owner._mosi.getNative(Pin::Capabilities::Output | Pin::Capabilities::Native);
        pins.miso      = _owner._miso.getNative(Pin::Capabilities::Input | Pin::Capabilities::Native);
        pins.lcd_cs    = _owner._lcd_cs.getNative(Pin::Capabilities::Output | Pin::Capabilities::Native);
        pins.lcd_dc    = _owner._lcd_dc.getNative(Pin::Capabilities::Output | Pin::Capabilities::Native);
        pins.lcd_reset = _owner._lcd_reset.defined() ? _owner._lcd_reset.getNative(Pin::Capabilities::Output | Pin::Capabilities::Native) : -1;
        pins.touch_cs  = _owner._touch_cs.getNative(Pin::Capabilities::Output | Pin::Capabilities::Native);

        DisplayOptions options;
        options.lcd_spi_hz    = _owner._lcd_spi_hz;
        options.touch_spi_hz  = _owner._touch_spi_hz;
        options.rotation      = _owner._rotation;
        options.invert_colors = _owner._invert_colors;
        options.swap_red_blue = _owner._swap_red_blue;

        _lcd.configure(pins, options);
        _lcd.init();
        _lcd.setRotation(options.rotation);
        _lcd.fillScreen(BLACK);
        _lcd.setTextColor(WHITE, BLACK);
        _lcd.setTextDatum(lgfx::middle_center);
        _lcd.setFont(&lgfx::fonts::Font4);
        _lcd.drawString("Ceratek tube lathe", W / 2, H / 2 - 20);
        _lcd.setFont(&lgfx::fonts::Font2);
        _lcd.drawString("Starting...", W / 2, H / 2 + 16);

        // The picture is drawn: light the backlight. Pin::write(true) gives the active level
        // set in config.yaml (gpio.5 = high, gpio.5:low = low).
        if (_owner._backlight.defined()) {
            _owner._backlight.write(true);
            log_info("Lathe screen: backlight on (" << _owner._backlight.name() << ")");
        }

        // Touch held during start-up, or no saved calibration: calibrate now. Nothing moves
        // at start-up (FluidNC starts locked until homed), so the NVS write is safe.
        int32_t x, y;
        bool    held = false;
        for (int i = 0; i < 10; i++) {
            held = _lcd.getTouch(&x, &y) > 0;
            if (!held) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (held || !_lcd.load_calibration()) {
            _lcd.run_calibration();
            if (!machine_moving()) {
                bool ok = _lcd.save_calibration();
                log_info("Lathe screen: touch calibration " << (ok ? "saved" : "NOT saved"));
            }
        }
        _lcd.fillScreen(BLACK);
    }

    void TouchUi::screen_loop() {
        bring_up_display();
        log_info("Lathe screen: ready, free heap " << xPortGetFreeHeapSize());

        int     press_count = 0;
        bool    pressed     = false;
        int     down_id     = B_NONE;
        int32_t    last_x = 0, last_y = 0;
        TickType_t ignore_until = 0;

        while (true) {
            if (_want_calibration) {
                _want_calibration = false;
                if (!machine_moving()) {
                    _lcd.run_calibration();
                    _lcd.save_calibration();
                }
                _lcd.fillScreen(BLACK);
                _drawn_version = 0;
                _drawn_bar.clear();
            }

            if (_want_map_reset) {
                _want_map_reset = false;
                _trail.clear();
                _trail_drawn = 0;
                _have_pos    = false;  // the next position reported is drawn in the middle
                _pos_known   = false;
                _seg_on      = false;
                _anchor      = MapPoint { 500, 500 };
                _last_state.clear();
                _drawn_point = MapPoint { -1, -1 };
            }

            draw_status_bar(false);
            watch_idle_stops();
            update_beeper();
            uint32_t version;
            {
                Lock l(_mutex);
                version = _version;
            }
            if (version != _drawn_version) {
                draw_body();
                update_map(true);
                _drawn_version = version;
                // A new screen: a press that began on the old one must not act on the new one,
                // and a quick second tap must not answer the next question by accident.
                down_id      = B_NONE;
                ignore_until = xTaskGetTickCount() + pdMS_TO_TICKS(400);
            }

            update_map(false);

            // Stop on the X test page: if X is still moving 0.6 s after the jog cancel, the
            // controller is reset (stops every motor at once; Locate is needed again).
            if (_xt_stop_check && int32_t(xTaskGetTickCount() - _xt_stop_check) >= 0) {
                _xt_stop_check = 0;
                if (machine_moving()) {
                    {
                        Lock l(_mutex);
                        _operator_stop = true;  // this reset is the operator's Stop: no warning
                    }
                    uint8_t b = RT_SOFT_RESET;
                    _owner.outbox().push(&b, 1);
                    say("X test: X did not stop, so the controller was reset. Press Locate before cutting.");
                }
            }

            // Touch: a press counts after 2 matching reads (40 ms); a button fires on release
            // inside the same button, never on press.
            int32_t x, y;
            bool    raw = _lcd.getTouch(&x, &y) > 0;
            if (raw) {
                if (++press_count >= 2) {
                    last_x = x;
                    last_y = y;
                    if (!pressed) {
                        pressed = true;
                        bool too_soon = int32_t(xTaskGetTickCount() - ignore_until) < 0;
                        down_id       = too_soon ? B_NONE : hit(x, y);
                    }
                }
            } else {
                press_count = 0;
                if (pressed) {
                    pressed = false;
                    if (down_id != B_NONE && hit(last_x, last_y) == down_id) {
                        on_button(down_id);
                    }
                    down_id = B_NONE;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    // ------------------------------------------------------------------ drawing

    std::vector<std::string> TouchUi::wrap(const std::string& text, int width) {
        std::vector<std::string> lines;
        std::string              line, word;
        auto                     flush_word = [&]() {
            if (word.empty()) {
                return;
            }
            std::string candidate = line.empty() ? word : line + " " + word;
            if (_lcd.textWidth(candidate.c_str()) <= width || line.empty()) {
                line = candidate;
            } else {
                lines.push_back(line);
                line = word;
            }
            word.clear();
        };
        for (char c : text) {
            if (c == ' ') {
                flush_word();
            } else {
                word += c;
            }
        }
        flush_word();
        if (!line.empty()) {
            lines.push_back(line);
        }
        return lines;
    }

    int TouchUi::draw_wrapped(const std::string& text, int x, int y, int w, int max_lines) {
        auto lines = wrap(text, w);
        int  h     = _lcd.fontHeight();
        int  n     = 0;
        for (const auto& l : lines) {
            if (n >= max_lines) {
                break;
            }
            _lcd.drawString(l.c_str(), x, y + n * h);
            n++;
        }
        return n;
    }

    // The newest messages that fit, oldest at the top. The newest message is white, older grey;
    // a warning is red (older warnings a darker red).
    void TouchUi::draw_log(int x, int y, int w, int h) {
        std::deque<std::string> log;
        {
            Lock l(_mutex);
            log = _log;
        }
        _lcd.setFont(&lgfx::fonts::Font2);
        _lcd.setTextDatum(lgfx::top_left);
        int line_h = _lcd.fontHeight() + 1;
        int fit    = h / line_h;

        struct Line {
            std::string text;
            bool        newest;
            bool        warning;
        };
        std::vector<Line> lines;  // built newest first
        bool              newest = true;
        for (auto it = log.rbegin(); it != log.rend() && int(lines.size()) < fit; ++it) {
            bool warning = it->rfind(WARNING_PREFIX, 0) == 0;
            auto wrapped = wrap(*it, w);
            for (auto r = wrapped.rbegin(); r != wrapped.rend(); ++r) {
                lines.push_back({ *r, newest, warning });
            }
            lines.push_back({ "", false, false });  // a little space between messages
            newest = false;
        }
        while (int(lines.size()) > fit) {
            lines.pop_back();  // drop the oldest lines that do not fit
        }
        int row = 0;
        for (auto it = lines.rbegin(); it != lines.rend(); ++it, ++row) {
            uint16_t colour = it->warning ? (it->newest ? RED : DIM_RED) : (it->newest ? WHITE : GREY);
            _lcd.setTextColor(colour, BLACK);
            _lcd.drawString(it->text.c_str(), x, y + row * line_h);
        }
        _lcd.setTextColor(WHITE, BLACK);
    }

    void TouchUi::draw_button(const Button& b, bool pressed) {
        uint16_t fill = pressed ? BLUE_SEL : (b.fill ? b.fill : DARK);
        _lcd.fillRoundRect(b.x, b.y, b.w, b.h, 8, b.enabled ? fill : BLACK);
        _lcd.drawRoundRect(b.x, b.y, b.w, b.h, 8, b.enabled ? WHITE : GREY);
        _lcd.setTextDatum(lgfx::middle_center);
        _lcd.setFont(&lgfx::fonts::Font4);
        if (_lcd.textWidth(b.label.c_str()) > b.w - 8) {
            _lcd.setFont(&lgfx::fonts::Font2);
        }
        _lcd.setTextColor(b.enabled ? WHITE : GREY, b.enabled ? fill : BLACK);
        _lcd.drawString(b.label.c_str(), b.x + b.w / 2, b.y + b.h / 2);
        _lcd.setTextColor(WHITE, BLACK);
    }

    void TouchUi::add_button(int id, int x, int y, int w, int h, const std::string& label, bool enabled, uint16_t fill) {
        Button b { id, int16_t(x), int16_t(y), int16_t(w), int16_t(h), label, enabled, fill };
        _buttons.push_back(b);
        draw_button(b);
    }

    int TouchUi::hit(int32_t x, int32_t y) const {
        if (_menu_button.id != B_NONE && x >= _menu_button.x && x < _menu_button.x + _menu_button.w && y >= _menu_button.y &&
            y < _menu_button.y + _menu_button.h) {
            return _menu_button.id;
        }
        if (_bar_button.id != B_NONE && _bar_button.enabled && x >= _bar_button.x && x < _bar_button.x + _bar_button.w && y >= _bar_button.y &&
            y < _bar_button.y + _bar_button.h) {
            return _bar_button.id;
        }
        for (const auto& b : _buttons) {
            if (b.enabled && x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) {
                return b.id;
            }
        }
        return B_NONE;
    }

    void TouchUi::draw_status_bar(bool force) {
        LiveStatus    live    = _owner.live();
        const Status& s       = live.status;
        bool          stop    = live.valid && s.has_pin(STOP_PIN);

        // While the machine is in alarm (stop switch, failed homing, ended job), nothing can
        // move, so make sure the motors are off: ask again every 2 s (cheap, and FluidNC
        // ignores it when idle_ms is 255).
        if (live.valid && s.state == "Alarm") {
            TickType_t now = xTaskGetTickCount();
            if (now - _last_release >= pdMS_TO_TICKS(2000)) {
                _last_release = now;
                _owner.release_motors();
            }
        }
        std::string   state   = live.valid ? s.state : "--";
        char          pos[64] = "";
        if (live.valid && s.has_mpos && s.mpos.size() >= 3) {
            std::snprintf(pos, sizeof(pos), "Y%.2f Z%.2f", s.mpos[1], s.mpos[2]);  // X turns without end: not shown
        }

        int bar_id = B_NONE;
        if (live.valid && (s.state == "Run" || s.state == "Jog")) {
            bar_id = B_PAUSE;
        } else if (live.valid && s.state == "Hold") {
            bar_id = B_RESUME;
        } else if (live.valid && s.state == "Home") {
            // Homing keeps every motor powered (FluidNC's rule), and on the bench nothing
            // visible moves, so it can look stopped. Give a way to end it.
            bar_id = B_STOP;
        }

        uint32_t    beat = (xTaskGetTickCount() / pdMS_TO_TICKS(1000)) & 1;
        uint16_t    bg   = stop ? RED : BAR;
        bool off_main;
        {
            Lock l(_mutex);
            off_main = _running || _manual || _xtest;
        }
        std::string key  = state + "|" + pos + "|" + s.pins + "|" + std::to_string(bar_id) + "|" + (stop ? "S" : "") + (off_main ? "M" : "");
        if (!force && key == _drawn_bar) {
            // heartbeat only: a small square that blinks once a second while the screen task runs
            _lcd.fillRect(W - 6, 2, 4, 4, beat ? WHITE : bg);
            return;
        }
        _drawn_bar = key;

        _lcd.fillRect(0, 0, W, BAR_H, bg);
        _lcd.setTextDatum(lgfx::middle_left);
        if (stop) {
            _lcd.setFont(&lgfx::fonts::Font4);
            _lcd.setTextColor(WHITE, bg);
            _lcd.drawString("STOP SWITCH PRESSED", 8, BAR_H / 2);
        } else {
            uint16_t c = GREY;
            if (state == "Idle") {
                c = GREEN;
            } else if (state == "Run" || state == "Jog" || state == "Home") {
                c = CYAN;
            } else if (state == "Hold") {
                c = YELLOW;
            } else if (state == "Alarm") {
                c = RED;
            }
            _lcd.setFont(&lgfx::fonts::Font4);
            _lcd.setTextColor(c, bg);
            _lcd.drawString(state.c_str(), 8, BAR_H / 2);
            _lcd.setFont(&lgfx::fonts::Font2);
            _lcd.setTextColor(WHITE, bg);
            _lcd.drawString(pos, 104, BAR_H / 2 - 8);
            std::string inputs = "Inputs: " + (s.pins.empty() ? std::string("none") : s.pins);
            _lcd.setTextColor(GREY, bg);
            _lcd.drawString(inputs.c_str(), 104, BAR_H / 2 + 8);
        }
        // heartbeat: a small square that blinks once a second while the screen task runs
        _lcd.fillRect(W - 6, 2, 4, 4, beat ? WHITE : bg);

        _menu_button = Button {};
        if (off_main) {
            _menu_button = Button { B_MENU, int16_t(W - 190), 3, 82, int16_t(BAR_H - 6), "Main", true, DARK };
            draw_button(_menu_button);
        }
        _bar_button = Button {};
        if (bar_id != B_NONE) {
            const char* label = bar_id == B_PAUSE ? "Pause" : bar_id == B_RESUME ? "Resume" : "Stop";
            uint16_t    color = bar_id == B_STOP ? DARK_RED : DARK_GREEN;
            _bar_button       = Button { bar_id, int16_t(W - 104), 3, 92, int16_t(BAR_H - 6), label, true, color };
            draw_button(_bar_button);
        }
        _lcd.setTextColor(WHITE, BLACK);
    }

    void TouchUi::draw_body() {
        Ask         ask;
        std::string prompt;
        bool        running;
        {
            Lock l(_mutex);
            ask     = _ask;
            prompt  = _prompt;
            running = _running;
        }
        _buttons.clear();
        _map.on = false;
        _lcd.fillRect(0, BAR_H, W, H - BAR_H, BLACK);
        _lcd.setTextColor(WHITE, BLACK);

        const int top = BAR_H + 4;

        if (!running) {
            bool manual;
            {
                Lock l(_mutex);
                manual = _manual;
            }
            bool xtest;
            {
                Lock l(_mutex);
                xtest = _xtest;
            }
            if (manual) {
                draw_manual_page();
            } else if (xtest) {
                draw_xtest_page();
            } else {
                draw_home_page();
            }
            return;
        }

        switch (ask) {
            case Ask::None:
                draw_log(8, top, W - 232, H - top - 8);
                place_map(W - 216, top, 208, H - top - 8);
                break;

            case Ask::Choice: {
                draw_log(8, top, W - 232, H - top - 128);
                _lcd.setFont(&lgfx::fonts::Font2);
                _lcd.setTextDatum(lgfx::top_left);
                _lcd.setTextColor(YELLOW, BLACK);
                draw_wrapped(prompt, 8, H - 120, W - 232, 3);
                _lcd.setTextColor(WHITE, BLACK);
                bool   with_settings;
                Labels labels;  // the step's own words, e.g. "Deeper" / "Depth OK"
                {
                    Lock l(_mutex);
                    with_settings = _with_settings && !_offered;
                    labels        = _labels;
                }
                if (with_settings) {
                    add_button(B_RETURN, 8, H - 72, 76, 64, labels.back);
                    add_button(B_SETTINGS, 88, H - 72, 76, 64, "Settings");
                    add_button(B_OK, 168, H - 72, W - 392, 64, labels.ok, true, DARK_GREEN);
                } else {
                    add_button(B_RETURN, 8, H - 72, 108, 64, labels.back);
                    add_button(B_OK, 124, H - 72, W - 348, 64, labels.ok, true, DARK_GREEN);
                }
                place_map(W - 216, top, 208, H - top - 8);
                break;
            }

            case Ask::Confirm: {
                draw_log(8, top, W - 176, 150);
                place_map(W - 164, top, 156, 150);
                _lcd.setFont(&lgfx::fonts::Font2);
                _lcd.setTextDatum(lgfx::top_left);
                _lcd.setTextColor(YELLOW, BLACK);
                draw_wrapped(prompt, 8, 204, W - 16, 2);
                _lcd.setTextColor(WHITE, BLACK);
                add_button(B_NO, 8, 240, 200, 72, "No");
                add_button(B_YES, W - 208, 240, 200, 72, "Yes", true, DARK_GREEN);
                break;
            }

            case Ask::Number: {
                double   initial;
                bool     can_go_back;
                uint32_t question;
                {
                    Lock l(_mutex);
                    initial     = _initial;
                    can_go_back = _can_go_back;
                    question    = _question;
                }
                if (question != _entry_question) {
                    // A new question: start from the value given before, if there is one.
                    _entry_question = question;
                    _entry.clear();
                    if (!std::isnan(initial)) {
                        char text[24];
                        std::snprintf(text, sizeof(text), "%g", initial);
                        if (std::strlen(text) <= 9) {
                            _entry = text;
                        }
                    }
                }
                draw_log(8, top, 168, 120);
                _lcd.setFont(&lgfx::fonts::Font2);
                _lcd.setTextDatum(lgfx::top_left);
                _lcd.setTextColor(YELLOW, BLACK);
                draw_wrapped(prompt, 8, 172, 168, 4);
                // keypad: 3 columns x 4 rows, then OK
                const char* keys[12] = { "7", "8", "9", "4", "5", "6", "1", "2", "3", ".", "0", "<-" };
                const int   ids[12]  = { B_KEY_0 + 7, B_KEY_0 + 8, B_KEY_0 + 9, B_KEY_0 + 4, B_KEY_0 + 5, B_KEY_0 + 6,
                                         B_KEY_0 + 1, B_KEY_0 + 2, B_KEY_0 + 3, B_KEY_POINT, B_KEY_0,     B_KEY_BACK };
                for (int i = 0; i < 12; i++) {
                    int col = i % 3, row = i / 3;
                    add_button(ids[i], 184 + col * 98, top + row * 56, 92, 50, keys[i]);
                }
                if (can_go_back) {
                    add_button(B_PREV, 184, top + 4 * 56, 92, 48, "Prev");
                    add_button(B_KEY_OK, 282, top + 4 * 56, 190, 48, "OK", !_entry.empty() && _entry != ".", DARK_GREEN);
                } else {
                    add_button(B_KEY_OK, 184, top + 4 * 56, 288, 48, "OK", !_entry.empty() && _entry != ".", DARK_GREEN);
                }
                draw_entry();
                break;
            }

        }
    }

    // Home page: what happened last, and the ways in.
    void TouchUi::draw_home_page() {
        bool busy;
        {
            Lock l(_mutex);
            busy = _busy;
        }
        const int top = BAR_H + 4;
        draw_log(8, top, W - 176, 150);
        place_map(W - 164, top, 156, 150);
        _lcd.setFont(&lgfx::fonts::Font2);
        _lcd.setTextDatum(lgfx::top_left);
        bool allowed = _owner.motion_allowed();
        if (!allowed) {
            _lcd.setTextColor(YELLOW, BLACK);
            _lcd.drawString("Motion is off: allow_motion is false in config.yaml.", 8, 222);
        }
        _lcd.setTextColor(WHITE, BLACK);
        bool located;
        {
            Lock l(_mutex);
            located = _located;
        }
        if (allowed && !located) {
            _lcd.setTextColor(YELLOW, BLACK);
            _lcd.drawString("Press Locate first: the position is not known yet.", 8, 222);
        }
        _lcd.setTextColor(WHITE, BLACK);
        // What the operator needs, in the order of use (2026-11-27). Calibrate and Test X motor
        // are on the Manual page; Reset is gone (Locate recovers after a stop by itself).
        const int bw = (W - 16 - 3 * 6) / 4;
        add_button(B_LOCATE, 8, 244, bw, 68, busy ? "Working" : "Locate", allowed && !busy, located ? 0 : DARK_GREEN);
        add_button(B_START, 8 + (bw + 6), 244, bw, 68, "Start new tube", allowed && !busy && located, located ? DARK_GREEN : 0);
        add_button(B_SPEEDS, 8 + 2 * (bw + 6), 244, bw, 68, "Speeds", !busy);
        add_button(B_MANUAL, 8 + 3 * (bw + 6), 244, bw, 68, "Manual", !busy);
    }

    // X motor test page: the X motor alone, at a chosen speed.
    void TouchUi::draw_xtest_page() {
        bool busy;
        {
            Lock l(_mutex);
            busy = _busy;
        }
        const int  top = BAR_H + 4;
        const bool on  = !busy && _owner.motion_allowed();
        draw_log(8, top, 192, H - top - 8);
        _lcd.setFont(&lgfx::fonts::Font2);
        _lcd.setTextDatum(lgfx::top_left);
        _lcd.setTextColor(GREY, BLACK);
        _lcd.drawString("Speed, rev/min:", 208, top);
        for (int i = 0; i < 4; i++) {
            add_button(B_XT_RPM_0 + i, 208 + i * 67, top + 18, 63, 36, XTEST_RPM_NAMES[i], on, i == _xt_rpm_index ? BLUE_SEL : 0);
        }
        add_button(B_XT_PLUS1, 208, top + 62, 128, 42, "+1 turn", on);
        add_button(B_XT_MINUS1, 344, top + 62, 128, 42, "-1 turn", on);
        add_button(B_XT_RUN_PLUS, 208, top + 110, 128, 42, "Run +", on, DARK_GREEN);
        add_button(B_XT_RUN_MINUS, 344, top + 110, 128, 42, "Run -", on, DARK_GREEN);
        add_button(B_XT_STOP, 208, top + 158, 264, 42, "Stop", true, DARK_RED);
        add_button(B_BACK, 208, top + 232, 264, 36, busy ? "Working..." : "Back", true);
    }

    // Manual page: home, recover, and moves for setting up, under the same rules as a job; and the
    // service tools (the X motor test, touch calibration).
    void TouchUi::draw_manual_page() {
        bool busy;
        {
            Lock l(_mutex);
            busy = _busy;
        }
        const int  top = BAR_H + 4;
        const bool on  = !busy && _owner.motion_allowed();
        draw_log(8, top, 192, 104);
        place_map(8, top + 110, 192, H - top - 118);
        add_button(B_M_HOME, 208, top, 128, 42, "Home", on, DARK_GREEN);
        add_button(B_M_RECOVER, 344, top, 128, 42, "Recover", on);
        for (int i = 0; i < 4; i++) {
            add_button(B_M_STEP_0 + i, 208 + i * 67, top + 48, 63, 36, MANUAL_STEP_NAMES[i], on, i == _m_step_index ? BLUE_SEL : 0);
        }
        add_button(B_M_Y_TOWARD, 208, top + 90, 128, 42, "Y to tube", on);
        add_button(B_M_Y_AWAY, 344, top + 90, 128, 42, "Y away", on);
        add_button(B_M_Z_ALONG, 208, top + 138, 128, 42, "Z along", on);
        add_button(B_M_Z_BACK, 344, top + 138, 128, 42, "Z back", on);
        add_button(B_XTEST, 208, top + 186, 128, 40, "Test X motor", on);
        add_button(B_CALIBRATE, 344, top + 186, 128, 40, "Calibrate", !busy);
        add_button(B_BACK, 208, top + 232, 264, 36, busy ? "Working..." : "Back", !busy);
    }

    // ------------------------------------------------------------------ the map

    namespace {
        const size_t   TRAIL_MAX  = 800;     // points kept for the path
        const uint16_t MAP_FRAME  = 0x8410;  // grey
        const uint16_t MAP_TRAIL  = 0x05FF;  // light blue
        const uint16_t MAP_CROSS  = 0xFFE0;  // yellow
        const uint16_t MAP_TOOL   = 0xFFFF;  // white dot: the tool
        const uint16_t MAP_LABEL  = 0xC618;  // light grey
        const uint16_t MAP_MOVE   = 0xF800;  // red: the current move and its distance
    }

    void TouchUi::place_map(int x, int y, int w, int h) {
        _map         = MapRect { x, y, w, h, true };
        _drawn_point = MapPoint { -1, -1 };
    }

    // Where a position (mm) is on the map, as thousandths across (Y) and up (Z). Before
    // Locate the numbers have no fixed meaning, so the first position is put in the middle and
    // moves are drawn from there.
    TouchUi::MapPoint TouchUi::map_point(double y_mm, double z_mm) const {
        auto clamp = [](double v) {
            long t = std::lround(v * 1000.0);
            return int16_t(t < 0 ? 0 : t > 1000 ? 1000 : t);
        };
        if (_map_ty <= 0 || _map_tz <= 0) {
            return MapPoint { 500, 500 };
        }
        if (_pos_known) {
            return MapPoint { clamp(y_mm / _map_ty), clamp(z_mm / _map_tz) };
        }
        return MapPoint { clamp(_anchor.y / 1000.0 + (y_mm - _ref_y) / _map_ty),
                          clamp(_anchor.z / 1000.0 + (z_mm - _ref_z) / _map_tz) };
    }

    // Called every loop: follows the tool, splits its motion into moves (one axis at a time),
    // records the path once located, and redraws the map when something it shows has changed.
    void TouchUi::update_map(bool force) {
        LiveStatus live = _owner.live();
        bool       located;
        double     z_length;
        {
            Lock l(_mutex);
            located  = _located;
            z_length = _z_length;
        }
        if (located != _map_located) {
            _map_located = located;
            force        = true;
        }
        const CutSettings cfg = _owner.cut_settings();
        _map_ty               = _owner.travel(Y_AXIS);
        _map_tz               = (located && z_length > 0) ? z_length : _owner.travel(Z_AXIS);

        TickType_t now = xTaskGetTickCount();
        if (live.valid && live.status.has_mpos && live.status.mpos.size() >= 3) {
            double y = cfg.y_toward_tube * live.status.mpos[1];
            double z = cfg.z_cut_direction * live.status.mpos[2];
            if (!_have_pos) {
                _have_pos = true;
                _y_mm = _ref_y = y;
                _z_mm = _ref_z = z;
            }
            // Homing or an alarm makes the numbers untrue until a homing finishes: from then
            // on, draw moves from where the tool was last known. A homing that ends in Idle
            // makes them true again.
            const std::string& st = live.status.state;
            const bool homed = _machine && !_machine->needs_homing;
            if (_pos_known && (st == "Home" || st == "Alarm" || !homed)) {
                _anchor    = map_point(_y_mm, _z_mm);  // still in true positions here
                _ref_y     = _y_mm;
                _ref_z     = _z_mm;
                _pos_known = false;
                force      = true;
            } else if (!_pos_known && homed && (st == "Idle" || st == "Run" || st == "Jog") &&
                       (_last_state == "Home" || _last_state == "Idle" || _last_state == "Run" || _last_state == "Jog")) {
                _pos_known = true;
                _seg_on    = false;
                _trail.clear();
                force = true;
            }
            _last_state = st;
            double dy = std::fabs(y - _y_mm), dz = std::fabs(z - _z_mm);
            if (dy > 0.0005 || dz > 0.0005) {
                char axis = dy >= dz ? 'Y' : 'Z';
                // A new move: after a pause, or when the other axis starts moving.
                if (!_seg_on || axis != _seg_axis || now - _last_motion > pdMS_TO_TICKS(700)) {
                    _seg_on   = true;
                    _seg_axis = axis;
                    _seg_y0   = _y_mm;
                    _seg_z0   = _z_mm;
                }
                _last_motion = now;
                _y_mm        = y;
                _z_mm        = z;
            }
            // The red line and its distance show a move while it happens: a second after the
            // tool has stopped they go (2026-11-24: "5.000 mm" left at the zero point looked
            // like an offset).
            if (_seg_on && now - _last_motion > pdMS_TO_TICKS(1000) && st != "Run" && st != "Jog" && st != "Home") {
                _seg_on = false;
                force   = true;
            }
            if (_pos_known) {
                MapPoint p = map_point(_y_mm, _z_mm);
                if (_trail.empty() || _trail.back().y != p.y || _trail.back().z != p.z) {
                    if (_trail.size() >= TRAIL_MAX) {
                        _trail.erase(_trail.begin(), _trail.begin() + TRAIL_MAX / 4);  // drop the oldest quarter
                        _trail_drawn = 0;
                    }
                    _trail.push_back(p);
                }
            }
        }
        if (!_map.on) {
            return;
        }
        MapPoint p    = map_point(_y_mm, _z_mm);
        double   dist = _seg_on ? (_seg_axis == 'Y' ? std::fabs(_y_mm - _seg_y0) : std::fabs(_z_mm - _seg_z0)) : -1;
        bool     changed = p.y != _drawn_point.y || p.z != _drawn_point.z || std::fabs(dist - _drawn_dist) > 0.0005 ||
                       _trail.size() != _trail_drawn;
        if (!force && !changed) {
            return;
        }
        if (_map_sprite && (_sprite_w != _map.w || _sprite_h != _map.h)) {
            _map_sprite->deleteSprite();
            delete _map_sprite;
            _map_sprite = nullptr;
        }
        if (!_map_sprite) {
            _map_sprite = new lgfx::LGFX_Sprite(&_lcd);
            _map_sprite->setColorDepth(8);  // 1 byte a pixel: at most about 47 kB
            if (!_map_sprite->createSprite(_map.w, _map.h)) {
                delete _map_sprite;  // not enough memory: draw straight to the screen instead
                _map_sprite = nullptr;
            } else {
                _sprite_w = _map.w;
                _sprite_h = _map.h;
            }
        }
        if (_map_sprite) {
            draw_map(*_map_sprite, 0, 0, _map.w, _map.h);
            _map_sprite->pushSprite(_map.x, _map.y);
        } else {
            draw_map(_lcd, _map.x, _map.y, _map.w, _map.h);
        }
        _drawn_point = p;
        _drawn_dist  = dist;
        _trail_drawn = _trail.size();
    }

    void TouchUi::draw_map(lgfx::LovyanGFX& g, int ox, int oy, int w, int h) {
        g.fillRect(ox, oy, w, h, BLACK);
        // the frame, with room for the switch letters around it
        const int fx = ox + 14, fy = oy + 14, fw = w - 22, fh = h - 28;
        if (fw < 20 || fh < 20) {
            return;
        }
        g.drawRect(fx, fy, fw, fh, MAP_FRAME);
        g.setFont(&lgfx::fonts::Font0);
        g.setTextColor(MAP_LABEL, BLACK);
        g.setTextDatum(lgfx::middle_center);
        g.drawString("A", fx + fw / 2, oy + 7);      // far end along the tube
        g.drawString("C", fx + fw / 2, oy + h - 7);  // start end along the tube
        g.drawString("B", ox + 6, fy + fh / 2);      // tool fully back
        g.setTextDatum(lgfx::top_left);

        auto px = [&](int16_t t) { return fx + 1 + int32_t(t) * (fw - 3) / 1000; };
        auto py = [&](int16_t t) { return fy + fh - 2 - int32_t(t) * (fh - 3) / 1000; };

        // the path so far (only once located)
        for (size_t i = 1; i < _trail.size(); i++) {
            g.drawLine(px(_trail[i - 1].y), py(_trail[i - 1].z), px(_trail[i].y), py(_trail[i].z), MAP_TRAIL);
        }

        // cross lines through the tool (dashed while the position is not known)
        MapPoint p = map_point(_y_mm, _z_mm);
        int      x = px(p.y), y = py(p.z);
        if (_pos_known) {
            g.drawFastHLine(fx + 1, y, fw - 2, MAP_CROSS);
            g.drawFastVLine(x, fy + 1, fh - 2, MAP_CROSS);
        } else {
            for (int i = fx + 1; i < fx + fw - 1; i += 6) {
                g.drawFastHLine(i, y, 3, MAP_CROSS);
            }
            for (int i = fy + 1; i < fy + fh - 1; i += 6) {
                g.drawFastVLine(x, i, 3, MAP_CROSS);
            }
            g.setTextColor(MAP_CROSS, BLACK);
            g.setTextDatum(lgfx::middle_center);
            const char* note = _map_located ? "position lost: home again" : "not located";
            if (_last_state == "Home") {
                // homing: say which switch it is waiting for (the axis that is moving)
                note = _seg_on && _seg_axis == 'Z' ? "homing: going to switch C" : "homing: going to switch B";
            }
            g.drawString(note, fx + fw / 2, fy + fh - 10);
            g.setTextDatum(lgfx::top_left);
        }

        // the current (or last) move: a red line from where it started to the tool, an arrow
        // head at the tool, and how far it has gone, updated as it moves
        if (_seg_on) {
            MapPoint s  = map_point(_seg_y0, _seg_z0);
            int      sx = px(s.y), sy = py(s.z);
            double   dist;
            char     text[24];
            g.setTextColor(MAP_MOVE, BLACK);
            if (_seg_axis == 'Y') {
                dist = std::fabs(_y_mm - _seg_y0);
                g.drawFastHLine(std::min(sx, x), y - 1, std::abs(x - sx) + 1, MAP_MOVE);
                g.drawFastHLine(std::min(sx, x), y, std::abs(x - sx) + 1, MAP_MOVE);
                g.drawFastHLine(std::min(sx, x), y + 1, std::abs(x - sx) + 1, MAP_MOVE);
                int d = x >= sx ? 1 : -1;  // arrow head pointing the way it moves
                g.fillTriangle(x + 6 * d, y, x - 2 * d, y - 5, x - 2 * d, y + 5, MAP_MOVE);
                std::snprintf(text, sizeof(text), "%.3f mm", dist);
                int tw = g.textWidth(text);
                int tx = d > 0 ? x + 9 : x - 9 - tw;  // on the side it is moving toward
                if (tx < fx + 2) {
                    tx = fx + 2;
                }
                if (tx + tw > fx + fw - 2) {
                    tx = fx + fw - 2 - tw;
                }
                g.drawString(text, tx, y - 12 < fy + 2 ? y + 4 : y - 12);
            } else {
                dist = std::fabs(_z_mm - _seg_z0);
                g.drawFastVLine(x - 1, std::min(sy, y), std::abs(y - sy) + 1, MAP_MOVE);
                g.drawFastVLine(x, std::min(sy, y), std::abs(y - sy) + 1, MAP_MOVE);
                g.drawFastVLine(x + 1, std::min(sy, y), std::abs(y - sy) + 1, MAP_MOVE);
                int d = y <= sy ? -1 : 1;  // screen y grows downward; moving up is toward A
                g.fillTriangle(x, y + 6 * d, x - 5, y - 2 * d, x + 5, y - 2 * d, MAP_MOVE);
                std::snprintf(text, sizeof(text), "%.3f mm", dist);
                int tw = g.textWidth(text);
                int tx = x + 6;
                if (tx + tw > fx + fw - 2) {
                    tx = x - 6 - tw;
                }
                int ty = d < 0 ? y - 18 : y + 10;  // on the side it is moving toward
                if (ty < fy + 2) {
                    ty = fy + 2;
                }
                if (ty > fy + fh - 10) {
                    ty = fy + fh - 10;
                }
                g.drawString(text, tx, ty);
            }
        }
        g.fillCircle(x, y, 3, MAP_TOOL);
    }

    // The keypad's value box and the OK button, redrawn alone while typing.
    void TouchUi::draw_entry() {
        _lcd.fillRect(9, 249, 166, 62, BLACK);
        _lcd.drawRect(8, 248, 168, 64, WHITE);
        _lcd.setFont(&lgfx::fonts::Font4);
        _lcd.setTextColor(WHITE, BLACK);
        _lcd.setTextDatum(lgfx::middle_right);
        _lcd.drawString(_entry.empty() ? " " : _entry.c_str(), 168, 280);
        _lcd.setTextDatum(lgfx::top_left);
        for (auto& b : _buttons) {
            if (b.id == B_KEY_OK) {
                b.enabled = !_entry.empty() && _entry != ".";
                draw_button(b);
            }
        }
    }

    // ------------------------------------------------------------------ touches

    // The buzzer, from the screen task only: warn() asks for beeps; this plays them a step at a
    // time (on BEEP_ON_MS, off BEEP_OFF_MS), so neither task waits for the sound.
    void TouchUi::update_beeper() {
        int request;
        {
            Lock l(_mutex);
            request       = _beep_request;
            _beep_request = 0;
        }
        if (!_owner._beeper.defined()) {
            return;
        }
        if (request > _beeps_left) {
            _beeps_left = request;  // a new warning: three beeps (again), never piling up
        }
        if (!_beep_on && _beeps_left == 0) {
            return;
        }
        TickType_t now = xTaskGetTickCount();
        if (int32_t(now - _beep_next) < 0) {
            return;
        }
        if (_beep_on) {
            _owner._beeper.write(false);
            _beep_on   = false;
            _beep_next = now + pdMS_TO_TICKS(BEEP_OFF_MS);
        } else {
            _owner._beeper.write(true);
            _beep_on   = true;
            _beeps_left--;
            _beep_next = now + pdMS_TO_TICKS(BEEP_ON_MS);
        }
    }

    // A stop while no job or manual operation runs (the tube turning on its own, a switch
    // pressed by hand while Idle): the job task is not watching, so the screen warns. The
    // start-up alarm, an alarm already seen, and the job task's own stops are left alone.
    void TouchUi::watch_idle_stops() {
        LiveStatus live = _owner.live();
        if (!live.valid) {
            return;
        }
        bool alarm = live.status.state == "Alarm";
        bool first = !_watch_valid;
        bool was   = _watch_alarm;
        _watch_valid = true;
        _watch_alarm = alarm;
        if (!alarm) {
            if (was) {
                Lock l(_mutex);
                _stop_handled = false;  // that stop is over: the next one is new
            }
            return;
        }
        if (first || was) {
            return;
        }
        {
            Lock l(_mutex);
            bool quiet    = _running || _busy || _abort || _operator_stop || _stop_handled;
            _stop_handled = true;
            if (quiet) {
                return;  // the job task deals with it, or the operator made it
            }
        }
        warn("Stopped while no job or manual move was running." + switch_note(live.status) +
             " Press Locate (or Manual, Home) before going on.");
    }

    void TouchUi::answer(const std::string& text, bool quit) {
        {
            Lock l(_mutex);
            if (_ask == Ask::None) {
                return;  // nothing is waiting for an answer
            }
            _answer = text;
            _quit   = quit;
            _ask    = Ask::None;  // so a second touch cannot answer twice
            _version++;
        }
        xSemaphoreGive(_answer_ready);
    }

    void TouchUi::on_button(int id) {
        // no click: the buzzer is kept for warnings
        Ask ask;
        {
            Lock l(_mutex);
            ask = _ask;
        }
        if (id >= B_KEY_0 && id <= B_KEY_9) {
            if (_entry.size() < 9) {
                _entry += char('0' + (id - B_KEY_0));
            }
            draw_entry();
            return;
        }
        if (id >= B_M_STEP_0 && id <= B_M_STEP_3) {
            _m_step_index = id - B_M_STEP_0;
            for (auto& b : _buttons) {
                if (b.id >= B_M_STEP_0 && b.id <= B_M_STEP_3) {
                    b.fill = (b.id - B_M_STEP_0) == _m_step_index ? BLUE_SEL : 0;
                    draw_button(b);
                }
            }
            return;
        }
        switch (id) {
            case B_START:
                _trail.clear();  // the map shows this job's path only
                send_command(Do::Job);
                break;
            case B_LOCATE:
                _trail.clear();
                send_command(Do::Locate);
                break;
            case B_MENU: {
                // Back to the main page from anywhere: a moving or paused machine is stopped at
                // once (a reset: its position must be found again), a question is answered
                // with "end", and the session ends without doing anything more.
                bool running, manual;
                {
                    Lock l(_mutex);
                    _abort        = true;
                    running       = _running;
                    manual        = _manual;
                    _offer_answer = Choice::End;
                    _manual       = false;
                    if (_xtest) {
                        _xtest = false;
                        uint8_t b = RT_JOG_CANCEL;
                        _owner.outbox().push(&b, 1);
                    }
                    _version++;
                }
                LiveStatus  live  = _owner.live();
                std::string state = live.valid ? live.status.state : "";
                bool        busy;
                {
                    Lock l(_mutex);
                    busy = _busy;
                }
                if ((running || busy) && (state == "Run" || state == "Jog" || state == "Hold" || state == "Home")) {
                    uint8_t b = RT_SOFT_RESET;
                    _owner.outbox().push(&b, 1);
                }
                if (running) {
                    answer("", true);  // a question waiting: end the session
                }
                (void)manual;
                break;
            }
            case B_SETTINGS:
                answer("settings");
                break;
            case B_SPEEDS:
                send_command(Do::Speeds);
                break;
            case B_OK:
            case B_RETURN: {
                Choice c = id == B_OK ? Choice::Ok : Choice::Return;
                bool   offered;
                {
                    Lock l(_mutex);
                    offered = _offered;
                    if (offered) {
                        _offer_answer = c;
                    }
                }
                if (!offered) {
                    answer(id == B_OK ? "ok" : "return");
                }
                break;
            }
            case B_MANUAL:
            case B_BACK: {
                bool leaving_test;
                {
                    Lock l(_mutex);
                    leaving_test = _xtest;
                    if (leaving_test) {
                        _xtest  = false;  // Back from the X test always works, and stops X
                        _manual = true;   // back to the Manual page it was opened from
                        _version++;
                    } else if (!_busy) {
                        _manual = id == B_MANUAL;
                        _version++;
                    }
                }
                if (leaving_test) {
                    uint8_t b = RT_JOG_CANCEL;
                    _owner.outbox().push(&b, 1);
                    _xt_stop_check = xTaskGetTickCount() + pdMS_TO_TICKS(600);
                    if (_xt_stop_check == 0) {
                        _xt_stop_check = 1;
                    }
                }
                break;
            }
            case B_XTEST: {
                {
                    Lock l(_mutex);
                    if (_busy) {
                        break;
                    }
                    _manual = false;
                    _xtest  = true;
                    _version++;
                }
                say("X motor test. Pick a speed, then +1 turn (watch the shaft) or Run. Shakes but never turns, "
                    "even at 2: check the X motor wires (the two coil pairs). Turns slowly but shakes when fast: "
                    "speed too high or driver current too low. Does nothing at all: check the X-Motor plug and driver.");
                break;
            }
            case B_XT_RPM_0:
            case B_XT_RPM_0 + 1:
            case B_XT_RPM_0 + 2:
            case B_XT_RPM_3:
                _xt_rpm_index = id - B_XT_RPM_0;
                {
                    Lock l(_mutex);
                    _version++;
                }
                break;
            case B_XT_PLUS1:
            case B_XT_MINUS1:
            case B_XT_RUN_PLUS:
            case B_XT_RUN_MINUS:
                if (machine_moving()) {
                    say("X test: X is still turning. Press Stop first.");
                    break;
                }
                if (id == B_XT_RUN_PLUS || id == B_XT_RUN_MINUS) {
                    send_command(Do::XTest, 'X', id == B_XT_RUN_PLUS ? 2.0 : -2.0, XTEST_RPM[_xt_rpm_index]);
                    break;
                }
                send_command(Do::XTest, 'X', id == B_XT_PLUS1 ? 1.0 : -1.0, XTEST_RPM[_xt_rpm_index]);
                break;
            case B_XT_STOP: {
                uint8_t b = RT_JOG_CANCEL;  // ends a test turn or run at once (the position is kept)
                _owner.outbox().push(&b, 1);
                _xt_stop_check = xTaskGetTickCount() + pdMS_TO_TICKS(600);
                if (_xt_stop_check == 0) {
                    _xt_stop_check = 1;
                }
                say("X test: stop.");
                break;
            }
            case B_M_HOME:
                send_command(Do::Home);
                break;
            case B_M_RECOVER:
                send_command(Do::Recover);
                break;
            case B_M_Y_TOWARD:
            case B_M_Y_AWAY:
            case B_M_Z_ALONG:
            case B_M_Z_BACK: {
                // Directions from config.yaml, as in the guided job's touch-off.
                const CutSettings cfg  = _owner.cut_settings();
                bool              y    = id == B_M_Y_TOWARD || id == B_M_Y_AWAY;
                double            sign = (id == B_M_Y_TOWARD || id == B_M_Z_ALONG) ? 1.0 : -1.0;
                sign *= y ? cfg.y_toward_tube : cfg.z_cut_direction;
                send_command(Do::Jog, y ? 'Y' : 'Z', sign * MANUAL_STEPS[_m_step_index]);
                break;
            }
            case B_CALIBRATE:
                _want_calibration = true;
                break;
            case B_PAUSE: {
                uint8_t b = RT_FEED_HOLD;
                _owner.outbox().push(&b, 1);
                break;
            }
            case B_RESUME: {
                uint8_t b = RT_CYCLE_START;
                _owner.outbox().push(&b, 1);
                break;
            }
            case B_STOP: {
                // FluidNC ends homing with alarm 6 and releases the motors (idle_ms rule);
                // the job then asks "Try again?" as for any failed homing, without a warning.
                {
                    Lock l(_mutex);
                    _operator_stop = true;
                }
                uint8_t b = RT_SOFT_RESET;
                _owner.outbox().push(&b, 1);
                break;
            }
            case B_YES:
                answer("y");
                break;
            case B_NO:
                answer("n");
                break;
            case B_KEY_POINT:
                if (_entry.find('.') == std::string::npos && _entry.size() < 9) {
                    _entry += '.';
                }
                draw_entry();
                break;
            case B_KEY_BACK:
                if (!_entry.empty()) {
                    _entry.pop_back();
                }
                draw_entry();
                break;
            case B_PREV:
                if (ask == Ask::Number) {
                    _entry.clear();
                    answer(PREV_ANSWER);
                }
                break;
            case B_KEY_OK:
                if (ask == Ask::Number && !_entry.empty() && _entry != ".") {
                    std::string v = _entry;
                    _entry.clear();
                    answer(v);
                }
                break;
            default:
                break;
        }
    }

}  // namespace lathe
