// NFSU2 Co-op — common types and small helpers.
// Portable C++17. No Windows headers here: the core also builds and runs in tests.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace nfc {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using f32 = float;
using f64 = double;

// --- clock -----------------------------------------------------------------
u64 nowMs();
void sleepMs(u32 ms);

// --- session shape ---------------------------------------------------------
constexpr int kMaxPlayers = 8;
using PlayerId = u8;
constexpr PlayerId kNoPlayer = 0xFF;

using EntityId = u32;
constexpr EntityId kNoEntity = 0;

constexpr u32 kProtocolVersion = 0x00040001;  // 0.4.0, bump on any wire change
constexpr u32 kMagic = 0x3243464Eu;           // 'NFC2' in little endian
constexpr u32 kMaxPayload = 1200;             // stay below a 1280 byte MTU
constexpr u32 kSnapshotEntitiesPerPacket = 18;

constexpr u32 kDefaultPort = 27970;
constexpr u32 kTickHz = 60;
constexpr u32 kSnapshotHz = 20;
constexpr u32 kHeartbeatMs = 1000;
constexpr u32 kDigestMs = 2000;
constexpr u32 kCountdownMs = 5000;
constexpr u32 kJoinTimeoutMs = 15000;
constexpr u32 kDisconnectTimeoutMs = 10000;

// --- small math ------------------------------------------------------------
inline f32 clampf(f32 v, f32 lo, f32 hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline i32 clampi(i32 v, i32 lo, i32 hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline u32 clampu(u32 v, u32 lo, u32 hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline f32 wrapPi(f32 a) {
    constexpr f32 kPi = 3.14159265358979323846f;
    constexpr f32 kTwoPi = 6.28318530717958647692f;
    while (a > kPi) a -= kTwoPi;
    while (a < -kPi) a += kTwoPi;
    return a;
}

struct Vec3 {
    f32 x = 0, y = 0, z = 0;
};

inline Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(const Vec3& a, f32 s) { return {a.x * s, a.y * s, a.z * s}; }
inline f32 dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline f32 length2(const Vec3& a) { return dot(a, a); }
inline f32 length(const Vec3& a) { return std::sqrt(length2(a)); }
inline Vec3 lerp(const Vec3& a, const Vec3& b, f32 t) { return a + (b - a) * t; }

// --- hashing / checksums ---------------------------------------------------
inline u32 crc32(const void* data, std::size_t n, u32 seed = 0) {
    static u32 table[256];
    static bool built = false;
    if (!built) {
        for (u32 i = 0; i < 256; ++i) {
            u32 c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        built = true;
    }
    u32 c = seed ^ 0xFFFFFFFFu;
    const u8* p = static_cast<const u8*>(data);
    for (std::size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

inline u64 fnv1a64(const void* data, std::size_t n, u64 seed = 0xCBF29CE484222325ull) {
    const u8* p = static_cast<const u8*>(data);
    u64 h = seed;
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

// --- version ---------------------------------------------------------------
inline const char* versionString() { return "0.4.0"; }

}  // namespace nfc
