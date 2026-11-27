// Ceratek lathe screen: the FluidNC module.
//
// LatheScreen is both a FluidNC Channel and a ConfigurableModule, like FluidNC's own OLED
// module. It exists only when config.yaml has a lathe_screen: block.
//
// To FluidNC it is one more command channel, exactly like the USB port: lines pushed into it
// are executed by FluidNC's normal code, and FluidNC answers with the same "ok", "error:n",
// "ALARM:n" and <status> lines it sends over USB. The lathe logic (core/) therefore talks to
// it through a byte port, the way the laptop software talks to the serial port. Nothing in
// FluidNC's motion, homing, limit or stop handling is changed or called directly.
//
// Tasks:
//   FluidNC polling task (core 0)  -> write(): bytes from FluidNC into the inbox
//                                  -> handle(): bytes from the outbox into FluidNC
//                                  -> StatusTap::write(): status reports for the status bar
//   lathescr task (core 0, prio 1) -> display, touch, screens (TouchUi)
//   lathejob task (core 0, prio 1) -> the guided sequence (core/Session), blocking like Python
#pragma once

#include "Channel.h"
#include "Config.h"
#include "Module.h"
#include "Pin.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <string>

#include "core/Grbl.h"
#include "core/Link.h"
#include "core/Plan.h"

namespace lathe {

    // A byte queue shared between tasks.
    class ByteQueue {
    public:
        explicit ByteQueue(size_t capacity) : _capacity(capacity) { _mutex = xSemaphoreCreateMutex(); }
        void   push(const uint8_t* data, size_t n);  // drops the oldest bytes when full
        size_t pop(uint8_t* out, size_t max);
        size_t size();
        void   clear();

    private:
        size_t            _capacity;
        std::string       _bytes;
        SemaphoreHandle_t _mutex;
    };

    // The byte port the lathe logic uses: reads what FluidNC sent, writes what FluidNC will get.
    class ChannelPort : public BytePort {
    public:
        ChannelPort(ByteQueue& inbox, ByteQueue& outbox) : _in(inbox), _out(outbox) {}
        size_t in_waiting() override { return _in.size(); }
        size_t read(uint8_t* buffer, size_t max) override { return _in.pop(buffer, max); }
        void   write(const uint8_t* data, size_t size) override { _out.push(data, size); }

    private:
        ByteQueue& _in;
        ByteQueue& _out;
    };

    class RtosClock : public Clock {
    public:
        double now() override;
        void   sleep(double s) override;
    };

    // What the screen shows in its status bar, kept up to date from every status report.
    struct LiveStatus {
        Status   status;
        uint32_t reports = 0;  // rises with every report
        bool     valid   = false;
    };

    // A second, listen-only channel for the status bar, built like FluidNC's Status_outputs.
    // FluidNC sends it a status report whenever the state or an input changes, and every
    // report interval. Keeping these reports out of the command channel means the lathe logic
    // sees exactly what it would see over USB.
    class StatusTap : public Channel {
    public:
        StatusTap() : Channel("lathe_status") {}
        void       start(uint32_t interval_ms);
        LiveStatus live();

        size_t write(uint8_t c) override;
        Error  pollLine(char* line) override;
        void   flushRx() override {}
        bool   lineComplete(char*, char) override { return false; }
        size_t timedReadBytes(char* buffer, size_t length, TickType_t timeout) override { return 0; }

    private:
        std::string       _partial;
        LiveStatus        _live;
        SemaphoreHandle_t _mutex = nullptr;
    };

    class TouchUi;

    class LatheScreen : public Channel, public ConfigurableModule {
    public:
        explicit LatheScreen(const char* name) : Channel(name), ConfigurableModule(name) {}

        LatheScreen(const LatheScreen&)            = delete;
        LatheScreen& operator=(const LatheScreen&) = delete;

        // ConfigurableModule
        void group(Configuration::HandlerBase& handler) override;
        void validate() override;
        void afterParse() override {}
        void init() override;

        // Channel: called by FluidNC's own tasks
        size_t write(uint8_t c) override;
        void   handle() override;

        // For the screen and job tasks
        LiveStatus  live() { return _tap.live(); }
        CutSettings cut_settings() const;
        bool        motion_allowed() const { return _allow_motion; }

        // Asks FluidNC's own task to switch the motors off now, if the machine is in Idle or
        // Alarm and config.yaml releases them when idle (idle_ms below 255). Safe from any task.
        void release_motors();

        // Max travel of an axis in mm from config.yaml (Y_AXIS, Z_AXIS); 0 if unknown.
        double travel(int axis) const;
        // Max rate of an axis from config.yaml (X: rev/min, Y and Z: mm/min); 0 if unknown.
        double max_rate(int axis) const;
        ByteQueue&  inbox() { return _inbox; }
        ByteQueue&  outbox() { return _outbox; }

        // Settings read from config.yaml
        Pin      _sck, _mosi, _miso, _lcd_cs, _lcd_dc, _lcd_reset, _touch_cs, _backlight, _beeper;
        int32_t  _rotation      = 1;
        uint32_t _lcd_spi_hz    = 40000000;
        uint32_t _touch_spi_hz  = 2500000;
        bool     _invert_colors = false;
        bool     _swap_red_blue = false;
        bool     _allow_motion  = false;
        int32_t  _status_interval_ms = 250;

        float   _depth_step_mm       = 1.0f;
        float   _cut_feed_mm_per_min = 60.0f;
        float   _tube_rpm            = 60.0f;
        int32_t _y_toward_tube       = 1;
        int32_t _z_cut_direction     = 1;
        float   _jog_feed_mm_per_min = 120.0f;
        float   _switch_a_margin_mm  = 1.0f;

    private:
        ByteQueue _inbox { 4096 };
        ByteQueue _outbox { 1024 };
        StatusTap _tap;
        TouchUi*  _ui    = nullptr;
        bool      _error = false;
    };

}  // namespace lathe
