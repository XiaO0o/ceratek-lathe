// Ceratek lathe screen: the operator's guided sequence for one tube. The steps are described in
// Plan.h (the group's plan, 2026-11-24). Everything the operator sees and answers goes through a
// small Ui object: on the controller the touchscreen (TouchUi), in the PC tests a scripted list.
#pragma once

#include <string>
#include <vector>

#include "Machine.h"
#include "Plan.h"

namespace lathe {

    // The operator chose to end the session.
    struct Quit {};

    // The operator chose to go back to the previous question (only where a question allows it).
    struct GoBack {};

    // The two buttons of each step: OK (this is right) and Return (not right: the step's other way).
    // End: the operator ended the session while the buttons were offered.
    enum class Choice { None, Ok, Return, End, Settings };

    // What a step's two buttons say on the screen (2026-11-27: "Deeper" rather than "Return").
    struct Labels {
        std::string ok   = "OK";
        std::string back = "Return";
    };

    class Ui {
    public:
        virtual ~Ui()                                         = default;
        virtual void        say(const std::string& text)       = 0;
        virtual double      ask_number(const std::string& prompt) = 0;
        virtual bool        confirm(const std::string& prompt)    = 0;

        // A number for one of a list of questions: shown pre-filled with `initial` (unless it is
        // NaN), and, when `can_go_back`, with a way back to the previous question, which throws
        // GoBack. A Ui without those features just asks the number.
        virtual double ask_value(const std::string& prompt, double initial, bool can_go_back) {
            (void)initial;
            (void)can_go_back;
            return ask_number(prompt);
        }

        // OK / Return, waiting for the answer (throws Quit if the session is ended instead).
        virtual Choice choose(const std::string& prompt, const Labels& labels) = 0;
        // The same with a third button that opens the movement settings (returns Settings).
        virtual Choice choose_or_settings(const std::string& prompt, const Labels& labels) { return choose(prompt, labels); }
        // OK / Return shown while the machine moves: offer() shows them, poll_choice() returns
        // None until one is pressed (each press once), withdraw() takes them away.
        virtual void   offer(const std::string& prompt, const Labels& labels) = 0;
        virtual Choice poll_choice()                    = 0;
        virtual void   withdraw()                       = 0;
        // The operator went back to the main page: the session must end without doing more.
        virtual bool aborted() { return false; }
        // A stop nobody planned: a switch touched where none should be (during the depth steps,
        // the cut, the way back, a manual move), a failed homing. Planned switch touches
        // (homing at B and C, Locate and the length measurement at A) are messages, not this.
        // On the touchscreen: red, with the buzzer. A Ui without it shows it as a message.
        virtual void warn(const std::string& text) { say("WARNING: " + text); }
        // The operator stopped the machine (the main-page button, Stop during homing): the stop
        // that follows is a message, not a warning.
        virtual bool stopped_on_purpose() { return aborted(); }
        // Does the tube (X) turn during the cut? The screen always says yes (2026-11-27); a test
        // may say no.
        virtual bool rotation_on() { return true; }
    };

    struct CutResult {
        bool   cut      = false;  // the cut was made
        int    rounds   = 0;      // depth rounds until OK
        double depth_mm = 0;      // from the start corner toward the tube
        double length_mm = 0;     // along the tube
        double start_z_mm = 0;    // where the length starts (machine Z)
        double tube_rpm = 0;      // chosen by the operator before the cut
        double cut_feed_mm_per_min = 0;
    };

    class Session {
    public:
        Session(Machine& machine, const CutSettings& settings, Ui& ui) : machine(machine), settings(settings), ui(ui) {}

        CutResult run();
        // The step per round the operator chose for this tube (or changed in Settings), for the
        // screen to offer it again next time; 0 if the job ended before the step was asked.
        double depth_step() const { return _step_chosen ? _step : 0; }

        Machine&    machine;
        CutSettings settings;
        Ui&         ui;
        CutResult   result;

    private:
        void   make_ready();
        void   movement_settings();
        void   find_depth();
        void   find_length();
        void   cut_parameters();
        void   cut();
        void   go_back();
        double z_now();
        bool   step_ok(double step);  // says why not when it is not
        double _step      = 0;  // depth step per round, mm (the operator may change it)
        bool   _step_chosen = false;  // the operator answered the step question in this job
        double _move_feed = 0;  // speed of the depth steps and the length measurement, mm/min
    };

    // Locating (the group's plan, 2026-11-24), separate from a tube job: home (switch B, then
    // switch C), travel along the tube to switch A to measure how far apart C and A are, then
    // go back to the zero point (bottom left): Z to 0, then Y to 0; reaching 0 ends it, and so
    // does a switch touched on the way (a warning). Returns the C-to-A distance (mm).
    // Throws Stopped or MachineError like the Machine calls it makes.
    double locate(Machine& machine, const CutSettings& settings, Ui& ui);

    // " Pressed now: switch C." from a status report's Pn field (Y = B, Z = C, P = A), or "".
    std::string switch_note(const Status& s);

    // A stop, shown as a warning unless the operator made it (Ui::stopped_on_purpose).
    void report_stop(Ui& ui, const std::string& text);

    // printf into a std::string
    std::string format(const char* fmt, ...);

}  // namespace lathe
