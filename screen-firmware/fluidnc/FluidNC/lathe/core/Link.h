// Ceratek lathe screen: one conversation with the controller over a serial-like port.
// C++ port of lathe/link.py. On the controller the "port" is the lathe_screen channel inside
// FluidNC; in the PC tests it is a simulated controller (host-tests/SimGrbl.h).
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <stdexcept>
#include <string>
#include <vector>

#include "Grbl.h"

namespace lathe {

    // What the link needs from a port: pyserial's in_waiting / read / write.
    class BytePort {
    public:
        virtual ~BytePort() = default;
        virtual size_t in_waiting()                            = 0;
        virtual size_t read(uint8_t* buffer, size_t max)       = 0;  // never blocks; 0 = nothing
        virtual void   write(const uint8_t* data, size_t size) = 0;
    };

    // Time, so the PC tests can run on a fake clock.
    class Clock {
    public:
        virtual ~Clock()              = default;
        virtual double now()          = 0;  // seconds, monotonic
        virtual void   sleep(double s) = 0;
    };

    class GrblTimeout : public std::runtime_error {
    public:
        GrblTimeout(const std::string& message, std::vector<std::string> heard = {}) : std::runtime_error(message), lines(std::move(heard)) {}
        std::vector<std::string> lines;
    };

    struct Reply {
        std::string              command;
        std::vector<std::string> lines;
        bool                     ok = true;
        std::string              error;  // text after "error:"

        int         error_code() const;  // -1 when not a number
        std::string describe_error() const;
    };

    // Lines that arrived without being the answer to anything. Kept as a log with a running
    // index, like the Python list, but trimmed at the front so a long job cannot fill the heap.
    class LineLog {
    public:
        size_t                   size() const { return _base + _lines.size(); }
        std::vector<std::string> since(size_t mark) const;
        std::vector<std::string> all() const { return since(_base); }
        void                     push(const std::string& line);
        bool                     contains_since(size_t mark, const std::string& line) const;

    private:
        static constexpr size_t _keep = 300;
        size_t                  _base = 0;
        std::deque<std::string> _lines;
    };

    class GrblLink {
    public:
        GrblLink(BytePort& port, Clock& clock) : _port(port), _clock(clock) {}

        void   realtime(char byte);
        void   send(const std::string& text);
        Reply  command(const std::string& text, double timeout = 3.0);
        Status status(double timeout = 1.0);

        std::vector<std::string> drain(double quiet = 1.5, double max_wait = 10.0, double first_byte = 3.0);

        LineLog unsolicited;

    private:
        bool next_line(double deadline, std::string& out);
        void set_aside_unsolicited();
        size_t pump();
        void   flush_partial_line();
        void   accept(const std::string& raw);

        BytePort&               _port;
        Clock&                  _clock;
        std::string             _buffer;
        std::deque<std::string> _pending;
    };

}  // namespace lathe
