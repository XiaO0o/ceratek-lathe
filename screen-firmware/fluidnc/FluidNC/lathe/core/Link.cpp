// C++ port of lathe/link.py. See Link.h.
#include "Link.h"

#include <cctype>
#include <cstdlib>

namespace lathe {

    namespace {
        constexpr double IDLE_SLEEP = 0.002;  // pause when the port had nothing, so idle loops stay cheap
        constexpr size_t MAX_LINE   = 2048;   // longer than any Grbl line; guards against noise without a newline
    }  // namespace

    int Reply::error_code() const { return (!ok && is_digits(error)) ? std::atoi(error.c_str()) : -1; }

    std::string Reply::describe_error() const {
        if (ok) {
            return "";
        }
        int code = error_code();
        if (code >= 0) {
            return "error:" + std::to_string(code) + " (" + lathe::describe_error(code) + ")";
        }
        return "error: " + error;
    }

    std::vector<std::string> LineLog::since(size_t mark) const {
        std::vector<std::string> out;
        size_t                   from = mark < _base ? 0 : mark - _base;
        for (size_t i = from; i < _lines.size(); i++) {
            out.push_back(_lines[i]);
        }
        return out;
    }

    bool LineLog::contains_since(size_t mark, const std::string& line) const {
        size_t from = mark < _base ? 0 : mark - _base;
        for (size_t i = from; i < _lines.size(); i++) {
            if (_lines[i] == line) {
                return true;
            }
        }
        return false;
    }

    void LineLog::push(const std::string& line) {
        _lines.push_back(line);
        if (_lines.size() > 2 * _keep) {
            size_t drop = _lines.size() - _keep;
            _lines.erase(_lines.begin(), _lines.begin() + drop);
            _base += drop;
        }
    }

    void GrblLink::realtime(char byte) {
        uint8_t b = static_cast<uint8_t>(byte);
        _port.write(&b, 1);
    }

    void GrblLink::send(const std::string& text) {
        set_aside_unsolicited();
        std::string line = text + "\n";
        _port.write(reinterpret_cast<const uint8_t*>(line.data()), line.size());
    }

    Reply GrblLink::command(const std::string& text, double timeout) {
        send(text);
        double                   deadline = _clock.now() + timeout;
        std::vector<std::string> lines;
        std::string              line;
        while (next_line(deadline, line)) {
            if (line == "ok") {
                Reply r;
                r.command = text;
                r.lines   = lines;
                return r;
            }
            if (starts_with_nocase(line, "error:")) {
                Reply r;
                r.command = text;
                r.lines   = lines;
                r.ok      = false;
                r.error   = trim(line.substr(6));
                return r;
            }
            lines.push_back(line);
        }
        throw GrblTimeout("no 'ok' or 'error' after '" + text + "'", lines);
    }

    Status GrblLink::status(double timeout) {
        realtime(RT_STATUS);
        double      deadline = _clock.now() + timeout;
        std::string line;
        while (next_line(deadline, line)) {
            if (is_status_report(line)) {
                Status s;
                if (!parse_status(line, s)) {
                    throw std::invalid_argument("malformed status report: " + line);
                }
                return s;
            }
            unsolicited.push(line);
        }
        throw GrblTimeout("no status report");
    }

    std::vector<std::string> GrblLink::drain(double quiet, double max_wait, double first_byte) {
        std::vector<std::string> lines;
        double                   start     = _clock.now();
        bool                     have_data = false;
        double                   last_data = 0;
        while (true) {
            double now = _clock.now();
            if (now - start >= max_wait) {
                break;
            }
            if (!have_data && now - start >= first_byte) {
                break;
            }
            if (have_data && now - last_data >= quiet) {
                break;
            }
            if (pump()) {
                have_data = true;
                last_data = _clock.now();
            }
            while (!_pending.empty()) {
                lines.push_back(_pending.front());
                _pending.pop_front();
            }
        }
        flush_partial_line();
        while (!_pending.empty()) {
            lines.push_back(_pending.front());
            _pending.pop_front();
        }
        return lines;
    }

    bool GrblLink::next_line(double deadline, std::string& out) {
        while (true) {
            if (!_pending.empty()) {
                out = _pending.front();
                _pending.pop_front();
                return true;
            }
            if (_clock.now() >= deadline) {
                return false;
            }
            pump();
        }
    }

    void GrblLink::set_aside_unsolicited() {
        // A late answer to an earlier, timed-out command must not be mistaken for the
        // answer to the next one.
        while (_port.in_waiting()) {
            pump();
        }
        for (const auto& l : _pending) {
            unsolicited.push(l);
        }
        _pending.clear();
    }

    size_t GrblLink::pump() {
        uint8_t chunk[256];
        size_t  n = _port.read(chunk, sizeof(chunk));
        if (!n) {
            _clock.sleep(IDLE_SLEEP);
            return 0;
        }
        _buffer.append(reinterpret_cast<const char*>(chunk), n);
        size_t cut;
        while ((cut = _buffer.find('\n')) != std::string::npos) {
            accept(_buffer.substr(0, cut));
            _buffer.erase(0, cut + 1);
        }
        if (_buffer.size() > MAX_LINE) {
            flush_partial_line();
        }
        return n;
    }

    void GrblLink::flush_partial_line() {
        if (!_buffer.empty()) {
            accept(_buffer);
            _buffer.clear();
        }
    }

    void GrblLink::accept(const std::string& raw) {
        std::string text = trim(raw);
        std::string clean;
        for (unsigned char c : text) {
            clean += (c >= 32 && c < 127) ? static_cast<char>(c) : '?';
        }
        if (!clean.empty()) {
            _pending.push_back(clean);
        }
    }

}  // namespace lathe
