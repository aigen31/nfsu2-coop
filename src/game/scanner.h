// PE reader + IDA-style pattern scanner. Used by coop-probe (offline analysis of
// speed2.exe) and by the plugin (find anchors in the loaded speed2.exe).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/common.h"

namespace nfc {

struct PeSection {
    std::string name;
    u32 va = 0;
    u32 vsize = 0;
    u32 raw = 0;
    u32 rawSize = 0;
    u32 characteristics = 0;
    bool executable() const { return (characteristics & 0x20000000u) != 0; }
};

class PeImage {
public:
    bool loadFile(const std::string& path);
    bool loadModule(void* moduleBase);
    bool loaded() const { return loaded_; }

    u32 imageBase() const { return imageBase_; }
    u32 sizeOfImage() const { return sizeOfImage_; }
    u32 entryPoint() const { return entryPoint_; }
    const std::vector<PeSection>& sections() const { return sections_; }
    const std::vector<u8>& fileData() const { return data_; }

    const u8* rvaPtr(u32 rva) const;
    bool readVa(u32 va, void* out, u32 n) const;
    bool vaInImage(u32 va) const;
    u32 vaToRva(u32 va) const { return va - imageBase_; }
    const PeSection* sectionForRva(u32 rva) const;
    std::string filePath() const { return path_; }

private:
    bool parse(const u8* base, std::size_t size);
    std::vector<u8> data_;
    bool loaded_ = false;
    bool mapped_ = false;
    const u8* base_ = nullptr;
    std::string path_;
    u32 imageBase_ = 0;
    u32 sizeOfImage_ = 0;
    u32 entryPoint_ = 0;
    std::vector<PeSection> sections_;
};

struct Anchor {
    const char* name;
    u32 va;
    const char* note;
};

// Function addresses documented by the Crashing Foreign Cars mod for the
// PC 1.2 build (speed2.exe). The probe proves they resolve in a given exe.
std::vector<Anchor> knownAnchors();

struct AnchorCheck {
    std::string name;
    u32 va = 0;
    bool inText = false;
    bool functionStart = false;
    u8 bytes[16] = {};
    std::string note;
};

std::vector<AnchorCheck> checkAnchors(const PeImage& img);

// "55 8B EC ?? 83 EC ??" — ?? matches one byte, ? matches one nibble.
bool scanPattern(const PeImage& img, const std::string& idaPattern, u32& outVa);

std::string hexBytes(const u8* data, std::size_t n);
std::string formatVa(u32 va);

// SHA-256 of a file, for recording the exact build in notes and reports.
std::string sha256File(const std::string& path);

}  // namespace nfc
