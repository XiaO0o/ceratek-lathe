// Ceratek lathe screen: display and touch set-up. See Display.h.
#include "Display.h"

#include <nvs.h>

namespace lathe {

    namespace {
        const char* NVS_NAMESPACE = "ceratek";
        const char* NVS_KEY       = "touch_cal";
    }  // namespace

    void Display::configure(const DisplayPins& pins, const DisplayOptions& options) {
        {
            auto cfg        = _bus.config();
            cfg.spi_host    = VSPI_HOST;
            cfg.spi_mode    = 0;
            cfg.freq_write  = options.lcd_spi_hz;
            cfg.freq_read   = 16000000;
            cfg.spi_3wire   = false;  // the panel has its own MISO line (SDO through R15)
            cfg.use_lock    = true;
            cfg.dma_channel = SPI_DMA_CH_AUTO;
            cfg.pin_sclk    = pins.sck;
            cfg.pin_mosi    = pins.mosi;
            cfg.pin_miso    = pins.miso;
            cfg.pin_dc      = pins.lcd_dc;
            _bus.config(cfg);
            _panel.setBus(&_bus);
        }
        {
            auto cfg             = _panel.config();
            cfg.pin_cs           = pins.lcd_cs;
            cfg.pin_rst          = pins.lcd_reset;
            cfg.pin_busy         = -1;
            cfg.panel_width      = 320;  // native orientation is portrait
            cfg.panel_height     = 480;
            cfg.offset_x         = 0;
            cfg.offset_y         = 0;
            cfg.offset_rotation  = 0;
            cfg.dummy_read_pixel = 8;
            cfg.dummy_read_bits  = 1;
            cfg.readable         = true;
            cfg.invert           = options.invert_colors;
            cfg.rgb_order        = options.swap_red_blue;
            cfg.dlen_16bit       = false;
            cfg.bus_shared       = true;  // the touch chip is on the same wires
            _panel.config(cfg);
        }
        {
            auto cfg            = _touch.config();
            cfg.x_min           = 300;
            cfg.x_max           = 3900;
            cfg.y_min           = 300;
            cfg.y_max           = 3900;
            cfg.pin_int         = -1;  // the pen interrupt is not wired to a known ESP32 pin
            cfg.bus_shared      = true;
            cfg.offset_rotation = 0;
            cfg.spi_host        = VSPI_HOST;
            cfg.freq            = options.touch_spi_hz;
            cfg.pin_sclk        = pins.sck;
            cfg.pin_mosi        = pins.mosi;
            cfg.pin_miso        = pins.miso;
            cfg.pin_cs          = pins.touch_cs;
            _touch.config(cfg);
            _panel.setTouch(&_touch);
        }
        setPanel(&_panel);
    }

    bool Display::load_calibration() {
        nvs_handle_t h;
        if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
            return false;
        }
        size_t len = sizeof(_cal);
        bool   ok  = nvs_get_blob(h, NVS_KEY, _cal, &len) == ESP_OK && len == sizeof(_cal);
        nvs_close(h);
        if (ok) {
            setTouchCalibrate(_cal);
        }
        return ok;
    }

    bool Display::save_calibration() {
        nvs_handle_t h;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
            return false;
        }
        bool ok = nvs_set_blob(h, NVS_KEY, _cal, sizeof(_cal)) == ESP_OK && nvs_commit(h) == ESP_OK;
        nvs_close(h);
        return ok;
    }

    void Display::run_calibration() {
        fillScreen(0x0000);
        setTextColor(0xFFFF, 0x0000);
        setTextDatum(lgfx::middle_center);
        setFont(&lgfx::fonts::Font4);
        drawString("Touch calibration", width() / 2, height() / 2 - 30);
        setFont(&lgfx::fonts::Font2);
        drawString("Touch each arrow tip in turn, firmly, with a fingernail or stylus.", width() / 2, height() / 2 + 10);
        calibrateTouch(_cal, static_cast<uint16_t>(0xFFFF), static_cast<uint16_t>(0x0000), 20);
        setTouchCalibrate(_cal);
    }

}  // namespace lathe
