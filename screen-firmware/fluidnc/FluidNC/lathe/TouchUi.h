// Ceratek lathe screen: the touchscreen side of the guided sequence.
//
// The guided sequence (core/Session, a port of session.py) runs on its own task and talks to
// the operator only through the Ui interface: say, ask, ask_number, confirm. TouchUi turns
// each of those into a screen -- a message list, a yes/no question, a numeric keypad, or the
// jog pad for touching off -- and hands the operator's answer back.
#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cmath>
#include <deque>
#include <string>
#include <vector>

#include "Display.h"
#include "core/Session.h"

namespace lathe {

    class LatheScreen;

    class TouchUi : public Ui {
    public:
        explicit TouchUi(LatheScreen& owner);
        void start();

        // Ui, called on the job task
        void        say(const std::string& text) override;
        std::string ask(const std::string& prompt) override;
        double      ask_number(const std::string& prompt) override;
        double      ask_value(const std::string& prompt, double initial, bool can_go_back) override;
        Choice      choose(const std::string& prompt) override;
        Choice      choose_or_settings(const std::string& prompt) override;
        void        offer(const std::string& prompt) override;
        Choice      poll_choice() override;
        void        withdraw() override;
        bool        aborted() override;
        bool        rotation_on() override { return _x_on; }
        bool        confirm(const std::string& prompt) override;
        // A red WARNING line and three beeps (also called on the screen task, see watch_idle_stops)
        void        warn(const std::string& text) override;
        bool        stopped_on_purpose() override;

    private:
        enum class Ask { None, Number, Confirm, Move, Choice };

        enum ButtonId {
            B_NONE = 0,
            B_START,
            B_LOCATE,
            B_SPEEDS,
            B_RESET,
            B_OK,
            B_MENU,
            B_SETTINGS,
            B_X_AXIS,
            B_RETURN,
            B_CALIBRATE,
            B_PAUSE,
            B_RESUME,
            B_END,
            B_YES,
            B_NO,
            B_KEY_0,  // B_KEY_0 .. B_KEY_9 are consecutive
            B_KEY_9 = B_KEY_0 + 9,
            B_KEY_POINT,
            B_KEY_BACK,
            B_KEY_OK,
            B_PREV,  // keypad: back to the previous question
            B_STOP,  // status bar while homing: soft reset, which ends homing and frees the motors
            B_STEP_0,  // four step sizes, consecutive
            B_STEP_3 = B_STEP_0 + 3,
            B_Y_TOWARD,
            B_Y_AWAY,
            B_Z_ALONG,
            B_Z_BACK,
            B_DONE,
            // home page and manual page
            B_MANUAL,
            B_BACK,
            B_M_HOME,
            B_M_RECOVER,
            B_M_STEP_0,  // four manual step sizes, consecutive
            B_M_STEP_3 = B_M_STEP_0 + 3,
            B_M_Y_TOWARD,
            B_M_Y_AWAY,
            B_M_Z_ALONG,
            B_M_Z_BACK,
            B_XTEST,      // home page: the X motor test page
            B_XT_RPM_0,   // four test speeds, consecutive
            B_XT_RPM_3 = B_XT_RPM_0 + 3,
            B_XT_PLUS1,
            B_XT_MINUS1,
            B_XT_RUN_PLUS,
            B_XT_RUN_MINUS,
            B_XT_STOP,
        };

        // What the job task is asked to do: run a guided job, or one manual operation.
        enum class Do { Job, Locate, Home, Recover, Jog, Reset, SpinOn, SpinOff, Speeds, XTest };
        struct Command {
            Do     what;
            char   axis;
            double distance;
            double feed;
        };

        struct Button {
            int         id;
            int16_t     x, y, w, h;
            std::string label;
            bool        enabled;
            uint16_t    fill;
        };

        // job task
        static void job_task(void* self);
        void        job_loop();
        std::string wait_answer(Ask kind, const std::string& prompt, double initial = NAN, bool can_go_back = false);

        // screen task
        static void screen_task(void* self);
        void        screen_loop();
        void        bring_up_display();
        void        draw_status_bar(bool force);
        void        draw_body();
        void        draw_button(const Button& b, bool pressed = false);
        void        add_button(int id, int x, int y, int w, int h, const std::string& label, bool enabled = true, uint16_t fill = 0);
        int         hit(int32_t x, int32_t y) const;
        void        on_button(int id);
        void        answer(const std::string& text, bool quit = false);
        void        draw_log(int x, int y, int w, int h);
        void        draw_entry();
        int         draw_wrapped(const std::string& text, int x, int y, int w, int max_lines);
        std::vector<std::string> wrap(const std::string& text, int width);
        void        update_beeper();     // plays the beeps warn() asked for, without waiting
        void        watch_idle_stops();  // a stop while nothing runs: no job task sees it, so warn here
        bool        machine_moving();
        void        send_command(Do what, char axis = 0, double distance = 0, double feed = 0);
        void        draw_home_page();
        void        draw_manual_page();
        void        draw_xtest_page();
        void        place_map(int x, int y, int w, int h);
        void        update_map(bool force);
        void        draw_map(lgfx::LovyanGFX& g, int ox, int oy, int w, int h);
        struct MapPoint;
        MapPoint    map_point(double y_mm, double z_mm) const;

        LatheScreen& _owner;
        Display      _lcd;

