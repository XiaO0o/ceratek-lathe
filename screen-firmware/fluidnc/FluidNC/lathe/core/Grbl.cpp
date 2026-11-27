// C++ port of lathe/grbl.py. See Grbl.h.
#include "Grbl.h"

#include <cctype>
#include <cstdlib>

namespace lathe {

    namespace {
        struct CodeText {
            int         code;
            const char* text;
        };

        // Alarm codes 1-9 are defined by Grbl 1.1. Higher numbers are firmware-specific: 10-18
        // are FluidNC v4.1.1's (src/Alarm.h). On this lathe 1 is switch B or C and 18 is switch A.
        const CodeText ALARMS[] = {
            { 1, "hard limit triggered; position is lost, re-home" },
            { 2, "soft limit: target is outside the machine travel" },
            { 3, "reset while moving; position is lost, re-home" },
            { 4, "probe fail: probe was already triggered before the cycle" },
            { 5, "probe fail: no contact within the programmed travel" },
            { 6, "homing fail: reset during the cycle" },
            { 7, "homing fail: safety door opened during the cycle" },
            { 8, "homing fail: switch still triggered after pull-off" },
            { 9, "homing fail: switch not found within the search distance" },
            { 10, "spindle control fault" },
            { 11, "an input was already active at start-up" },
            { 12, "homing fail: switch at the wrong end" },
            { 13, "stop switch (e-stop) pressed" },
            { 14, "not homed" },
            { 15, "start-up" },
            { 16, "I/O expander reset" },
            { 17, "G-code error" },
            { 18, "switch A (probe input) touched outside a probing move" },
        };

        const CodeText ERRORS[] = {
            { 1, "expected a command letter" },
            { 2, "bad number format" },
            { 3, "invalid or unsupported '$' command" },
            { 4, "negative value not allowed" },
            { 5, "homing is not enabled in the settings" },
            { 6, "step pulse time too short" },
            { 7, "settings storage read failed; defaults restored" },
            { 8, "command only allowed when idle" },
            { 9, "locked out: controller is in alarm or jog state" },
            { 10, "soft limits need homing enabled" },
            { 11, "line too long" },
            { 12, "setting exceeds the maximum step rate" },
            { 13, "safety door open" },
            { 14, "startup line too long" },
            { 15, "jog target exceeds machine travel" },
            { 16, "invalid jog command" },
            { 17, "laser mode needs a PWM spindle" },
            { 20, "unsupported or invalid G-code command" },
            { 21, "two commands from the same modal group" },
            { 22, "feed rate not set" },
            { 23, "command needs an integer value" },
            { 24, "two commands both need axis words" },
            { 25, "repeated G-code word" },
            { 26, "axis words missing" },
            { 27, "invalid line number" },
            { 28, "missing P or L value" },
            { 29, "work coordinate system not supported" },
            { 30, "G53 needs G0 or G1" },
            { 31, "axis words with no motion mode active" },
            { 32, "arc has no axis words in the plane" },
            { 33, "invalid motion target" },
            { 34, "arc radius error" },
            { 35, "arc is missing its offset words" },
            { 36, "unused G-code words in the line" },
            { 37, "tool length offset on the wrong axis" },
            { 38, "tool number too large" },
        };

        const char* find(const CodeText* table, size_t n, int code) {
            for (size_t i = 0; i < n; i++) {
                if (table[i].code == code) {
                    return table[i].text;
                }
            }
            return nullptr;
        }

        bool parse_double(const std::string& s, double& out) {
            if (s.empty()) {
                return false;
            }
            const char* begin = s.c_str();
            char*       end   = nullptr;
            out               = std::strtod(begin, &end);
            return end == begin + s.size();
        }

        // "1.0,2.0,3.0" -> {1,2,3}. False if any item is not a number (Python: ValueError).
        bool parse_floats(const std::string& s, std::vector<double>& out) {
            out.clear();
            size_t start = 0;
            while (true) {
                size_t comma = s.find(',', start);
                double v;
                if (!parse_double(s.substr(start, comma == std::string::npos ? std::string::npos : comma - start), v)) {
                    return false;
                }
                out.push_back(v);
                if (comma == std::string::npos) {
                    return true;
                }
                start = comma + 1;
            }
        }
    }  // namespace

