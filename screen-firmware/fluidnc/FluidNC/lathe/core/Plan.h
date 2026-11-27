// Ceratek lathe screen: the settings of a cut and the moves of the cut.
//
// The group's plan (2026-11-24, as told on the screen):
//   - Locate first (switches B, C, A); afterwards the tool is at the start corner (bottom left).
//   - Start new tube, from the start corner:
//       1. depth: each round the tool moves one step further toward the tube (axis Y, "B") and
//          stops. OK: this depth is right. Return: not yet, one step further.
//       2. length: the tool moves slowly along the tube toward switch A (axis Z). OK: the length
//          is this long. Return: not right, back to the start of the length and measure again.
//          Touching switch A ends the measurement there.
//       3. cut: the tube turns (axis X) while the tool cuts along the measured length at the
//          chosen depth, from the start of the length toward A.
//       4. back to the start corner: Y to 0, then Z to 0. Reaching 0 ends it; a switch touched
//          on the way stops it too (as at any time).
// The turning tube axis (X) is not on the bench yet; its moves are sent all the same.
#pragma once

#include <string>
#include <vector>

namespace lathe {

    struct CutSettings {
        double depth_step_mm;        // how far each round moves the tool toward the tube
        double cut_feed_mm_per_min;  // speed of the depth steps, the length measurement and the cut
        int    y_toward_tube;        // +1 or -1
        int    z_cut_direction;      // +1 or -1: from switch C toward switch A
        double jog_feed_mm_per_min;  // speed of the moves with the tool clear of the tube
        double z_search_mm;          // farthest the length measurement may go (Z max travel)
        double tube_rpm;             // turning speed of the tube during the cut
        bool   rotate = true;        // the third axis (X, the tube) turns during the cut

        // Empty when the settings make sense, else the reason.
        std::string problem() const;
    };

    // The cut (machine coordinates): at the depth, back to the start of the length (no
    // retreat from the tube), then along the length while the tube turns.
    std::vector<std::string> cut_gcode(double depth_y, double start_z, double length, const CutSettings& settings);

    // Back to the start corner: out to Y 0, then along to Z 0.
    std::vector<std::string> return_gcode(const CutSettings& settings);

    // f"{value:.Nf}"
    std::string fixed(double value, int decimals);

}  // namespace lathe
