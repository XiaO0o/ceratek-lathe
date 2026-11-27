// Ceratek lathe screen: the FluidNC module. See LatheScreen.h.
#include "LatheScreen.h"

#include "Logging.h"
#include "Serial.h"  // allChannels
#include "Event.h"
#include "Protocol.h"
#include "System.h"
#include "Stepping.h"
#include "Machine/Axes.h"
#include "Machine/Axis.h"

#include <esp_timer.h>

#include "TouchUi.h"

namespace lathe {
    // Runs on FluidNC's protocol task (through an event), so it cannot race a move starting:
    // a move that starts afterwards enables the motors again in Stepper::wake_up().
    // Added 2026-11-22: after the stop switch was pressed during a pass and the job was ended,
    // the motor stayed powered and heated up although FluidNC's idle_ms rule should have
    // switched it off. $MD could not help there: FluidNC refuses it in the Critical state.
    static void release_motors_now() {
        if (::Machine::Stepping::_idleMsecs >= 255) {
            return;  // configured to hold position
        }
        if (state_is(State::Idle) || state_is(State::Alarm) || state_is(State::Critical)) {
            ::Machine::Axes::set_disable(true, true);
        }
    }
    static NoArgEvent releaseMotorsEvent { release_motors_now };

    void LatheScreen::release_motors() { protocol_send_event(&releaseMotorsEvent); }

    double LatheScreen::travel(int axis) const {
        if (axis < 0 || axis >= MAX_N_AXIS || !::Machine::Axes::_axis[axis]) {
            return 0;
        }
        return ::Machine::Axes::_axis[axis]->_maxTravel;
    }

    double LatheScreen::max_rate(int axis) const {
        if (axis < 0 || axis >= MAX_N_AXIS || !::Machine::Axes::_axis[axis]) {
            return 0;
        }
        return ::Machine::Axes::_axis[axis]->_maxRate;
    }


    namespace {
        class Lock {
        public:
            explicit Lock(SemaphoreHandle_t m) : _m(m) { xSemaphoreTake(_m, portMAX_DELAY); }
            ~Lock() { xSemaphoreGive(_m); }

        private:
            SemaphoreHandle_t _m;
        };
    }  // namespace

    // ------------------------------------------------------------------ ByteQueue

    void ByteQueue::push(const uint8_t* data, size_t n) {
        Lock l(_mutex);
        _bytes.append(reinterpret_cast<const char*>(data), n);
        if (_bytes.size() > _capacity) {
            _bytes.erase(0, _bytes.size() - _capacity);
        }
    }

    size_t ByteQueue::pop(uint8_t* out, size_t max) {
        Lock   l(_mutex);
        size_t n = _bytes.size() < max ? _bytes.size() : max;
        for (size_t i = 0; i < n; i++) {
            out[i] = static_cast<uint8_t>(_bytes[i]);
        }
        _bytes.erase(0, n);
        return n;
    }

    size_t ByteQueue::size() {
        Lock l(_mutex);
        return _bytes.size();
    }

    void ByteQueue::clear() {
        Lock l(_mutex);
        _bytes.clear();
    }

    // ------------------------------------------------------------------ RtosClock

    double RtosClock::now() { return esp_timer_get_time() / 1e6; }

    void RtosClock::sleep(double s) {
        TickType_t ticks = pdMS_TO_TICKS(static_cast<uint32_t>(s * 1000));
        vTaskDelay(ticks ? ticks : 1);
    }

    // ------------------------------------------------------------------ StatusTap

    void StatusTap::start(uint32_t interval_ms) {
        _mutex            = xSemaphoreCreateMutex();
        _report_when_idle = true;
        allChannels.registration(this);
        setReportInterval(interval_ms);
    }

    LiveStatus StatusTap::live() {
        if (!_mutex) {
            return LiveStatus {};
        }
        Lock l(_mutex);
        return _live;
    }

    size_t StatusTap::write(uint8_t c) {
        if (c == '\r') {
            return 1;
        }
        if (c != '\n') {
            if (_partial.size() < 256) {
                _partial += static_cast<char>(c);
            }
            return 1;
        }
        Status s;
        if (parse_status(_partial, s) && _mutex) {
            Lock l(_mutex);
            _live.status = s;
            _live.valid  = true;
            _live.reports++;
        }
        _partial.clear();
        return 1;
    }

    Error StatusTap::pollLine(char* line) {
        autoReport();
        return Error::NoData;
    }

    // ------------------------------------------------------------------ LatheScreen

