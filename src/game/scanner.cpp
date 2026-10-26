#include "scanner.h"

#include <cstdio>
#include <cstring>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace nfc {
namespace {

u16 rd16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }
u32 rd32(const u8* p) { return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24); }

// --- SHA-256, small and dependency free ------------------------------------
struct Sha256 {
    u32 h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    u64 total = 0;
    u8 buf[64] = {};
    std::size_t fill = 0;

    static u32 rotr(u32 x, int n) { return (x >> n) | (x << (32 - n)); }

    void block(const u8* p) {
        static const u32 k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        u32 w[64];
        for (int i = 0; i < 16; ++i) w[i] = (static_cast<u32>(p[i * 4]) << 24) | (static_cast<u32>(p[i * 4 + 1]) << 16) |
                                          (static_cast<u32>(p[i * 4 + 2]) << 8) | p[i * 4 + 3];
        for (int i = 16; i < 64; ++i) {
            const u32 s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const u32 s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const u32 s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const u32 ch = (e & f) ^ (~e & g);
            const u32 t1 = hh + s1 + ch + k[i] + w[i];
            const u32 s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const u32 maj = (a & b) ^ (a & c) ^ (b & c);
            const u32 t2 = s0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    void update(const u8* p, std::size_t n) {
        total += n;
        while (n) {
            const std::size_t take = std::min<std::size_t>(64 - fill, n);
            std::memcpy(buf + fill, p, take);
            fill += take;
            p += take;
            n -= take;
            if (fill == 64) {
                block(buf);
                fill = 0;
            }
        }
    }

    std::string finish() {
        const u64 bits = total * 8;
        const u8 pad = 0x80;
        update(&pad, 1);
        const u8 zero = 0;
        while (fill != 56) update(&zero, 1);
        u8 len[8];
        for (int i = 0; i < 8; ++i) len[7 - i] = static_cast<u8>((bits >> (8 * i)) & 0xFF);
        update(len, 8);
        char out[65];
        for (int i = 0; i < 8; ++i)
            std::snprintf(out + i * 8, 9, "%08x", h[i]);
        return std::string(out, 64);
    }
};

bool isFunctionStart(const u8* b) {
    // push ebp; mov ebp,esp | push reg(s) | mov edi,edi | mov eax, [abs] (thiscall boot)
    if (b[0] == 0x55 && b[1] == 0x8B && b[2] == 0xEC) return true;
    if (b[0] == 0x8B && b[1] == 0xFF) return true;
    if (b[0] >= 0x50 && b[0] <= 0x57) return true;
    if (b[0] == 0x81 && b[1] == 0xEC) return true;
    if (b[0] == 0x83 && b[1] == 0xEC) return true;
    return false;
}

}  // namespace

bool PeImage::loadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    data_.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    if (data_.size() < 0x200) return false;
    path_ = path;
    mapped_ = false;
    base_ = data_.data();
    return parse(data_.data(), data_.size());
}

bool PeImage::loadModule(void* moduleBase) {
#ifdef _WIN32
    const u8* base = static_cast<const u8*>(moduleBase);
    if (!base) return false;
    data_.clear();
    mapped_ = true;
    base_ = base;
    path_.clear();
    return parse(base, 0);
#else
    (void)moduleBase;
    return false;
#endif
}

bool PeImage::parse(const u8* base, std::size_t size) {
    if (base[0] != 'M' || base[1] != 'Z') return false;
    const u32 peOff = rd32(base + 0x3C);
    if (size && peOff + 0x18 > size) return false;
    if (std::memcmp(base + peOff, "PE\0\0", 4) != 0) return false;
    const u16 machine = rd16(base + peOff + 4);
    const u16 numSections = rd16(base + peOff + 6);
    const u16 optSize = rd16(base + peOff + 20);
    const u8* opt = base + peOff + 24;
    const u16 magic = rd16(opt);
    if (magic != 0x10B) return false;  // 32-bit only
    imageBase_ = rd32(opt + 28);
    entryPoint_ = rd32(opt + 16);
    sizeOfImage_ = rd32(opt + 56);
    const u16 optHeaderSize = optSize;
    const u8* sec = opt + optHeaderSize;
    if (size && static_cast<std::size_t>(sec - base) + numSections * 40 > size) return false;
    sections_.clear();
    for (u16 i = 0; i < numSections; ++i) {
        const u8* s = sec + i * 40;
        PeSection ps;
        char name[9] = {};
        std::memcpy(name, s, 8);
        ps.name = name;
        ps.vsize = rd32(s + 8);
        ps.va = rd32(s + 12);
        ps.rawSize = rd32(s + 16);
        ps.raw = rd32(s + 20);
        ps.characteristics = rd32(s + 36);
        sections_.push_back(ps);
    }
    loaded_ = true;
    (void)machine;
    return true;
}

const PeSection* PeImage::sectionForRva(u32 rva) const {
    for (const PeSection& s : sections_) {
        if (rva >= s.va && rva < s.va + std::max(s.vsize, s.rawSize)) return &s;
    }
    return nullptr;
}

