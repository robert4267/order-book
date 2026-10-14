#pragma once

// NASDAQ TotalView-ITCH 5.0 decoder.
//
// Works on the framing used by NASDAQ's binary sample files: every message is
// preceded by a 2-byte big-endian length. Fields are memcpy'd straight from the
// caller's buffer: no allocation, no alignment requirement.
//
// Only the messages needed to build books are decoded. Everything else is
// counted and skipped using the length prefix.

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>

namespace lob::itch {

namespace detail {

// std::byteswap is C++23. GCC doesn't turn the portable shift loop into a
// single bswap for 64-bit loads, so use the builtin where there is one.
template <class T>
constexpr T byteswap(T v) noexcept {
    static_assert(std::is_unsigned_v<T>);
    if constexpr (sizeof(T) == 1) {
        return v;
    } else {
#if defined(__GNUC__) || defined(__clang__)
        if constexpr (sizeof(T) == 2) return __builtin_bswap16(v);
        else if constexpr (sizeof(T) == 4) return __builtin_bswap32(v);
        else if constexpr (sizeof(T) == 8) return __builtin_bswap64(v);
#endif
        T r = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            r = static_cast<T>((r << 8) | (v & 0xFF));
            v = static_cast<T>(v >> 8);
        }
        return r;
    }
}

template <class T>
inline T load_be(const char* p) noexcept {
    T v;
    std::memcpy(&v, p, sizeof(T));
    if constexpr (std::endian::native == std::endian::little) v = byteswap(v);
    return v;
}

// Timestamps are 6 bytes: nanoseconds since midnight.
inline std::uint64_t load_be48(const char* p) noexcept {
    return (std::uint64_t{load_be<std::uint16_t>(p)} << 32) | load_be<std::uint32_t>(p + 2);
}

}  // namespace detail

using Symbol = std::array<char, 8>;  // space padded on the wire

inline std::string_view trim(const Symbol& s) noexcept {
    std::size_t n = s.size();
    while (n > 0 && s[n - 1] == ' ') --n;
    return {s.data(), n};
}

// Header fields shared by every message.
struct Header {
    char type;
    std::uint16_t locate;  // per-day id of the instrument, see StockDirectory
    std::uint16_t tracking;
    std::uint64_t timestamp;  // ns since midnight
};

struct SystemEvent {  // 'S'
    Header h;
    char event;  // O S Q M E C
};

struct StockDirectory {  // 'R'
    Header h;
    Symbol stock;
    char market_category;
    char financial_status;
    std::uint32_t round_lot;
};

struct AddOrder {  // 'A' and 'F'; mpid is blank for 'A'
    Header h;
    std::uint64_t ref;
    char side;  // 'B' or 'S'
    std::uint32_t shares;
    Symbol stock;
    std::uint32_t price;  // 4 implied decimals
    std::array<char, 4> mpid;
};

struct OrderExecuted {  // 'E' and 'C'; price only meaningful for 'C'
    Header h;
    std::uint64_t ref;
    std::uint32_t shares;
    std::uint64_t match;
    bool printable;
    std::uint32_t price;
};

struct OrderCancel {  // 'X', partial cancel
    Header h;
    std::uint64_t ref;
    std::uint32_t shares;
};

struct OrderDelete {  // 'D'
    Header h;
    std::uint64_t ref;
};

struct OrderReplace {  // 'U'; new order keeps side and symbol, loses priority
    Header h;
    std::uint64_t old_ref;
    std::uint64_t new_ref;
    std::uint32_t shares;
    std::uint32_t price;
};

// Wire size of each message type including the type byte, 0 if the type isn't
// part of ITCH 5.0.
constexpr std::size_t message_size(char type) noexcept {
    switch (type) {
        case 'S': return 12;
        case 'R': return 39;
        case 'H': return 25;
        case 'Y': return 20;
        case 'L': return 26;
        case 'V': return 35;
        case 'W': return 12;
        case 'K': return 28;
        case 'J': return 35;
        case 'h': return 21;
        case 'A': return 36;
        case 'F': return 40;
        case 'E': return 31;
        case 'C': return 36;
        case 'X': return 23;
        case 'D': return 19;
        case 'U': return 35;
        case 'P': return 44;
        case 'Q': return 40;
        case 'B': return 19;
        case 'I': return 50;
        case 'N': return 20;
        case 'O': return 48;
        default: return 0;
    }
}

