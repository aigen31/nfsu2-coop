// nfsu2coop.ini: the only thing a player edits. Parsed without a dependency.
#pragma once

#include <map>
#include <string>

#include "core/common.h"

namespace nfc {

struct GameConfig {
    bool host = false;
    std::string serverHost = "127.0.0.1";
    u16 port = kDefaultPort;
    std::string playerName = "player";
    std::string sessionName = "career";
    u16 eventId = 1;
    bool overlay = true;
    bool publishTraffic = true;
    bool drawRemoteCars = true;
    u32 snapshotHz = kSnapshotHz;
    u32 countdownMs = kCountdownMs;
    u32 maxTraffic = 20;
    u32 maxBots = 6;
    std::string logPath;
    std::string iniPath;
    bool verbose = false;
};

class IniFile {
public:
    bool load(const std::string& path);
    std::string get(const std::string& section, const std::string& key, const std::string& def = "") const;
    int getInt(const std::string& section, const std::string& key, int def) const;
    bool getBool(const std::string& section, const std::string& key, bool def) const;
    bool has(const std::string& section, const std::string& key) const;

private:
    std::map<std::string, std::map<std::string, std::string>> data_;
};

bool loadConfig(const std::string& path, GameConfig& out);
std::string defaultConfigText();

// Function/pointer addresses for one game build. Any zero value keeps the
// matching feature off; the mod stays fully usable for networking and career.
struct GameBindings {
    // Player car: absolute VA of the pointer that holds the car object.
    u32 carPointer = 0;
    u32 posOffset = 0;
    u32 velOffset = 0;
    u32 yawOffset = 0;
    u32 speedOffset = 0;
    u32 gearOffset = 0;
    // Array of Car*: [CarArray + CarIndex*CarStride] = local player's car.
    // Found on the verified build: CarArray 0x89CCF8, index 0, stride 4,
    // position at +0x50, 3x3 basis at +0x90.
    u32 carArray = 0;
    u32 carIndex = 0;
    u32 carStride = 4;
    u32 rotOffset = 0;
    // Traffic/AI cars: a flat array in the game.
    u32 trafficList = 0;
    u32 trafficCount = 0;
    u32 trafficStride = 0;
    u32 trafficPosOffset = 0;
    u32 trafficModelOffset = 0;
    // Actions, called on the game thread.
    u32 startEventFn = 0;
    u32 careerCashFn = 0;
    u32 careerRepFn = 0;
    u32 zonePointer = 0;

    bool carBound() const { return (carPointer != 0 || carArray != 0) && posOffset != 0; }
    bool trafficBound() const { return trafficList != 0 && trafficCount != 0 && trafficStride != 0; }
};

bool loadBindings(const std::string& path, GameBindings& out);

}  // namespace nfc



