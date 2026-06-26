/*
  Copyright (c) 2026 Schildkroet

  This file is part of cangaroo.

  cangaroo is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  cangaroo is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with cangaroo.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once
// ============================================================
//  ldf_parser.hpp  –  LIN Description File Parser (C++20)
//  Single-header, no external dependencies
// ============================================================
//
//  Supported sections
//  ------------------
//  LIN_description_file
//  LIN_protocol_version, LIN_language_version
//  Channel_name
//  Node_attributes   (with NAD, configured_NAD, initial_NAD,
//                     product_id, response_error, P2_min,
//                     ST_min, N_As_timeout, N_Cr_timeout,
//                     configurable_frames, frames)
//  Nodes             (Master / Slaves)
//  Signals           (with init value scalar or array)
//  Frames            (unconditional)
//  Sporadic_frames
//  Event_triggered_frames
//  Diagnostic_frames
//  Signal_encoding_types  (logical, physical, bcd, ascii)
//  Signal_representation
//  Schedule_tables   (with all command types)
//
//  Unit handling
//  -------------
//  Numbers  : decimal, hex (0x…), binary (0b…), float
//  Suffixes : k / K  → ×1 000
//             M      → ×1 000 000
//  Time     : value alone → seconds
//             ms          → milliseconds  → stored as seconds
//             us / µs     → microseconds  → stored as seconds
//             kbps / bps  → stored as bps (double)
//
//  Usage
//  -----
//  #include "ldf_parser.hpp"
//
//  auto result = ldf::parse_file("my_bus.ldf");
//  if (!result) { std::cerr << result.error() << '\n'; return 1; }
//  const ldf::LdfFile& ldf = *result;
//
// ============================================================

#include <algorithm>
#include <charconv>
#include <cstdint>
#ifdef __cpp_lib_expected
#include <expected>
#endif
#include <format>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

// ---------------------------------------------------------------------------
// C++20 polyfill: std::expected is C++23; provide a minimal stand-in when the
// compiler does not yet support it (tested with GCC 12 / Clang 14).
// ---------------------------------------------------------------------------
#ifndef __cpp_lib_expected
namespace std {
template <class T, class E>
class expected {
    bool _has;
    union { T _val; E _err; };
public:
    expected(T v) : _has(true)  { new (&_val) T(std::move(v)); }
    struct unexpect_t {};
    expected(unexpect_t, E e) : _has(false) { new (&_err) E(std::move(e)); }
    ~expected() { if (_has) _val.~T(); else _err.~E(); }
    explicit operator bool() const noexcept { return _has; }
    bool has_value()         const noexcept { return _has; }
    T&       operator*()       &  { return _val; }
    const T& operator*() const &  { return _val; }
    T*       operator->()         { return &_val; }
    const T* operator->() const   { return &_val; }
    T&       value()              { if (!_has) throw std::runtime_error(_err); return _val; }
    const E& error() const        { return _err; }
};
template <class T, class E, class... Args>
expected<T,E> make_expected(Args&&... a) { return expected<T,E>(T(std::forward<Args>(a)...)); }
template <class E>
struct unexpected_t {
    E value;
    explicit unexpected_t(E e) : value(std::move(e)) {}
};
template <class E>
unexpected_t<E> unexpected(E e) { return unexpected_t<E>(std::move(e)); }
} // namespace std
#define LDF_UNEXPECTED(e) std::expected<ldf::LdfFile,std::string>(std::expected<ldf::LdfFile,std::string>::unexpect_t{}, e)
#else
#define LDF_UNEXPECTED(e) std::unexpected(e)
#endif

namespace ldf {

// ============================================================
//  Data structures
// ============================================================

struct Signal {
    std::string name;
    uint32_t    bit_length   = 0;
    uint64_t    init_value   = 0;          // scalar default
    std::vector<uint8_t> init_array;       // used if array init
    std::string publisher;
    std::vector<std::string> subscribers;
};

struct FrameSignalRef {
    std::string signal_name;
    uint32_t    bit_offset = 0;
};

struct Frame {
    std::string name;
    uint8_t     id        = 0;
    std::string publisher;
    uint8_t     length    = 0;             // bytes
    std::vector<FrameSignalRef> signals;
};

struct SporadicFrame {
    std::string name;
    std::vector<std::string> frames;
};

struct EventTriggeredFrame {
    std::string name;
    std::string schedule_table;
    uint8_t     frame_id        = 0;
    std::vector<std::string> frames;
};

struct DiagnosticFrame {
    std::string name;
    uint8_t     id = 0;
    std::vector<FrameSignalRef> signals;
};

// ---------- Schedule table entries ----------

struct AssignFrameIdRangeCmd {
    std::string node_name;
    uint32_t    start_index = 0;
    std::vector<uint8_t> pids; // up to 4
};

struct AssignNadCmd      { std::string node_name; };
struct AssignFrameIdCmd  { std::string node_name; std::string frame_name; };
struct FreeRangeCmd      { std::string node_name; uint8_t start_pid = 0; uint8_t end_pid = 0; };
struct UnconditionalCmd  { std::string frame_name; };
struct SporadicCmd       { std::string frame_name; };
struct EventTrigCmd      { std::string frame_name; };
struct MasterReqCmd      {};
struct SlaveRespCmd      {};
struct DataDumpCmd       { std::string node_name; std::vector<uint8_t> data; };
struct SaveConfigCmd     { std::string node_name; };
struct ConditionalCfgCmd {};

using ScheduleEntry = std::variant<
    UnconditionalCmd, SporadicCmd, EventTrigCmd,
    MasterReqCmd, SlaveRespCmd,
    AssignNadCmd, AssignFrameIdCmd, AssignFrameIdRangeCmd,
    FreeRangeCmd, DataDumpCmd, SaveConfigCmd, ConditionalCfgCmd
>;

struct ScheduleTableEntry {
    ScheduleEntry command;
    double        delay_s = 0.0;   // in seconds
};

struct ScheduleTable {
    std::string name;
    std::vector<ScheduleTableEntry> entries;
};

// ---------- Signal encoding ----------

struct LogicalValue {
    uint32_t    signal_value = 0;
    std::string text;
};

struct PhysicalRange {
    double      min_value   = 0;
    double      max_value   = 0;
    double      scale       = 1;
    double      offset      = 0;
    std::string unit;
};

struct BcdValue  {};
struct AsciiValue{};

using EncodingValue = std::variant<LogicalValue, PhysicalRange, BcdValue, AsciiValue>;

struct SignalEncodingType {
    std::string name;
    std::vector<EncodingValue> values;
};

// ---------- Node attributes ----------

struct ProductId {
    uint16_t supplier_id  = 0;
    uint16_t function_id  = 0;
    uint8_t  variant      = 0;
};

struct ConfigurableFrame {
    std::string name;
    std::optional<uint8_t> message_id;
};

struct NodeAttribute {
    std::string name;
    uint8_t     nad            = 0;
    std::optional<uint8_t> configured_nad;
    std::optional<uint8_t> initial_nad;
    std::optional<ProductId> product_id;
    std::optional<std::string> response_error_signal;
    double      p2_min_s       = 0;
    double      st_min_s       = 0;
    double      n_as_timeout_s = 0;
    double      n_cr_timeout_s = 0;
    std::vector<ConfigurableFrame> configurable_frames;
};

// ---------- Top-level ----------

struct Nodes {
    std::string              master;
    double                   master_time_base_s  = 0;
    double                   master_jitter_s     = 0;
    std::vector<std::string> slaves;
};

struct LdfFile {
    std::string lin_protocol_version;
    std::string lin_language_version;
    double      lin_speed_bps     = 19200;  // default 19.2 kbaud
    std::string channel_name;

    Nodes       nodes;
    std::vector<Signal>              signals;
    std::vector<Frame>               frames;
    std::vector<SporadicFrame>       sporadic_frames;
    std::vector<EventTriggeredFrame> event_triggered_frames;
    std::vector<DiagnosticFrame>     diagnostic_frames;
    std::vector<ScheduleTable>       schedule_tables;
    std::vector<SignalEncodingType>  signal_encoding_types;
    std::unordered_map<std::string, std::vector<std::string>> signal_representation; // encoding → signals

    std::vector<NodeAttribute>       node_attributes;

    // helpers
    const Signal*  find_signal(std::string_view n) const noexcept {
        for (auto& s : signals) if (s.name == n) return &s;
        return nullptr;
    }
    const Frame*   find_frame (std::string_view n) const noexcept {
        for (auto& f : frames)  if (f.name == n) return &f;
        return nullptr;
    }
};

// ============================================================
//  Lexer / tokeniser helpers
// ============================================================

namespace detail {

struct ParseError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ---------- character utilities ----------

static bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_hex_digit(char c) { return is_digit(c) || (c>='a'&&c<='f') || (c>='A'&&c<='F'); }
static bool is_alnum(char c) { return is_alpha(c) || is_digit(c); }
static bool is_ident(char c) { return is_alnum(c) || c == '_' || c == '-' || c == '.'; }

// ============================================================
//  Parser state
// ============================================================

class Parser {
public:
    explicit Parser(std::string src)
        : src_(std::move(src)), pos_(0), line_(1) {}

    LdfFile parse() {
        LdfFile ldf;
        skip_ws_comments();

        // The file MUST begin with "LIN_description_file"
        if (!try_keyword("LIN_description_file"))
            throw ParseError("Expected 'LIN_description_file' at start");
        expect(';');
        skip_ws_comments();

        while (pos_ < src_.size()) {
            std::string kw = read_identifier();
            skip_ws_comments();

            if (kw == "LIN_protocol_version")   { expect('='); ldf.lin_protocol_version = read_quoted_string(); expect(';'); }
            else if (kw == "LIN_language_version"){ expect('='); ldf.lin_language_version = read_quoted_string(); expect(';'); }
            else if (kw == "LIN_speed")          { expect('='); ldf.lin_speed_bps = read_speed(); expect(';'); }
            else if (kw == "Channel_name")       { expect('='); ldf.channel_name = read_quoted_string(); expect(';'); }
            else if (kw == "Nodes")              { parse_nodes(ldf.nodes); }
            else if (kw == "Signals")            { parse_signals(ldf.signals); }
            else if (kw == "Frames")             { parse_frames(ldf.frames); }
            else if (kw == "Sporadic_frames")    { parse_sporadic_frames(ldf.sporadic_frames); }
            else if (kw == "Event_triggered_frames") { parse_event_triggered_frames(ldf.event_triggered_frames); }
            else if (kw == "Diagnostic_frames")  { parse_diagnostic_frames(ldf.diagnostic_frames); }
            else if (kw == "Schedule_tables")    { parse_schedule_tables(ldf.schedule_tables); }
            else if (kw == "Signal_encoding_types") { parse_signal_encoding_types(ldf.signal_encoding_types); }
            else if (kw == "Signal_representation") { parse_signal_representation(ldf.signal_representation); }
            else if (kw == "Node_attributes")    { parse_node_attributes(ldf.node_attributes); }
            else {
                // Unknown section – skip until matching '}'
                skip_unknown_block();
            }
            skip_ws_comments();
        }
        return ldf;
    }

private:
    std::string src_;
    size_t      pos_;
    int         line_;

    // --------------------------------------------------------
    //  Low-level helpers
    // --------------------------------------------------------

    char peek(size_t off = 0) const {
        size_t i = pos_ + off;
        return i < src_.size() ? src_[i] : '\0';
    }
    char advance() {
        char c = src_[pos_++];
        if (c == '\n') ++line_;
        return c;
    }
    bool at_end() const { return pos_ >= src_.size(); }

    void skip_ws_comments() {
        while (!at_end()) {
            // whitespace
            if (is_ws(peek())) { advance(); continue; }
            // C++ line comment
            if (peek() == '/' && peek(1) == '/') {
                while (!at_end() && peek() != '\n') advance();
                continue;
            }
            // C block comment
            if (peek() == '/' && peek(1) == '*') {
                advance(); advance();
                while (!at_end()) {
                    if (peek() == '*' && peek(1) == '/') { advance(); advance(); break; }
                    advance();
                }
                continue;
            }
            break;
        }
    }

    void expect(char c) {
        skip_ws_comments();
        if (at_end() || peek() != c)
            throw ParseError(std::format("Line {}: expected '{}' got '{}'", line_, c, at_end() ? '\0' : peek()));
        advance();
        skip_ws_comments();
    }

    bool try_char(char c) {
        skip_ws_comments();
        if (!at_end() && peek() == c) { advance(); skip_ws_comments(); return true; }
        return false;
    }

    bool try_keyword(std::string_view kw) {
        skip_ws_comments();
        if (src_.compare(pos_, kw.size(), kw) == 0 &&
            (pos_ + kw.size() >= src_.size() || !is_ident(src_[pos_ + kw.size()])))
        {
            pos_ += kw.size();
            skip_ws_comments();
            return true;
        }
        return false;
    }

    std::string read_identifier() {
        skip_ws_comments();
        if (at_end() || !is_ident(peek()))
            throw ParseError(std::format("Line {}: expected identifier got '{}'", line_, peek()));
        std::string id;
        while (!at_end() && is_ident(peek())) id += advance();
        skip_ws_comments();
        return id;
    }

    std::string read_quoted_string() {
        skip_ws_comments();
        // Accept both "2.1" and 2.1 / 2_1 / identifier
        if (peek() == '"') {
            advance();
            std::string s;
            while (!at_end() && peek() != '"') {
                char c = advance();
                if (c == '\\' && !at_end()) s += advance();
                else s += c;
            }
            if (at_end()) throw ParseError("Unterminated string literal");
            advance(); // closing "
            skip_ws_comments();
            return s;
        }
        // unquoted: read until whitespace, ';', ',', '{'
        if (at_end() || (!is_ident(peek()) && peek() != '-' && peek() != '+'))
            throw ParseError(std::format("Line {}: expected string or identifier, got '{}'", line_, peek()));
        std::string s;
        while (!at_end() && peek() != ';' && peek() != ',' && peek() != '{' && !is_ws(peek()))
            s += advance();
        skip_ws_comments();
        return s;
    }

    // --------------------------------------------------------
    //  Number / unit parsing
    // --------------------------------------------------------

    // Read a raw numeric string (decimal / hex / binary / float).
    // Returns the value as double and whether it had a decimal point.
    double read_raw_number() {
        skip_ws_comments();
        if (at_end()) throw ParseError(std::format("Line {}: expected number", line_));

        // hex
        if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
            pos_ += 2;
            if (!is_hex_digit(peek()))
                throw ParseError(std::format("Line {}: invalid hex literal", line_));
            std::string s;
            while (is_hex_digit(peek())) s += advance();
            uint64_t v = std::stoull(s, nullptr, 16);
            return static_cast<double>(v);
        }
        // binary
        if (peek() == '0' && (peek(1) == 'b' || peek(1) == 'B')) {
            pos_ += 2;
            std::string s;
            while (peek() == '0' || peek() == '1') s += advance();
            if (s.empty()) throw ParseError(std::format("Line {}: invalid binary literal", line_));
            return static_cast<double>(std::stoull(s, nullptr, 2));
        }
        // decimal / float
        std::string s;
        if (peek() == '-' || peek() == '+') s += advance();
        while (!at_end() && (is_digit(peek()) || peek() == '.')) s += advance();
        // exponent
        if (!at_end() && (peek() == 'e' || peek() == 'E')) {
            s += advance();
            if (!at_end() && (peek() == '+' || peek() == '-')) s += advance();
            while (!at_end() && is_digit(peek())) s += advance();
        }
        double v = 0.0;
        std::from_chars(s.data(), s.data() + s.size(), v);
        return v;
    }

    // Multiplier suffix:  k/K → 1e3,  M → 1e6,  (none) → 1
    double read_multiplier_suffix() {
        if (at_end()) return 1.0;
        char c = peek();
        if (c == 'k' || c == 'K') { advance(); return 1e3; }
        if (c == 'M')              { advance(); return 1e6; }
        return 1.0;
    }

    // Read a speed value → always returns bps as double
    // Accepts: 19200  /  19.2k  /  19200 bps  /  19.2 kbps
    double read_speed() {
        double v   = read_raw_number();
        double mul = read_multiplier_suffix();
        v *= mul;
        // optional unit: bps / kbps
        skip_ws_comments();
        if (try_keyword("kbps")) v *= 1e3;
        else try_keyword("bps");
        return v;
    }

    // Read a time value → returns seconds
    // Accepts: 5  /  5ms  /  5 ms  /  500us  /  500 us  /  500µs
    double read_time_s() {
        double v = read_raw_number();
        skip_ws_comments();
        // suffix (no whitespace between value and suffix in many LDF files)
        if (try_keyword("ms"))  return v * 1e-3;
        if (try_keyword("us"))  return v * 1e-6;
        // µ is multi-byte UTF-8: 0xC2 0xB5
        if ((unsigned char)peek() == 0xC2 && (unsigned char)peek(1) == 0xB5) {
            pos_ += 2;
            skip_ws_comments();
            try_keyword("s");
            return v * 1e-6;
        }
        if (try_keyword("s"))   return v;
        return v; // assume seconds
    }

    // Read an integer (hex / binary / decimal)
    uint64_t read_integer() {
        return static_cast<uint64_t>(read_raw_number());
    }

    // --------------------------------------------------------
    //  Block helpers
    // --------------------------------------------------------

    void skip_unknown_block() {
        // skip until '{' then match braces
        while (!at_end() && peek() != '{') advance();
        if (at_end()) return;
        int depth = 0;
        while (!at_end()) {
            char c = advance();
            if (c == '{') ++depth;
            else if (c == '}') { if (--depth == 0) break; }
        }
        skip_ws_comments();
    }

    // --------------------------------------------------------
    //  Section parsers
    // --------------------------------------------------------

    // --- Nodes ---
    void parse_nodes(Nodes& n) {
        expect('{');
        while (!at_end() && peek() != '}') {
            std::string kw = read_identifier();
            if (kw == "Master") {
                expect(':');
                n.master = read_identifier();
                expect(',');
                n.master_time_base_s = read_time_s();
                expect(',');
                n.master_jitter_s    = read_time_s();
                expect(';');
            } else if (kw == "Slaves") {
                expect(':');
                while (true) {
                    n.slaves.push_back(read_identifier());
                    if (peek() == ',') { advance(); skip_ws_comments(); }
                    else { expect(';'); break; }
                }
            } else {
                // unknown field – skip to ';'
                while (!at_end() && peek() != ';') advance();
                try_char(';');
            }
            skip_ws_comments();
        }
        expect('}');
    }

    // --- Signals ---
    void parse_signals(std::vector<Signal>& sigs) {
        expect('{');
        skip_ws_comments();
        while (!at_end() && peek() != '}') {
            Signal s;
            s.name       = read_identifier();
            expect(':');
            s.bit_length = static_cast<uint32_t>(read_integer());
            expect(',');

            // init value: scalar or { b0, b1, … }
            if (peek() == '{') {
                advance(); skip_ws_comments();
                while (true) {
                    s.init_array.push_back(static_cast<uint8_t>(read_integer()));
                    if (peek() == ',') { advance(); skip_ws_comments(); }
                    else { expect('}'); break; }
                }
            } else {
                s.init_value = read_integer();
            }
            expect(',');
            s.publisher = read_identifier();
            // optional subscriber list
            while (peek() == ',') {
                advance(); skip_ws_comments();
                s.subscribers.push_back(read_identifier());
            }
            expect(';');
            sigs.push_back(std::move(s));
            skip_ws_comments();
        }
        expect('}');
    }

    // --- Frames ---
    void parse_frames(std::vector<Frame>& frames) {
        expect('{');
        skip_ws_comments();
        while (!at_end() && peek() != '}') {
            Frame f;
            f.name      = read_identifier();
            expect(':');
            f.id        = static_cast<uint8_t>(read_integer());
            expect(',');
            f.publisher = read_identifier();
            expect(',');
            f.length    = static_cast<uint8_t>(read_integer());
            expect('{');
            skip_ws_comments();
            while (!at_end() && peek() != '}') {
                FrameSignalRef ref;
                ref.signal_name = read_identifier();
                expect(',');
                ref.bit_offset  = static_cast<uint32_t>(read_integer());
                expect(';');
                f.signals.push_back(ref);
                skip_ws_comments();
            }
            expect('}');
            frames.push_back(std::move(f));
            skip_ws_comments();
        }
        expect('}');
    }

    // --- Sporadic frames ---
    void parse_sporadic_frames(std::vector<SporadicFrame>& sf) {
        expect('{');
        skip_ws_comments();
        while (!at_end() && peek() != '}') {
            SporadicFrame s;
            s.name = read_identifier();
            expect(':');
            while (true) {
                s.frames.push_back(read_identifier());
                if (peek() == ',') { advance(); skip_ws_comments(); }
                else { expect(';'); break; }
            }
            sf.push_back(std::move(s));
            skip_ws_comments();
        }
        expect('}');
    }

    // --- Event triggered frames ---
    void parse_event_triggered_frames(std::vector<EventTriggeredFrame>& etf) {
        expect('{');
        skip_ws_comments();
        while (!at_end() && peek() != '}') {
            EventTriggeredFrame e;
            e.name           = read_identifier();
            expect(':');
            e.schedule_table = read_identifier();
            expect(',');
            e.frame_id       = static_cast<uint8_t>(read_integer());
            expect(',');
            while (true) {
                e.frames.push_back(read_identifier());
                if (peek() == ',') { advance(); skip_ws_comments(); }
                else { expect(';'); break; }
            }
            etf.push_back(std::move(e));
            skip_ws_comments();
        }
        expect('}');
    }

    // --- Diagnostic frames ---
    void parse_diagnostic_frames(std::vector<DiagnosticFrame>& df) {
        expect('{');
        skip_ws_comments();
        while (!at_end() && peek() != '}') {
            DiagnosticFrame d;
            d.name = read_identifier();
            expect(':');
            d.id   = static_cast<uint8_t>(read_integer());
            expect('{');
            skip_ws_comments();
            while (!at_end() && peek() != '}') {
                FrameSignalRef r;
                r.signal_name = read_identifier();
                expect(',');
                r.bit_offset  = static_cast<uint32_t>(read_integer());
                expect(';');
                d.signals.push_back(r);
                skip_ws_comments();
            }
            expect('}');
            df.push_back(std::move(d));
            skip_ws_comments();
        }
        expect('}');
    }

    // --- Schedule tables ---
    void parse_schedule_tables(std::vector<ScheduleTable>& tables) {
        expect('{');
        skip_ws_comments();
        while (!at_end() && peek() != '}') {
            ScheduleTable t;
            t.name = read_identifier();
            expect('{');
            skip_ws_comments();
            while (!at_end() && peek() != '}') {
                ScheduleTableEntry e;
                std::string cmd = read_identifier();

                if (cmd == "MasterReq")        e.command = MasterReqCmd{};
                else if (cmd == "SlaveResp")   e.command = SlaveRespCmd{};
                else if (cmd == "AssignNAD") {
                    expect('{');
                    AssignNadCmd c; c.node_name = read_identifier(); expect('}');
                    e.command = c;
                }
                else if (cmd == "AssignFrameIdRange") {
                    expect('{');
                    AssignFrameIdRangeCmd c;
                    c.node_name   = read_identifier(); expect(',');
                    c.start_index = static_cast<uint32_t>(read_integer());
                    while (peek() == ',') {
                        advance(); skip_ws_comments();
                        c.pids.push_back(static_cast<uint8_t>(read_integer()));
                    }
                    expect('}');
                    e.command = c;
                }
                else if (cmd == "AssignFrameId") {
                    expect('{');
                    AssignFrameIdCmd c;
                    c.node_name  = read_identifier(); expect(',');
                    c.frame_name = read_identifier(); expect('}');
                    e.command = c;
                }
                else if (cmd == "FreeFormat") {
                    expect('{');
                    DataDumpCmd c;
                    c.node_name = read_identifier();
                    while (peek() == ',') {
                        advance(); skip_ws_comments();
                        c.data.push_back(static_cast<uint8_t>(read_integer()));
                    }
                    expect('}');
                    e.command = c;
                }
                else if (cmd == "DataDump") {
                    expect('{');
                    DataDumpCmd c;
                    c.node_name = read_identifier();
                    while (peek() == ',') {
                        advance(); skip_ws_comments();
                        c.data.push_back(static_cast<uint8_t>(read_integer()));
                    }
                    expect('}');
                    e.command = c;
                }
                else if (cmd == "SaveConfiguration") {
                    expect('{');
                    SaveConfigCmd c; c.node_name = read_identifier(); expect('}');
                    e.command = c;
                }
                else if (cmd == "ConditionalChangeNAD") {
                    // skip args
                    expect('{');
                    while (!at_end() && peek() != '}') advance();
                    expect('}');
                    e.command = ConditionalCfgCmd{};
                }
                else {
                    // Treat as unconditional frame name
                    e.command = UnconditionalCmd{ cmd };
                }

                // delay
                if (!try_keyword("delay"))
                    throw ParseError(std::format("Line {}: expected 'delay'", line_));
                e.delay_s = read_time_s();
                expect(';');
                t.entries.push_back(std::move(e));
                skip_ws_comments();
            }
            expect('}');
            tables.push_back(std::move(t));
            skip_ws_comments();
        }
        expect('}');
    }

    // --- Signal encoding types ---
    void parse_signal_encoding_types(std::vector<SignalEncodingType>& enc) {
        expect('{');
        skip_ws_comments();
        while (!at_end() && peek() != '}') {
            SignalEncodingType t;
            t.name = read_identifier();
            expect('{');
            skip_ws_comments();
            while (!at_end() && peek() != '}') {
                std::string kind = read_identifier();
                if (kind == "logical_value") {
                    expect(',');
                    LogicalValue lv;
                    lv.signal_value = static_cast<uint32_t>(read_integer());
                    if (peek() == ',') {
                        advance(); skip_ws_comments();
                        lv.text = read_quoted_string();
                    }
                    expect(';');
                    t.values.push_back(lv);
                } else if (kind == "physical_value") {
                    expect(',');
                    PhysicalRange pr;
                    pr.min_value = read_raw_number(); expect(',');
                    pr.max_value = read_raw_number(); expect(',');
                    pr.scale     = read_raw_number(); expect(',');
                    pr.offset    = read_raw_number();
                    if (peek() == ',') {
                        advance(); skip_ws_comments();
                        pr.unit = read_quoted_string();
                    }
                    expect(';');
                    t.values.push_back(pr);
                } else if (kind == "bcd_value") {
                    expect(';');
                    t.values.push_back(BcdValue{});
                } else if (kind == "ascii_value") {
                    expect(';');
                    t.values.push_back(AsciiValue{});
                } else {
                    while (!at_end() && peek() != ';') advance();
                    try_char(';');
                }
                skip_ws_comments();
            }
            expect('}');
            enc.push_back(std::move(t));
            skip_ws_comments();
        }
        expect('}');
    }

    // --- Signal representation ---
    void parse_signal_representation(std::unordered_map<std::string, std::vector<std::string>>& rep) {
        expect('{');
        skip_ws_comments();
        while (!at_end() && peek() != '}') {
            std::string enc = read_identifier();
            expect(':');
            while (true) {
                rep[enc].push_back(read_identifier());
                if (peek() == ',') { advance(); skip_ws_comments(); }
                else { expect(';'); break; }
            }
            skip_ws_comments();
        }
        expect('}');
    }

    // --- Node attributes ---
    void parse_node_attributes(std::vector<NodeAttribute>& attrs) {
        expect('{');
        skip_ws_comments();
        while (!at_end() && peek() != '}') {
            NodeAttribute a;
            a.name = read_identifier();
            expect('{');
            skip_ws_comments();
            while (!at_end() && peek() != '}') {
                std::string kw = read_identifier();
                if (kw == "LIN_protocol") {
                    expect('=');
                    read_quoted_string(); // version string – discarded
                    expect(';');
                } else if (kw == "configured_NAD") {
                    expect('=');
                    a.configured_nad = static_cast<uint8_t>(read_integer());
                    expect(';');
                } else if (kw == "initial_NAD") {
                    expect('=');
                    a.initial_nad = static_cast<uint8_t>(read_integer());
                    expect(';');
                } else if (kw == "NAD") {
                    expect('=');
                    a.nad = static_cast<uint8_t>(read_integer());
                    expect(';');
                } else if (kw == "product_id") {
                    expect('=');
                    ProductId pid;
                    pid.supplier_id = static_cast<uint16_t>(read_integer()); expect(',');
                    pid.function_id = static_cast<uint16_t>(read_integer());
                    if (peek() == ',') { advance(); skip_ws_comments(); pid.variant = static_cast<uint8_t>(read_integer()); }
                    a.product_id = pid;
                    expect(';');
                } else if (kw == "response_error") {
                    expect('=');
                    a.response_error_signal = read_identifier();
                    expect(';');
                } else if (kw == "P2_min") {
                    expect('=');
                    a.p2_min_s = read_time_s();
                    expect(';');
                } else if (kw == "ST_min") {
                    expect('=');
                    a.st_min_s = read_time_s();
                    expect(';');
                } else if (kw == "N_As_timeout") {
                    expect('=');
                    a.n_as_timeout_s = read_time_s();
                    expect(';');
                } else if (kw == "N_Cr_timeout") {
                    expect('=');
                    a.n_cr_timeout_s = read_time_s();
                    expect(';');
                } else if (kw == "configurable_frames") {
                    expect('{');
                    skip_ws_comments();
                    while (!at_end() && peek() != '}') {
                        ConfigurableFrame cf;
                        cf.name = read_identifier();
                        if (peek() == '=') {
                            advance(); skip_ws_comments();
                            cf.message_id = static_cast<uint8_t>(read_integer());
                        }
                        expect(';');
                        a.configurable_frames.push_back(cf);
                        skip_ws_comments();
                    }
                    expect('}');
                } else if (kw == "frames") {
                    // alias for configurable_frames in some tools
                    expect('{');
                    skip_ws_comments();
                    while (!at_end() && peek() != '}') {
                        ConfigurableFrame cf;
                        cf.name = read_identifier();
                        if (peek() == '=') {
                            advance(); skip_ws_comments();
                            cf.message_id = static_cast<uint8_t>(read_integer());
                        }
                        expect(';');
                        a.configurable_frames.push_back(cf);
                        skip_ws_comments();
                    }
                    expect('}');
                } else {
                    // skip unknown field
                    while (!at_end() && peek() != ';' && peek() != '}') advance();
                    try_char(';');
                }
                skip_ws_comments();
            }
            expect('}');
            attrs.push_back(std::move(a));
            skip_ws_comments();
        }
        expect('}');
    }
};

} // namespace detail

// ============================================================
//  Public API
// ============================================================

/// Parse an LDF string.
/// Returns a populated LdfFile on success, or an error message.
[[nodiscard]]
inline std::expected<LdfFile, std::string> parse(const std::string& source)
{
    try {
        detail::Parser p(source);
        return p.parse();
    } catch (const detail::ParseError& e) {
        return LDF_UNEXPECTED(std::string(e.what()));
    } catch (const std::exception& e) {
        return LDF_UNEXPECTED(std::string("Exception: ") + e.what());
    }
}

/// Parse an LDF file by path.
[[nodiscard]]
inline std::expected<LdfFile, std::string> parse_file(const std::string& path)
{
    std::ifstream f(path, std::ios::in);
    if (!f)
        return LDF_UNEXPECTED("Cannot open file: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return parse(ss.str());
}

// ============================================================
//  Pretty-printer  (optional, useful for debugging)
// ============================================================

inline std::string to_string(const LdfFile& ldf)
{
    std::ostringstream o;
    o << "LDF File\n"
      << "  Protocol : " << ldf.lin_protocol_version << "\n"
      << "  Language : " << ldf.lin_language_version << "\n"
      << "  Speed    : " << ldf.lin_speed_bps        << " bps\n"
      << "  Channel  : " << ldf.channel_name         << "\n\n";

    o << "Nodes\n"
      << "  Master : " << ldf.nodes.master
      << "  (time_base=" << ldf.nodes.master_time_base_s*1e3 << " ms"
      << ", jitter="     << ldf.nodes.master_jitter_s *1e3 << " ms)\n"
      << "  Slaves :";
    for (auto& s : ldf.nodes.slaves) o << " " << s;
    o << "\n\n";

    o << "Signals (" << ldf.signals.size() << ")\n";
    for (auto& s : ldf.signals) {
        o << "  " << s.name << "  bits=" << s.bit_length
          << "  init=";
        if (!s.init_array.empty()) {
            o << '{';
            for (size_t i = 0; i < s.init_array.size(); ++i)
                o << (i?",":"") << (int)s.init_array[i];
            o << '}';
        } else o << s.init_value;
        o << "  pub=" << s.publisher << "\n";
    }

    o << "\nFrames (" << ldf.frames.size() << ")\n";
    for (auto& f : ldf.frames) {
        o << "  " << f.name << "  id=0x" << std::hex << (int)f.id << std::dec
          << "  pub=" << f.publisher << "  len=" << (int)f.length << "\n";
        for (auto& r : f.signals)
            o << "    " << r.signal_name << " @ bit " << r.bit_offset << "\n";
    }

    o << "\nSchedule tables (" << ldf.schedule_tables.size() << ")\n";
    for (auto& t : ldf.schedule_tables) {
        o << "  " << t.name << "\n";
        for (auto& e : t.entries) {
            o << "    ";
            std::visit([&o](auto&& cmd) {
                using T = std::decay_t<decltype(cmd)>;
                if constexpr (std::is_same_v<T, UnconditionalCmd>)    o << "Frame("  << cmd.frame_name << ")";
                else if constexpr (std::is_same_v<T, MasterReqCmd>)   o << "MasterReq";
                else if constexpr (std::is_same_v<T, SlaveRespCmd>)   o << "SlaveResp";
                else if constexpr (std::is_same_v<T, AssignNadCmd>)   o << "AssignNAD("  << cmd.node_name << ")";
                else if constexpr (std::is_same_v<T, SaveConfigCmd>)  o << "SaveConfig(" << cmd.node_name << ")";
                else o << "(other)";
            }, e.command);
            o << "  delay=" << e.delay_s * 1e3 << " ms\n";
        }
    }

    return o.str();
}

} // namespace ldf
