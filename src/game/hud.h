// Data the plugin hands to the on-screen HUD. Plain strings, copied per frame.
#pragma once

#include <string>
#include <vector>

namespace nfc {

struct HudState {
    bool visible = true;
    bool connected = false;
    bool host = false;
    bool bound = false;  // game memory bindings resolved (car/traffic readable)
    std::string title = "NFSU2 Co-op";
    std::string status;
    std::string raceLine;
    std::string careerLine;
    std::vector<std::string> players;
    std::vector<std::string> standings;
    std::vector<std::string> remotes;
    std::vector<std::string> warnings;
    std::vector<std::string> notices;
    int puppets = 0;
    int traffic = 0;
};

}  // namespace nfc