enum class Error : std::uint8_t {
    None,
    Truncated,  // buffer ends inside a frame; more data may follow
    BadLength,  // frame shorter than its message type requires
};

struct Result {
    std::size_t consumed = 0;  // bytes of complete frames processed
    std::size_t messages = 0;
    Error error = Error::None;
};

namespace detail {

inline Header header(const char* p) noexcept {
    return {p[0], load_be<std::uint16_t>(p + 1), load_be<std::uint16_t>(p + 3), load_be48(p + 5)};
}

template <class H, class M>
inline void emit(H& h, const M& m) {
    if constexpr (requires { h.on(m); }) h.on(m);
}

template <class H>
inline void dispatch(H& h, const char* p) {
    switch (p[0]) {
        case 'S': emit(h, SystemEvent{header(p), p[11]}); break;
        case 'R': {
            StockDirectory m{header(p), {}, p[19], p[20], load_be<std::uint32_t>(p + 21)};
            std::memcpy(m.stock.data(), p + 11, 8);
            emit(h, m);
            break;
        }
        case 'A':
        case 'F': {
            AddOrder m{header(p),
                       load_be<std::uint64_t>(p + 11),
                       p[19],
                       load_be<std::uint32_t>(p + 20),
                       {},
                       load_be<std::uint32_t>(p + 32),
                       {' ', ' ', ' ', ' '}};
            std::memcpy(m.stock.data(), p + 24, 8);
            if (p[0] == 'F') std::memcpy(m.mpid.data(), p + 36, 4);
            emit(h, m);
            break;
        }
        case 'E':
            emit(h, OrderExecuted{header(p), load_be<std::uint64_t>(p + 11),
                                  load_be<std::uint32_t>(p + 19), load_be<std::uint64_t>(p + 23),
                                  true, 0});
            break;
        case 'C':
            emit(h, OrderExecuted{header(p), load_be<std::uint64_t>(p + 11),
                                  load_be<std::uint32_t>(p + 19), load_be<std::uint64_t>(p + 23),
                                  p[31] == 'Y', load_be<std::uint32_t>(p + 32)});
            break;
        case 'X':
            emit(h, OrderCancel{header(p), load_be<std::uint64_t>(p + 11),
                                load_be<std::uint32_t>(p + 19)});
            break;
        case 'D': emit(h, OrderDelete{header(p), load_be<std::uint64_t>(p + 11)}); break;
        case 'U':
            emit(h, OrderReplace{header(p), load_be<std::uint64_t>(p + 11),
                                 load_be<std::uint64_t>(p + 19), load_be<std::uint32_t>(p + 27),
                                 load_be<std::uint32_t>(p + 31)});
            break;
        default: break;
    }
}

}  // namespace detail

// Decodes every complete frame in [data, data + size) and calls h.on(msg) for
// the message structs above that the handler accepts. If the handler has
// on_message(char type) it's called for every frame first, known or not.
//
// Stops at the first incomplete frame with Error::Truncated so a caller
// reading in chunks can carry the tail over; `consumed` says where to resume.
// A frame that is too short for its type stops with Error::BadLength. Longer
// frames and unknown types are accepted and skipped, empty frames ignored.
template <class H>
Result parse(const char* data, std::size_t size, H& h) {
    Result r;
    while (size - r.consumed >= 2) {
        const char* p = data + r.consumed;
        const std::size_t len = detail::load_be<std::uint16_t>(p);
        if (size - r.consumed - 2 < len) {
            r.error = Error::Truncated;
            return r;
        }
        p += 2;
        // BinaryFILE marks the end of a session with an empty frame
        if (len == 0) {
            r.consumed += 2;
            continue;
        }
        if (len < message_size(p[0])) {
            r.error = Error::BadLength;
            return r;
        }
        if constexpr (requires { h.on_message(p[0]); }) h.on_message(p[0]);
        detail::dispatch(h, p);
        r.consumed += 2 + len;
        ++r.messages;
    }
    if (r.consumed != size) r.error = Error::Truncated;
    return r;
}