const u8* PeImage::rvaPtr(u32 rva) const {
    const PeSection* s = sectionForRva(rva);
    if (!s) return nullptr;
    if (mapped_) return base_ + rva;
    const u32 off = s->raw + (rva - s->va);
    if (off >= data_.size()) return nullptr;
    return data_.data() + off;
}

bool PeImage::readVa(u32 va, void* out, u32 n) const {
    if (!loaded_ || va < imageBase_) return false;
    const u8* p = rvaPtr(va - imageBase_);
    if (!p) return false;
    std::memcpy(out, p, n);
    return true;
}

bool PeImage::vaInImage(u32 va) const {
    if (!loaded_ || va < imageBase_) return false;
    return sectionForRva(va - imageBase_) != nullptr;
}

std::vector<Anchor> knownAnchors() {
    return {
        {"CarRenderInfo::Render", 0x620AE0, "draws a car; conflicts with other mods that hook it"},
        {"CarRenderInfo::RenderNeon", 0x60D7F0, "neon pass"},
        {"CarDamage::SetDamageToZero", 0x637D70, "repairs damage; useful to resync damage state"},
        {"Player::ClearZone", 0x5FFE70, "zone transition; fires on city/race load"},
        {"MenuScreen::BaseNotify", 0x55B5C0, "menu events; career event completion shows here"},
        {"MenuScreen::SetButtons", 0x54E6E0, "help bar"},
        {"MenuScreen::ShowTwoButtons", 0x558790, "dialogs"},
        {"BuyCarPart", 0x5289F0, "shop purchase; career sync point"},
        {"InstallPart", 0x55C230, "part install; career sync point"},
        {"CarSelect::RefreshHeader", 0x4B0140, "garage car select"},
    };
}

std::vector<AnchorCheck> checkAnchors(const PeImage& img) {
    std::vector<AnchorCheck> out;
    for (const Anchor& a : knownAnchors()) {
        AnchorCheck c;
        c.name = a.name;
        c.va = a.va;
        c.note = a.note;
        c.inText = img.vaInImage(a.va);
        if (c.inText && img.readVa(a.va, c.bytes, sizeof(c.bytes))) c.functionStart = isFunctionStart(c.bytes);
        out.push_back(c);
    }
    return out;
}

bool scanPattern(const PeImage& img, const std::string& idaPattern, u32& outVa) {
    // Parse "55 8B EC ?? 83 EC" into byte/mask pairs.
    std::vector<u8> bytes;
    std::vector<u8> mask;
    for (std::size_t i = 0; i < idaPattern.size();) {
        while (i < idaPattern.size() && idaPattern[i] == ' ') ++i;
        if (i >= idaPattern.size()) break;
        if (idaPattern[i] == '?') {
            if (i + 1 < idaPattern.size() && idaPattern[i + 1] == '?') {
                bytes.push_back(0);
                mask.push_back(0);
                i += 2;
            } else {
                // nibble wildcard: high nibble fixed, low wild
                const char hi = i + 1 < idaPattern.size() ? idaPattern[i + 1] : '?';
                { const std::string tmp = std::string(1, hi) + "0"; bytes.push_back(static_cast<u8>(std::strtoul(tmp.c_str(), nullptr, 16))); }
                mask.push_back(0xF0);
                i += 2;
            }
        } else {
            if (i + 2 > idaPattern.size()) break;
            bytes.push_back(static_cast<u8>(std::strtoul(idaPattern.substr(i, 2).c_str(), nullptr, 16)));
            mask.push_back(0xFF);
            i += 2;
        }
    }
    if (bytes.empty()) return false;

    for (const PeSection& s : img.sections()) {
        if (!s.executable()) continue;
        const u8* base = img.rvaPtr(s.va);
        if (!base) continue;
        const std::size_t n = s.vsize ? s.vsize : s.rawSize;
        for (std::size_t i = 0; i + bytes.size() <= n; ++i) {
            bool hit = true;
            for (std::size_t j = 0; j < bytes.size(); ++j) {
                if ((base[i + j] & mask[j]) != (bytes[j] & mask[j])) {
                    hit = false;
                    break;
                }
            }
            if (hit) {
                outVa = img.imageBase() + s.va + static_cast<u32>(i);
                return true;
            }
        }
    }
    return false;
}

std::string hexBytes(const u8* data, std::size_t n) {
    static const char* hex = "0123456789ABCDEF";
    std::string s;
    for (std::size_t i = 0; i < n; ++i) {
        if (i) s.push_back(' ');
        s.push_back(hex[data[i] >> 4]);
        s.push_back(hex[data[i] & 0xF]);
    }
    return s;
}

std::string formatVa(u32 va) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%06X", va);
    return buf;
}

std::string sha256File(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    Sha256 ctx;
    std::vector<char> buf(64 * 1024);
    while (f) {
        f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize n = f.gcount();
        if (n <= 0) break;
        ctx.update(reinterpret_cast<const u8*>(buf.data()), static_cast<std::size_t>(n));
    }
    return ctx.finish();
}

}  // namespace nfc

