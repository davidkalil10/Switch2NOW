#pragma once

#include <atomic>

namespace opennow::video_display {

// Display geometry modes for the Switch 2 stream renderer. FIT is the default:
// the whole decoded frame stays inside the physical screen frame (letterbox /
// pillarbox instead of cropping the game).
//
// Zoom was removed deliberately: on a 16:9 stream it only cropped the picture
// and did not help with the full-frame problem. These three modes change the
// display shape without offering any magnification.
enum class Mode : int {
    Fit = 0,
    Fill,
    Stretch,
};

inline constexpr int kModeCount = 3;

inline std::atomic<int> g_mode{static_cast<int>(Mode::Fit)};

inline const char* modeLabel(int mode)
{
    switch (mode) {
        case static_cast<int>(Mode::Fit):
            return "FIT (inside frame)";
        case static_cast<int>(Mode::Fill):
            return "FILL (crop edges)";
        case static_cast<int>(Mode::Stretch):
            return "STRETCH";
        default:
            return "FIT (inside frame)";
    }
}

} // namespace opennow::video_display
