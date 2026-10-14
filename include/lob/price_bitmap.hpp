#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace lob {

// One bit per price level, set when the level has orders. A second, smaller
// layer marks which 64-bit words are non-zero, so a scan after the best level
// empties moves 4096 levels per step instead of 64.
class PriceBitmap {
public:
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    explicit PriceBitmap(std::size_t n)
        : n_(n), words_((n + 63) / 64, 0), summary_((words_.size() + 63) / 64, 0) {}

    [[nodiscard]] std::size_t size() const noexcept { return n_; }

    [[nodiscard]] bool test(std::size_t i) const noexcept {
        return (words_[i >> 6] >> (i & 63)) & 1;
    }

    void set(std::size_t i) noexcept {
        const std::size_t w = i >> 6;
        words_[w] |= bit(i & 63);
        summary_[w >> 6] |= bit(w & 63);
    }

    void clear(std::size_t i) noexcept {
        const std::size_t w = i >> 6;
        words_[w] &= ~bit(i & 63);
        if (words_[w] == 0) summary_[w >> 6] &= ~bit(w & 63);
    }

    // lowest set index >= i
    [[nodiscard]] std::size_t next(std::size_t i) const noexcept {
        if (i >= n_) return npos;
        std::size_t w = i >> 6;
        const std::uint64_t m = words_[w] & (~0ULL << (i & 63));
        if (m) return (w << 6) + ctz(m);
        w = next_word(w + 1);
        return w == npos ? npos : (w << 6) + ctz(words_[w]);
    }

    // highest set index <= i
    [[nodiscard]] std::size_t prev(std::size_t i) const noexcept {
        if (n_ == 0) return npos;
        if (i >= n_) i = n_ - 1;
        std::size_t w = i >> 6;
        const std::uint64_t m = words_[w] & (~0ULL >> (63 - (i & 63)));
        if (m) return (w << 6) + 63 - clz(m);
        if (w == 0) return npos;
        w = prev_word(w - 1);
        return w == npos ? npos : (w << 6) + 63 - clz(words_[w]);
    }

private:
    static constexpr std::uint64_t bit(std::size_t i) { return 1ULL << i; }
    static std::size_t ctz(std::uint64_t x) noexcept {
        return static_cast<std::size_t>(std::countr_zero(x));
    }
    static std::size_t clz(std::uint64_t x) noexcept {
        return static_cast<std::size_t>(std::countl_zero(x));
    }

    std::size_t next_word(std::size_t w) const noexcept {
        std::size_t s = w >> 6;
        if (s >= summary_.size()) return npos;
        std::uint64_t m = summary_[s] & (~0ULL << (w & 63));
        while (!m) {
            if (++s == summary_.size()) return npos;
            m = summary_[s];
        }
        return (s << 6) + ctz(m);
    }

    std::size_t prev_word(std::size_t w) const noexcept {
        std::size_t s = w >> 6;
        std::uint64_t m = summary_[s] & (~0ULL >> (63 - (w & 63)));
        while (!m) {
            if (s == 0) return npos;
            m = summary_[--s];
        }
        return (s << 6) + 63 - clz(m);
    }

    std::size_t n_;
    std::vector<std::uint64_t> words_;
    std::vector<std::uint64_t> summary_;
};

}  // namespace lob
