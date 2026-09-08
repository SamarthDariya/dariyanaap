#pragma once

#include <bit>
#include <cstdint>

namespace dariyanaap::buckets {

// The histogram's layout, kept separate from the histogram so the index
// arithmetic can be tested on its own — and so kCount below is derived from
// the layout rather than written down next to it.
//
// Log-linear, in nanoseconds. Below kSubBuckets every value is its own slot
// and exact; above it, each power of two is cut into kSubBuckets slots, so a
// slot's *relative* width is constant. That constant is the error bound, and
// DESIGN.md decision 4 derives 128 from it: 1/128 = 0.781%, the only
// sub-bucket count that meets the <=1% claim without paying for precision no
// experiment in the track can use.
//
// constexpr and header-only, against the repo's .hpp/.cpp habit, for two
// reasons: index_of is called once per recorded sample, so a call through a
// translation-unit boundary would be measurement overhead; and kCount can then
// be computed by the same code that indexes, instead of being a constant that
// agrees with it by hand.

constexpr int kSubBucketBits = 7;
constexpr std::int64_t kSubBuckets = std::int64_t{1} << kSubBucketBits;  // 128
constexpr std::int64_t kMaxValue = 60'000'000'000;                       // 60s in ns

// Slot holding `value` nanoseconds. Precondition: 0 <= value <= kMaxValue.
// Out-of-range values are policy, not arithmetic, and Histogram owns that
// (chunk 1.3) — passing one here is a bug, not an error to be reported.
constexpr int index_of(std::int64_t value) {
    if (value < kSubBuckets) {
        return static_cast<int>(value);
    }
    // 63 - countl_zero is floor(log2(value)): one instruction on arm64 and x86.
    const int msb = 63 - std::countl_zero(static_cast<std::uint64_t>(value));
    const int shift = msb - kSubBucketBits;
    // value >> shift lands in [kSubBuckets, 2*kSubBuckets), so subtracting
    // kSubBuckets gives a mantissa in [0, kSubBuckets) — every octave uses
    // exactly kSubBuckets slots, which is what makes the layout gapless.
    const std::int64_t mantissa = (value >> shift) - kSubBuckets;
    return static_cast<int>(((msb - kSubBucketBits + 1) << kSubBucketBits) + mantissa);
}

// Derived, not asserted. The octave-ceiling formula would allocate 3,840 for a
// top octave that stops at 60s rather than 68.7s; this gives 3,808.
constexpr int kCount = index_of(kMaxValue) + 1;

namespace detail {

// The (exponent, mantissa) a slot decodes to. Shared so slot_low and slot_high
// cannot drift apart.
struct Decoded {
    int exponent;
    std::int64_t mantissa;
};

constexpr Decoded decode(int index) {
    const int exponent = (index >> kSubBucketBits) - 1 + kSubBucketBits;
    const std::int64_t mantissa = (index & (kSubBuckets - 1)) + kSubBuckets;
    return {exponent, mantissa};
}

}  // namespace detail

// Lowest value that lands in this slot.
constexpr std::int64_t slot_low(int index) {
    if (index < kSubBuckets) {
        return index;
    }
    const detail::Decoded d = detail::decode(index);
    return d.mantissa << (d.exponent - kSubBucketBits);
}

// Highest value that lands in this slot. This is the value percentile()
// reports (chunk 1.4), so a reported latency is never lower than the real one.
constexpr std::int64_t slot_high(int index) {
    if (index < kSubBuckets) {
        return index;
    }
    const detail::Decoded d = detail::decode(index);
    return ((d.mantissa + 1) << (d.exponent - kSubBucketBits)) - 1;
}

}  // namespace dariyanaap::buckets