    std::string describe_alarm(int code) {
        const char* t = find(ALARMS, sizeof(ALARMS) / sizeof(ALARMS[0]), code);
        return t ? std::string(t) : "firmware-specific alarm " + std::to_string(code);
    }

    std::string describe_error(int code) {
        const char* t = find(ERRORS, sizeof(ERRORS) / sizeof(ERRORS[0]), code);
        return t ? std::string(t) : "firmware-specific error " + std::to_string(code);
    }

    std::string describe_pin(char letter) {
        switch (letter) {
            case 'X':
                return "X limit input";
            case 'Y':
                return "Y limit input";
            case 'Z':
                return "Z limit input";
            case 'P':
                return "probe input";
            case 'D':
                return "safety door input";
            case 'H':
                return "feed hold input";
            case 'R':
                return "reset input";
            case 'S':
                return "cycle start input";
            case 'E':
                return "e-stop input (FluidNC)";
            case 'F':
                return "fault input (FluidNC)";
            default:
                return std::string("input '") + letter + "'";
        }
    }

    std::string trim(const std::string& s) {
        size_t b = 0, e = s.size();
        while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) {
            b++;
        }
        while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
            e--;
        }
        return s.substr(b, e - b);
    }

    bool starts_with_nocase(const std::string& s, const char* prefix) {
        size_t i = 0;
        for (; prefix[i]; i++) {
            if (i >= s.size() || std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(prefix[i]))) {
                return false;
            }
        }
        return true;
    }

    bool is_digits(const std::string& s) {
        if (s.empty()) {
            return false;
        }
        for (char c : s) {
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                return false;
            }
        }
        return true;
    }

    bool is_status_report(const std::string& line) {
        std::string t = trim(line);
        return t.size() >= 3 && t.front() == '<' && t.back() == '>';
    }

    bool parse_status(const std::string& line, Status& out) {
        std::string text = trim(line);
        if (!is_status_report(text)) {
            return false;
        }
        std::string body = text.substr(1, text.size() - 2);

        std::vector<std::string> parts;
        size_t                   start = 0;
        while (true) {
            size_t bar = body.find('|', start);
            parts.push_back(body.substr(start, bar == std::string::npos ? std::string::npos : bar - start));
            if (bar == std::string::npos) {
                break;
            }
            start = bar + 1;
        }

        Status      s;
        std::string head  = parts[0];
        size_t      colon = head.find(':');
        s.state           = head.substr(0, colon);
        std::string sub   = colon == std::string::npos ? "" : head.substr(colon + 1);
        if (s.state.empty()) {
            return false;
        }
        for (char c : s.state) {
            if (!std::isalpha(static_cast<unsigned char>(c))) {
                return false;
            }
        }
        if (!sub.empty()) {
            if (!is_digits(sub)) {
                return false;
            }
            s.substate = std::atoi(sub.c_str());
        }

        std::string fs, f;
        bool        have_fs = false, have_f = false;
        for (size_t i = 1; i < parts.size(); i++) {
            size_t      c     = parts[i].find(':');
            std::string key   = parts[i].substr(0, c);
            std::string value = c == std::string::npos ? "" : parts[i].substr(c + 1);
            if (key.empty()) {
                continue;
            }
            if (key == "MPos") {
                if (!value.empty()) {
                    if (!parse_floats(value, s.mpos)) {
                        return false;
                    }
                    s.has_mpos = true;
                }
            } else if (key == "WPos") {
                std::vector<double> w;
                if (!value.empty() && !parse_floats(value, w)) {
                    return false;
                }
            } else if (key == "FS") {
                fs      = value;
                have_fs = !value.empty();
            } else if (key == "F") {
                f      = value;
                have_f = !value.empty();
            } else if (key == "Pn") {
                s.pins = value;
            }
        }
        if (have_fs || have_f) {
            std::vector<double> speeds;
            if (!parse_floats(have_fs ? fs : f, speeds)) {
                return false;
            }
            s.has_feed = true;
            s.feed     = speeds[0];
        }
        s.raw = text;
        out   = s;
        return true;
    }

}  // namespace lathe
