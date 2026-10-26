// Deterministic RNG. Owned by us, never the platform's: two machines with the
// same seed must produce the same traffic and the same bot behaviour.
#pragma once

#include "common.h"

namespace nfc {

class Rng {
public:
    explicit Rng(u64 seed = 0x9E3779B97F4A7C15ull) { reseed(seed); }

    void reseed(u64 seed) {
        s_ = seed ? seed : 0x9E3779B97F4A7C15ull;
        for (int i = 0; i < 4; ++i) next();
    }

    u64 next() {
        s_ ^= s_ >> 12;
        s_ ^= s_ << 25;
        s_ ^= s_ >> 27;
        return s_ * 2685821657736338717ull;
    }

    u32 next32() { return static_cast<u32>(next() >> 32); }

    // [0, n)
    u32 below(u32 n) { return n ? next32() % n : 0; }

    f32 unit() { return static_cast<f32>(next32() >> 8) / 16777216.0f; }

    f32 range(f32 lo, f32 hi) { return lo + (hi - lo) * unit(); }

    i32 rangeI(i32 lo, i32 hi) { return lo + static_cast<i32>(below(static_cast<u32>(hi - lo + 1))); }

    bool chance(f32 p) { return unit() < p; }

    u64 state() const { return s_; }

private:
    u64 s_ = 0;
};

// Stable hash of a few values, used for ids and per-entity decisions.
inline u64 mix64(u64 a, u64 b = 0) {
    u64 x = a ^ (b + 0x9E3779B97F4A7C15ull + (a << 6) + (a >> 2));
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBull;
    x ^= x >> 31;
    return x;
}

}  // namespace nfc