    void LatheScreen::group(Configuration::HandlerBase& handler) {
        // Screen wiring. MKS TS35-R on the MKS DLC32 EXP1/EXP2 sockets: sck gpio.18,
        // mosi gpio.23, miso gpio.19, lcd_cs gpio.25, lcd_dc gpio.33, lcd_reset gpio.27,
        // touch_cs gpio.26, backlight gpio.5 (high = lit on the V2.0_002 drawing).
        handler.item("sck_pin", _sck);
        handler.item("mosi_pin", _mosi);
        handler.item("miso_pin", _miso);
        handler.item("lcd_cs_pin", _lcd_cs);
        handler.item("lcd_dc_pin", _lcd_dc);
        handler.item("lcd_reset_pin", _lcd_reset);
        handler.item("touch_cs_pin", _touch_cs);
        handler.item("backlight_pin", _backlight);
        handler.item("beeper_pin", _beeper);
        handler.item("rotation", _rotation, 0, 3);
        handler.item("lcd_spi_hz", _lcd_spi_hz, 1000000, 80000000);
        handler.item("touch_spi_hz", _touch_spi_hz, 100000, 4000000);
        handler.item("invert_colors", _invert_colors);
        handler.item("swap_red_blue", _swap_red_blue);
        handler.item("status_interval_ms", _status_interval_ms, 50, 2000);

        // false: the screen shows status and can be calibrated, but cannot start anything.
        handler.item("allow_motion", _allow_motion);

        // Cutting parameters (the group's plan, see core/Plan.h).
        handler.item("depth_step_mm", _depth_step_mm);
        handler.item("cut_feed_mm_per_min", _cut_feed_mm_per_min);
        handler.item("tube_rpm", _tube_rpm);
        handler.item("y_toward_tube", _y_toward_tube, -1, 1);
        handler.item("z_cut_direction", _z_cut_direction, -1, 1);
        handler.item("jog_feed_mm_per_min", _jog_feed_mm_per_min);
    }

    CutSettings LatheScreen::cut_settings() const {
        // The cut looks for switch A at most as far as the Z axis can travel.
        double z_search = 0;
        if (::Machine::Axes::_axis[Z_AXIS]) {
            z_search = ::Machine::Axes::_axis[Z_AXIS]->_maxTravel;
        }
        return CutSettings { _depth_step_mm, _cut_feed_mm_per_min, int(_y_toward_tube), int(_z_cut_direction), _jog_feed_mm_per_min,
                             z_search,       _tube_rpm };
    }

    void LatheScreen::validate() {
        const Pin* required[] = { &_sck, &_mosi, &_miso, &_lcd_cs, &_lcd_dc, &_touch_cs };
        for (const Pin* p : required) {
            if (!p->defined()) {
                log_config_error("lathe_screen: sck, mosi, miso, lcd_cs, lcd_dc and touch_cs pins must all be set");
                _error = true;
                return;
            }
        }
        std::string problem = cut_settings().problem();
        if (!problem.empty()) {
            log_config_error("lathe_screen: " << problem.c_str());
            _error = true;
        }
    }

    void LatheScreen::init() {
        // validate() normally catches these; checked again so that an unset pin can never be
        // handed to the display driver as some default GPIO.
        const Pin* required[] = { &_sck, &_mosi, &_miso, &_lcd_cs, &_lcd_dc, &_touch_cs };
        for (const Pin* p : required) {
            if (!p->defined()) {
                _error = true;
            }
        }
        if (_error) {
            log_error("lathe_screen: not started because of configuration errors (see above)");
            return;
        }
        log_info("Lathe screen: SCK " << _sck.name() << " MOSI " << _mosi.name() << " MISO " << _miso.name() << " LCD_CS " << _lcd_cs.name()
                                      << " DC " << _lcd_dc.name() << " RST " << _lcd_reset.name() << " TOUCH_CS " << _touch_cs.name()
                                      << " BL " << _backlight.name() << " motion " << (_allow_motion ? "allowed" : "off"));
        if (_backlight.defined()) {
            _backlight.setAttr(Pin::Attr::Output);
            _backlight.write(false);  // dark until the first picture is drawn
        }
        if (_beeper.defined()) {
            _beeper.setAttr(Pin::Attr::Output);
            _beeper.write(false);
        }

        // The command channel: lines pushed into it are run by FluidNC like USB lines. It sends
        // no automatic reports (interval 0), so the lathe logic sees only answers to its own
        // requests, as over USB. The status bar gets its reports from the separate tap.
        allChannels.registration(this);
        _tap.start(_status_interval_ms);

        _ui = new TouchUi(*this);
        _ui->start();
        log_info("Lathe screen: started, free heap " << xPortGetFreeHeapSize());
    }

    size_t LatheScreen::write(uint8_t c) {
        _inbox.push(&c, 1);
        return 1;
    }

    // Called by FluidNC at the start of every pollLine(), on its polling task: hand over what
    // the lathe logic and the screen want to send. Channel::push() treats a real-time byte
    // ('?', '!', '~', 0x18) at once and queues the rest as line input, exactly as for USB.
    void LatheScreen::handle() {
        uint8_t buf[64];
        size_t  n;
        while ((n = _outbox.pop(buf, sizeof(buf))) > 0) {
            for (size_t i = 0; i < n; i++) {
                push(buf[i]);
            }
        }
    }

}  // namespace lathe

namespace {
    ConfigurableModuleFactory::InstanceBuilder<lathe::LatheScreen> lathe_screen_module __attribute__((init_priority(105))) ("lathe_screen");
}