        // shared between the tasks, under _mutex
        SemaphoreHandle_t       _mutex;
        SemaphoreHandle_t       _answer_ready;
        QueueHandle_t           _commands;
        std::deque<std::string> _log;
        uint32_t                _version  = 1;  // rises with every change the body shows
        Ask                     _ask      = Ask::None;
        std::string             _prompt;
        std::string             _answer;
        bool                    _quit     = false;
        double                  _initial  = NAN;    // keypad: value shown when the question opens
        bool                    _can_go_back = false;  // keypad: show the Prev button
        uint32_t                _question = 0;      // rises with every question asked
        bool                    _running  = false;  // a guided job is in progress
        bool                    _busy     = false;  // a manual operation is in progress
        bool                    _manual   = false;  // the manual page is shown
        bool                    _xtest    = false;  // the X motor test page is shown
        int                     _xt_rpm_index = 1;
        TickType_t              _xt_stop_check = 0;  // after Stop: when to check that X really stopped
        bool                    _located  = false;  // Locate has finished since power-up
        bool                    _offered     = false;   // OK / Return shown while the machine moves
        bool                    _with_settings = false; // the OK / Return question also has Settings
        bool                    _abort       = false;   // the main-page button was pressed
        Choice                  _offer_answer = Choice::None;
        double                  _z_length = 0;      // measured switch C to switch A, mm
        int                     _beep_request  = 0;      // beeps asked for by warn(), not yet started
        bool                    _operator_stop = false;  // Stop during homing / X test reset: the next stop is no warning
        bool                    _stop_handled  = false;  // the alarm now showing has been dealt with (warned, or planned)
        bool                    _spin_active   = false;  // the tube turns on its own (X: ON); set by the job task
        bool                    _spin_stop     = false;  // Pause while it does: the job task makes sure it stopped

        // screen task only
        std::vector<Button> _buttons;      // body buttons
        Button              _bar_button {};  // Pause / Resume / End, in the status bar
        Button              _menu_button {};  // "Main": back to the main page, always there off it
        Button              _x_button {};     // third axis (X, the tube) on / off: always there
        volatile bool       _x_on = false;   // off at power-up: the tube turns only when asked
        // Motor speeds, set on the Speeds page (kept in flash): tube (X) rev/min and the Y / Z
        // travel speed mm/min. 0 until the job task has read them.
        volatile double     _tube_rpm  = 0;
        volatile double     _move_feed = 0;
        // The depth step per round used last (kept in flash), offered first for the next tube;
        // 0: config.yaml's depth_step_mm. Job task only.
        double              _depth_step = 0;
        bool                _spinning  = false;  // job task only: the tube turns on its own (X: ON)
        void                load_speeds();
        void                save_speeds();
        void                remember_depth_step(double step);
        CutSettings         settings_now();
        // buzzer and the idle watch, screen task only
        int                 _beeps_left  = 0;
        bool                _beep_on     = false;
        TickType_t          _beep_next   = 0;
        bool                _watch_valid = false;  // a status has been seen
        bool                _watch_alarm = false;  // the last status seen was Alarm
        uint32_t            _drawn_version = 0;
        std::string         _drawn_bar;
        TickType_t          _last_release = 0;  // last motor release asked for in alarm
        std::string         _entry;        // keypad value
        uint32_t            _entry_question = 0;  // the question _entry belongs to
        int                 _step_index   = 1;  // touch-off jog step, index into STEPS
        int                 _m_step_index = 1;  // manual jog step, index into MANUAL_STEPS
        bool                _want_calibration = false;
        volatile bool       _want_map_reset   = false;  // set by the job task after Reset

        // The map: a rectangle for the frame (Y across, from switch B on the left toward the
        // tube on the right; Z up, from switch C at the bottom to switch A at the top), cross
        // lines at the tool's position, and the path it has taken.
        struct MapRect {
            int  x = 0, y = 0, w = 0, h = 0;
            bool on = false;
        };
        struct MapPoint {
            int16_t y, z;  // position as thousandths of each axis's travel
        };
        MapRect               _map;
        std::vector<MapPoint> _trail;               // newest last, at most TRAIL_MAX (only once located)
        bool                  _map_located = false;
        double                _map_ty = 0, _map_tz = 0;   // mm that the frame stands for, across and up
        bool                  _have_pos = false;          // a position has been reported
        double                _y_mm = 0, _z_mm = 0;       // tool, mm (from switch B / C once located)
        // The controller's numbers are true positions only after a homing that no alarm has
        // undone since. Otherwise moves are drawn from _anchor (the middle at power-up, or where
        // the tool was last known), measured from _ref_y/_ref_z.
        bool                  _pos_known = false;
        Machine* volatile     _machine   = nullptr;  // the job task's machine (read: has it homed since its last stop?)
        std::string           _last_state;
        double                _ref_y = 0, _ref_z = 0;
        MapPoint              _anchor { 500, 500 };
        // The current move: where it started, along which axis, and when the tool last moved.
        bool                  _seg_on = false;
        char                  _seg_axis = 0;              // 'Y' or 'Z'
        double                _seg_y0 = 0, _seg_z0 = 0;
        TickType_t            _last_motion = 0;
        // What the map last showed, to redraw only on a change.
        MapPoint              _drawn_point { -1, -1 };
        double                _drawn_dist  = -1;
        size_t                _trail_drawn = 0;
        lgfx::LGFX_Sprite*    _map_sprite  = nullptr;  // drawn off screen, then copied: no flicker
        int                   _sprite_w = 0, _sprite_h = 0;
    };

}  // namespace lathe
