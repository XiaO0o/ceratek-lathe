// Ceratek lathe screen: the MKS TS35-R display and touch, driven with LovyanGFX.
//
// Panel: ST7796, 480 x 320, 4-wire SPI on the ESP32's VSPI host (the SD card uses HSPI).
// Touch: ADS7843E, XPT2046 command set, on the same bus with its own chip select.
// Pins come from the lathe_screen: block of config.yaml, not from #defines.
// Only the screen task may call anything in here.
#pragma once

#include <cstdint>

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

namespace lathe {

    struct DisplayPins {
        int sck, mosi, miso;
        int lcd_cs, lcd_dc, lcd_reset;
        int touch_cs;
    };

    struct DisplayOptions {
        uint32_t lcd_spi_hz    = 40000000;
        uint32_t touch_spi_hz  = 2500000;
        int      rotation      = 1;  // 1 = landscape, as the MKS firmware
        bool     invert_colors = false;
        bool     swap_red_blue = false;
    };

    class Display : public lgfx::LGFX_Device {
    public:
        // Sets up the bus, panel and touch from the pins. Call init() afterwards.
        void configure(const DisplayPins& pins, const DisplayOptions& options);

        // Touch calibration: 8 values from calibrateTouch(), kept in the ESP32's NVS.
        bool load_calibration();
        bool save_calibration();
        void run_calibration();  // blocks until the four corners have been touched

    private:
        lgfx::Panel_ST7796  _panel;
        lgfx::Bus_SPI       _bus;
        lgfx::Touch_XPT2046 _touch;
        uint16_t            _cal[8] = {};
    };

}  // namespace lathe
