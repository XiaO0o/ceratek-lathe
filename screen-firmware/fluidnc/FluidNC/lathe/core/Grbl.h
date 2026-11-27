// Ceratek lathe screen: what the controller says, turned into values.
// C++ port of lathe/grbl.py (ceratek-lathe repository). No FluidNC or Arduino headers here,
// so this file also builds for the PC tests.
#pragma once

#include <string>
#include <vector>

namespace lathe {

    // Real-time command bytes. Never queued, never answered with "ok".
    constexpr char RT_STATUS      = '?';
    constexpr char RT_FEED_HOLD   = '!';
    constexpr char RT_CYCLE_START = '~';
    constexpr char RT_SOFT_RESET  = 0x18;
    constexpr char RT_JOG_CANCEL  = static_cast<char>(0x85);

    std::string describe_alarm(int code);
    std::string describe_error(int code);
    std::string describe_pin(char letter);

    // One real-time status report, e.g. <Idle|MPos:0.000,0.000,0.000|FS:0,0|Pn:Y>
    struct Status {
        std::string         raw;
        std::string         state;          // "Idle", "Run", "Hold", "Alarm", ...
        int                 substate = -1;  // -1: none
        bool                has_mpos = false;
        std::vector<double> mpos;
        bool                has_feed = false;
        double              feed     = 0;
        std::string         pins;  // letters of the Pn: field, e.g. "E" or "YZ"

        bool alarmed() const { return state == "Alarm"; }
        bool has_pin(char c) const { return pins.find(c) != std::string::npos; }
    };

    bool is_status_report(const std::string& line);

    // Parse a <...> report. Returns false (and leaves out untouched) if it is not a valid one,
    // where the Python raises ValueError.
    bool parse_status(const std::string& line, Status& out);

    std::string trim(const std::string& s);
    bool        starts_with_nocase(const std::string& s, const char* prefix);
    bool        is_digits(const std::string& s);

}  // namespace lathe
