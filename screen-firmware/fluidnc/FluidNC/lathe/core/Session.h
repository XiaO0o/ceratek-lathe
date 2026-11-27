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

    class Ui {
    public:
        virtual ~Ui()                                         = default;
        virtual void        say(const std::string& text)       = 0;
        virtual std::string ask(const std::string& prompt)     = 0;  // used for the touch moves
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
        virtual Choice choose(const std::string& prompt) = 0;
        // The same with a third button that opens the movement settings (returns Settings).
        virtual Choice choose_or_settings(const std::string& prompt) { return choose(prompt); }
        // OK / Return shown while the machine moves: offer() shows them, poll_choice() returns
        // None until one is pressed (each press once), withdraw() takes them away.
        virtual void   offer(const std::string& prompt) = 0;
        virtual Choice poll_choice()                    = 0;
        virtual void   withdraw()                       = 0;
        // The operator went back to the main page: the session must end without doing more.
        virtual bool aborted() { return false; }
        // The third axis (X, the tube) switch on the screen: does the tube turn during the cut?
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
        double _step      = 0;  // depth step per round, mm (the operator may change it)
        double _move_feed = 0;  // speed of the depth steps and the length measurement, mm/min
    };

    // Locating (the group's plan, 2026-11-24), separate from a tube job: home (switch B, then
    // switch C), travel along the tube to switch A to measure how far apart C and A are, then
    // go back to the zero point (bottom left): Z to 0, then Y to 0; reaching 0 ends it, and so
    // does a switch touched on the way. Returns the C-to-A distance (mm).
    // Throws Stopped or MachineError like the Machine calls it makes.
    double locate(Machine& machine, const CutSettings& settings, Ui& ui);

    // printf into a std::string
    std::string format(const char* fmt, ...);

}  // namespace lathe