// Encoder for the same framing, used by the tests and the synthetic file
// generator. Not meant for the hot path.
namespace encode {

template <class T>
void put(std::string& out, T v) {
    for (int shift = static_cast<int>(sizeof(T) * 8) - 8; shift >= 0; shift -= 8)
        out.push_back(static_cast<char>((v >> shift) & 0xFF));
}

inline void put48(std::string& out, std::uint64_t v) {
    put(out, static_cast<std::uint16_t>(v >> 32));
    put(out, static_cast<std::uint32_t>(v));
}

inline void put_text(std::string& out, std::string_view s, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i) out.push_back(i < s.size() ? s[i] : ' ');
}

// Writes the length prefix and common header; body follows.
inline void begin(std::string& out, char type, std::uint16_t locate, std::uint64_t ts) {
    put(out, static_cast<std::uint16_t>(message_size(type)));
    out.push_back(type);
    put(out, locate);
    put(out, std::uint16_t{0});
    put48(out, ts);
}

inline void system_event(std::string& out, char event, std::uint64_t ts = 0) {
    begin(out, 'S', 0, ts);
    out.push_back(event);
}

inline void stock_directory(std::string& out, std::uint16_t locate, std::string_view stock,
                            std::uint64_t ts = 0) {
    begin(out, 'R', locate, ts);
    put_text(out, stock, 8);
    out += "QN";                   // market category, financial status
    put(out, std::uint32_t{100});  // round lot
    out += "NCZ PN 1N";            // flags we don't decode
    put(out, std::uint32_t{0});    // ETP leverage
    out.push_back('N');
}

inline void add_order(std::string& out, std::uint16_t locate, std::uint64_t ref, char side,
                      std::uint32_t shares, std::string_view stock, std::uint32_t price,
                      std::string_view mpid = {}, std::uint64_t ts = 0) {
    begin(out, mpid.empty() ? 'A' : 'F', locate, ts);
    put(out, ref);
    out.push_back(side);
    put(out, shares);
    put_text(out, stock, 8);
    put(out, price);
    if (!mpid.empty()) put_text(out, mpid, 4);
}

inline void executed(std::string& out, std::uint16_t locate, std::uint64_t ref,
                     std::uint32_t shares, std::uint64_t match, std::uint64_t ts = 0) {
    begin(out, 'E', locate, ts);
    put(out, ref);
    put(out, shares);
    put(out, match);
}

inline void executed_with_price(std::string& out, std::uint16_t locate, std::uint64_t ref,
                                std::uint32_t shares, std::uint64_t match, bool printable,
                                std::uint32_t price, std::uint64_t ts = 0) {
    begin(out, 'C', locate, ts);
    put(out, ref);
    put(out, shares);
    put(out, match);
    out.push_back(printable ? 'Y' : 'N');
    put(out, price);
}

inline void cancel(std::string& out, std::uint16_t locate, std::uint64_t ref, std::uint32_t shares,
                   std::uint64_t ts = 0) {
    begin(out, 'X', locate, ts);
    put(out, ref);
    put(out, shares);
}

inline void del(std::string& out, std::uint16_t locate, std::uint64_t ref, std::uint64_t ts = 0) {
    begin(out, 'D', locate, ts);
    put(out, ref);
}

inline void replace(std::string& out, std::uint16_t locate, std::uint64_t old_ref,
                    std::uint64_t new_ref, std::uint32_t shares, std::uint32_t price,
                    std::uint64_t ts = 0) {
    begin(out, 'U', locate, ts);
    put(out, old_ref);
    put(out, new_ref);
    put(out, shares);
    put(out, price);
}

// Non-displayed trade; decoded only as far as counting it.
inline void trade(std::string& out, std::uint16_t locate, char side, std::uint32_t shares,
                  std::string_view stock, std::uint32_t price, std::uint64_t match,
                  std::uint64_t ts = 0) {
    begin(out, 'P', locate, ts);
    put(out, std::uint64_t{0});
    out.push_back(side);
    put(out, shares);
    put_text(out, stock, 8);
    put(out, price);
    put(out, match);
}

}  // namespace encode

}  // namespace lob::itch
