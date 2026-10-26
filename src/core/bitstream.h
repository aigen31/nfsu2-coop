// Bounds-checked little-endian bit/byte stream, used for every wire message.
// A reader that runs past the end never reads out of bounds: it sets bad() and
// returns zeros, so a truncated or hostile packet cannot crash the game.
#pragma once

#include <cstring>
#include <string>
#include <vector>
#include "common.h"

namespace nfc {

class BitWriter {
public:
    explicit BitWriter(std::size_t reserve = 256) { buf_.reserve(reserve); }

    void u8v(u8 v) { buf_.push_back(v); }
    void i8v(i8 v) { buf_.push_back(static_cast<u8>(v)); }
    void u16v(u16 v) {
        buf_.push_back(static_cast<u8>(v & 0xFF));
        buf_.push_back(static_cast<u8>((v >> 8) & 0xFF));
    }
    void i16v(i16 v) { u16v(static_cast<u16>(v)); }
    void u32v(u32 v) {
        for (int i = 0; i < 4; ++i) buf_.push_back(static_cast<u8>((v >> (8 * i)) & 0xFF));
    }
    void i32v(i32 v) { u32v(static_cast<u32>(v)); }
    void u64v(u64 v) {
        for (int i = 0; i < 8; ++i) buf_.push_back(static_cast<u8>((v >> (8 * i)) & 0xFF));
    }
    void f32v(f32 v) {
        u32 bits;
        std::memcpy(&bits, &v, sizeof(bits));
        u32v(bits);
    }
    void bytes(const void* p, std::size_t n) {
        const u8* b = static_cast<const u8*>(p);
        buf_.insert(buf_.end(), b, b + n);
    }
    void str(const std::string& s) {
        u16v(static_cast<u16>(std::min<std::size_t>(s.size(), 0xFFFF)));
        bytes(s.data(), std::min<std::size_t>(s.size(), 0xFFFF));
    }
    // Quantized helpers. Saturation keeps a wild value from wrapping into a lie.
    void qPos(f32 meters) { i32v(clampi(static_cast<i32>(std::lround(meters * 64.0f)), -2000000000, 2000000000)); }
    void qVel(f32 mps) { i16v(static_cast<i16>(clampi(static_cast<i32>(std::lround(mps * 256.0f)), -32767, 32767))); }
    void qAngle16(f32 rad) {
        f32 a = wrapPi(rad);
        u16v(static_cast<u16>(std::lround((a / 6.28318530717958647692f) * 65535.0f) & 0xFFFF));
    }
    void qAngleS(f32 rad) { i16v(static_cast<i16>(clampi(static_cast<i32>(std::lround(rad * 10000.0f)), -30000, 30000))); }

    std::size_t size() const { return buf_.size(); }
    const u8* data() const { return buf_.data(); }
    std::vector<u8>& vec() { return buf_; }
    const std::vector<u8>& vec() const { return buf_; }
    void clear() { buf_.clear(); }

private:
    std::vector<u8> buf_;
};

class BitReader {
public:
    BitReader(const u8* data, std::size_t size) : data_(data), size_(size) {}
    explicit BitReader(const std::vector<u8>& v) : data_(v.data()), size_(v.size()) {}
    explicit BitReader(const BitWriter& w) : data_(w.data()), size_(w.size()) {}

    bool ok() const { return !bad_; }
    bool bad() const { return bad_; }
    std::size_t left() const { return size_ - pos_; }
    std::size_t pos() const { return pos_; }

    u8 u8v() {
        if (!need(1)) return 0;
        return data_[pos_++];
    }
    i8 i8v() { return static_cast<i8>(u8v()); }
    u16 u16v() {
        if (!need(2)) return 0;
        u16 v = static_cast<u16>(data_[pos_]) | static_cast<u16>(data_[pos_ + 1]) << 8;
        pos_ += 2;
        return v;
    }
    i16 i16v() { return static_cast<i16>(u16v()); }
    u32 u32v() {
        if (!need(4)) return 0;
        u32 v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<u32>(data_[pos_ + i]) << (8 * i);
        pos_ += 4;
        return v;
    }
    i32 i32v() { return static_cast<i32>(u32v()); }
    u64 u64v() {
        if (!need(8)) return 0;
        u64 v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<u64>(data_[pos_ + i]) << (8 * i);
        pos_ += 8;
        return v;
    }
    f32 f32v() {
        u32 bits = u32v();
        f32 v;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }
    bool bytes(void* out, std::size_t n) {
        if (!need(n)) return false;
        std::memcpy(out, data_ + pos_, n);
        pos_ += n;
        return true;
    }
    std::string str() {
        u16 n = u16v();
        if (!need(n)) return {};
        std::string s(reinterpret_cast<const char*>(data_ + pos_), n);
        pos_ += n;
        return s;
    }
    f32 qPos() { return static_cast<f32>(i32v()) / 64.0f; }
    f32 qVel() { return static_cast<f32>(i16v()) / 256.0f; }
    f32 qAngle16() { return static_cast<f32>(u16v()) / 65535.0f * 6.28318530717958647692f; }
    f32 qAngleS() { return static_cast<f32>(i16v()) / 10000.0f; }

private:
    bool need(std::size_t n) {
        if (bad_) return false;
        if (n > size_ - pos_) {
            bad_ = true;
            return false;
        }
        return true;
    }
    const u8* data_;
    std::size_t size_;
    std::size_t pos_ = 0;
    bool bad_ = false;
};

}  // namespace nfc
