// See Plan.h.
#include "Plan.h"

#include <cmath>
#include <cstdio>

namespace lathe {

    std::string fixed(double value, int decimals) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%.*f", decimals, value);
        std::string s = buf;
        if (s == "-0" || s.rfind("-0.", 0) == 0) {
            // "-0.000" -> "0.000": a value that rounds to zero has no sign
            bool all_zero = true;
            for (char c : s.substr(1)) {
                if (c != '0' && c != '.') {
                    all_zero = false;
                }
            }
            if (all_zero) {
                s = s.substr(1);
            }
        }
        return s;
    }

    std::string CutSettings::problem() const {
        if (!(depth_step_mm > 0)) {
            return "depth_step_mm must be greater than zero";
        }
        if (depth_step_mm > MAX_DEPTH_STEP_MM) {
            return "depth_step_mm must not be more than 10 mm";
        }
        if (!(switch_a_margin_mm >= 0) || switch_a_margin_mm > 10.0) {
            return "switch_a_margin_mm must be from 0 to 10 mm";
        }
        if (!(cut_feed_mm_per_min > 0)) {
            return "cut_feed_mm_per_min must be greater than zero";
        }
        if (!(jog_feed_mm_per_min > 0)) {
            return "jog_feed_mm_per_min must be greater than zero";
        }
        if (!(tube_rpm > 0)) {
            return "tube_rpm must be greater than zero";
        }
        if (y_toward_tube != 1 && y_toward_tube != -1) {
            return "y_toward_tube must be 1 or -1";
        }
        if (z_cut_direction != 1 && z_cut_direction != -1) {
            return "z_cut_direction must be 1 or -1";
        }
        if (!(z_search_mm > 0)) {
            return "the Z axis needs a max_travel_mm greater than zero";
        }
        return "";
    }

    std::vector<std::string> cut_gcode(double depth_y, double start_z, double length, const CutSettings& s) {
        std::string cut = fixed(s.cut_feed_mm_per_min, 1);
        // Along the length at the cut feed while the tube turns at tube_rpm. X (turns) and Z
        // (mm) move together in one line, so the feed is for the combined length.
        double minutes = length / s.cut_feed_mm_per_min;
        double turns   = s.rotate ? s.tube_rpm * minutes : 0.0;
        double feed    = minutes > 0 ? std::sqrt(length * length + turns * turns) / minutes : s.cut_feed_mm_per_min;
        // Clear of the tube before going back along it: 2 mm back from the depth, not to Y 0
        // (that is next to switch B). A depth of 2 mm or less backs off half of it, so the tool
        // never reaches 0.
        double depth   = std::fabs(depth_y);
        double back    = depth > 2.0 ? 2.0 : depth / 2.0;
        double clear_y = depth_y - (depth_y < 0 ? -back : back);
        return {
            "G21 G90",
            "G1 Y" + fixed(clear_y, 3) + " F" + cut,   // 2 mm back from the tube
            "G1 Z" + fixed(start_z, 3) + " F" + cut,   // to the start of the length
            "G1 Y" + fixed(depth_y, 3) + " F" + cut,   // in to the depth again
            "G91",                                                         // the turning axis has no fixed zero
            s.rotate ? "G1 X" + fixed(turns, 3) + " Z" + fixed(s.z_cut_direction * length, 3) + " F" + fixed(feed, 1)
                     : "G1 Z" + fixed(s.z_cut_direction * length, 3) + " F" + cut,  // third axis off: no turning
            "G90",
        };
    }

    std::vector<std::string> return_gcode(const CutSettings& s) {
        std::string travel = fixed(s.jog_feed_mm_per_min, 1);
        return { "G21 G90", "G1 Y0.000 F" + travel, "G1 Z0.000 F" + travel };
    }

}  // namespace lathe
