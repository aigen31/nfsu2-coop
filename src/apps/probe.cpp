// coop-probe: identify the game build and prove that the documented function
// anchors resolve in it. Run this before trusting any offset table.
//
//   coop-probe --exe "<game>\speed2.exe" --json probe-report.json
//
// Exit code 0 when every documented anchor resolves inside an executable
// section, 2 otherwise (a different build: re-run the scanner, then update
// docs/GAME-BINDINGS.md).
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "game/scanner.h"

using namespace nfc;

namespace {

struct Result {
    std::string exe;
    std::string sha256;
    u32 imageBase = 0;
    u32 entryPoint = 0;
    u32 sizeOfImage = 0;
    std::vector<AnchorCheck> anchors;
    bool patternFound = false;
    u32 patternVa = 0;
    int resolved = 0;
    int total = 0;
};

void printReport(const Result& r, const PeImage& img) {
    std::printf("NFS Underground 2 - speed2.exe\n");
    std::printf("  file         %s\n", r.exe.c_str());
    std::printf("  sha256       %s\n", r.sha256.c_str());
    std::printf("  image base   0x%08X   entry 0x%08X   image size 0x%X\n", r.imageBase, r.entryPoint,
                r.sizeOfImage);
    std::printf("  sections:\n");
    for (const PeSection& s : img.sections()) {
        std::printf("    %-8s va 0x%06X  vsize 0x%06X  raw 0x%06X  %s\n", s.name.c_str(), s.va, s.vsize, s.raw,
                    s.executable() ? "code" : "");
    }
    std::printf("  documented anchors (Crashing Foreign Cars mod, PC 1.2):\n");
    for (const AnchorCheck& c : r.anchors) {
        std::printf("    %s %-30s %s  %-47s %s\n", c.inText ? "OK  " : "MISS", c.name.c_str(),
                    formatVa(c.va).c_str(), hexBytes(c.bytes, 8).c_str(), c.functionStart ? "function start" : "");
    }
    std::printf("  pattern scan %s at %s\n", r.patternFound ? "found" : "NOT FOUND", formatVa(r.patternVa).c_str());
    std::printf("  %d/%d anchors resolved\n", r.resolved, r.total);
}

void writeJson(const Result& r, const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "{\n  \"game\": \"NFS Underground 2\",\n  \"exe\": \"%s\",\n  \"sha256\": \"%s\",\n",
                 r.exe.c_str(), r.sha256.c_str());
    std::fprintf(f, "  \"imageBase\": %u,\n  \"entryPoint\": %u,\n  \"anchors\": [\n", r.imageBase, r.entryPoint);
    for (std::size_t i = 0; i < r.anchors.size(); ++i) {
        const AnchorCheck& c = r.anchors[i];
        std::fprintf(f, "    {\"name\": \"%s\", \"va\": %u, \"resolved\": %s, \"functionStart\": %s}%s\n",
                     c.name.c_str(), c.va, c.inText ? "true" : "false", c.functionStart ? "true" : "false",
                     i + 1 == r.anchors.size() ? "" : ",");
    }
    std::fprintf(f, "  ],\n  \"patternFound\": %s,\n  \"patternVa\": %u\n}\n", r.patternFound ? "true" : "false",
                 r.patternVa);
    std::fclose(f);
}

}  // namespace

int main(int argc, char** argv) {
    std::string exe = "game\\speed2.exe";
    std::string json;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--exe") && i + 1 < argc)
            exe = argv[++i];
        else if (!std::strcmp(argv[i], "--json") && i + 1 < argc)
            json = argv[++i];
        else if (!std::strcmp(argv[i], "--help")) {
            std::printf("usage: coop-probe [--exe <speed2.exe>] [--json <report.json>]\n");
            return 0;
        }
    }

    PeImage img;
    if (!img.loadFile(exe)) {
        std::printf("cannot read a 32-bit PE at '%s'\n", exe.c_str());
        return 1;
    }

    Result r;
    r.exe = exe;
    r.sha256 = sha256File(exe);
    r.imageBase = img.imageBase();
    r.entryPoint = img.entryPoint();
    r.sizeOfImage = img.sizeOfImage();
    r.anchors = checkAnchors(img);
    for (const AnchorCheck& c : r.anchors) {
        ++r.total;
        if (c.inText) ++r.resolved;
    }
    // Signature of Player::ClearZone's first 11 bytes: proves the scanner works
    // on this build and gives a pattern to re-find the function after a patch.
    r.patternFound = scanPattern(img, "53 56 8B F1 8B 46 58 33 DB 3B C3", r.patternVa);

    printReport(r, img);
    if (!json.empty()) writeJson(r, json);
    return r.resolved == r.total ? 0 : 2;
}
