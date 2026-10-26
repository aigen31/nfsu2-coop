#include "config.h"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace nfc {
namespace {

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

bool IniFile::load(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line, section;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = lower(trim(line.substr(1, line.size() - 2)));
            continue;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = lower(trim(line.substr(0, eq)));
        std::string value = trim(line.substr(eq + 1));
        if (!value.empty() && value.front() == '"' && value.back() == '"' && value.size() >= 2)
            value = value.substr(1, value.size() - 2);
        data_[section][key] = value;
    }
    return true;
}

std::string IniFile::get(const std::string& section, const std::string& key, const std::string& def) const {
    auto s = data_.find(lower(section));
    if (s == data_.end()) return def;
    auto k = s->second.find(lower(key));
    return k == s->second.end() ? def : k->second;
}

int IniFile::getInt(const std::string& section, const std::string& key, int def) const {
    const std::string v = get(section, key, "");
    if (v.empty()) return def;
    return static_cast<int>(std::strtol(v.c_str(), nullptr, 0));
}

bool IniFile::getBool(const std::string& section, const std::string& key, bool def) const {
    const std::string v = lower(get(section, key, ""));
    if (v.empty()) return def;
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

bool IniFile::has(const std::string& section, const std::string& key) const {
    auto s = data_.find(lower(section));
    if (s == data_.end()) return false;
    return s->second.count(lower(key)) != 0;
}

std::string defaultConfigText() {
    return
        "; NFSU2 Co-op configuration. Keys are case-insensitive.\n"
        "; Edit, save, restart the game. Log: NFSU2Coop.log next to the .asi\n"
        "\n[Coop]\n"
        "; Host=1 starts a session on this PC; Host=0 joins Server.\n"
        "Host=0\n"
        "Server=127.0.0.1\n"
        "Port=27970\n"
        "Name=Player\n"
        "Session=career\n"
        "; Event id used by F6 (host: open) and F7 (guest: report done).\n"
        "EventId=1\n"
        "\n[Game]\n"
        "PublishTraffic=1\n"
        "SnapshotHz=20\n"
        "CountdownMs=5000\n"
        "MaxTraffic=20\n"
        "MaxBots=6\n"
        "\n[Overlay]\n"
        "Enabled=1\n"
        "VerboseLog=0\n"
        "\n[Bindings]\n"
        "; Offsets for YOUR speed2.exe build. All zeros = protocol-only mode:\n"
        "; networking, lobby, race director, standings and career still work;\n"
        "; car/traffic sync needs real values. See docs/GAME-BINDINGS.md.\n"
        "CarPointer=0\n"
        "PosOffset=0\n"
        "VelOffset=0\n"
        "YawOffset=0\n"
        "SpeedOffset=0\n"
        "GearOffset=0\n"
        "TrafficList=0\n"
        "TrafficCount=0\n"
        "TrafficStride=0\n"
        "TrafficPosOffset=0\n"
        "TrafficModelOffset=0\n"
        "StartEvent=0\n"
        "CareerCash=0\n"
        "CareerRep=0\n"
        "ZonePointer=0\n";
}

bool loadConfig(const std::string& path, GameConfig& out) {
    IniFile ini;
    if (!ini.load(path)) return false;
    out.host = ini.getBool("coop", "host", false);
    out.serverHost = ini.get("coop", "server", "127.0.0.1");
    out.port = static_cast<u16>(ini.getInt("coop", "port", kDefaultPort));
    out.playerName = ini.get("coop", "name", "player");
    out.sessionName = ini.get("coop", "session", "career");
    out.eventId = static_cast<u16>(ini.getInt("coop", "eventid", 1));
    out.publishTraffic = ini.getBool("game", "publishtraffic", true);
    out.snapshotHz = static_cast<u32>(ini.getInt("game", "snapshothz", kSnapshotHz));
    out.countdownMs = static_cast<u32>(ini.getInt("game", "countdownms", kCountdownMs));
    out.maxTraffic = static_cast<u32>(ini.getInt("game", "maxtraffic", 20));
    out.maxBots = static_cast<u32>(ini.getInt("game", "maxbots", 6));
    out.overlay = ini.getBool("overlay", "enabled", true);
    out.verbose = ini.getBool("overlay", "verboselog", false);
    return true;
}

bool loadBindings(const std::string& path, GameBindings& out) {
    IniFile ini;
    if (!ini.load(path)) return false;
    auto va = [&](const char* key) { return static_cast<u32>(ini.getInt("bindings", key, 0)); };
    out.carPointer = va("carpointer");
    out.posOffset = va("posoffset");
    out.velOffset = va("veloffset");
    out.yawOffset = va("yawoffset");
    out.speedOffset = va("speedoffset");
    out.gearOffset = va("gearoffset");
    out.trafficList = va("trafficlist");
    out.trafficCount = va("trafficcount");
    out.trafficStride = va("trafficstride");
    out.trafficPosOffset = va("trafficposoffset");
    out.trafficModelOffset = va("trafficmodeloffset");
    out.startEventFn = va("startevent");
    out.careerCashFn = va("careercash");
    out.careerRepFn = va("careerrept");
    out.zonePointer = va("zonepointer");
    return true;
}

}  // namespace nfc
