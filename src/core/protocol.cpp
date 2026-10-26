#include "protocol.h"

#include <cstdio>

namespace nfc {

const char* toString(MsgType t) {
    switch (t) {
        case MsgType::Hello: return "Hello";
        case MsgType::Welcome: return "Welcome";
        case MsgType::Reject: return "Reject";
        case MsgType::Ping: return "Ping";
        case MsgType::Pong: return "Pong";
        case MsgType::Ownership: return "Ownership";
        case MsgType::PlayerState: return "PlayerState";
        case MsgType::Snapshot: return "Snapshot";
        case MsgType::EntityRemove: return "EntityRemove";
        case MsgType::TrafficSpawn: return "TrafficSpawn";
        case MsgType::TrafficDespawn: return "TrafficDespawn";
        case MsgType::RaceCmd: return "RaceCmd";
        case MsgType::RaceReady: return "RaceReady";
        case MsgType::RaceProgress: return "RaceProgress";
        case MsgType::ProgressEvent: return "ProgressEvent";
        case MsgType::ProgressSnapshot: return "ProgressSnapshot";
        case MsgType::Digest: return "Digest";
        case MsgType::RepairRequest: return "RepairRequest";
        case MsgType::RepairData: return "RepairData";
        case MsgType::Zone: return "Zone";
        case MsgType::PlayerList: return "PlayerList";
        case MsgType::Chat: return "Chat";
        case MsgType::HostInfo: return "HostInfo";
    }
    return "?";
}

bool encodePacket(MsgType type, const u8* payload, std::size_t len, std::vector<u8>& out) {
    if (len > 0xFFFF || len > kMaxPayload) return false;
    PacketHeader h{};
    h.magic = kMagic;
    h.version = static_cast<u16>(kProtocolVersion);
    h.type = static_cast<u8>(type);
    h.flags = 0;
    h.len = static_cast<u16>(len);
    h.crc = static_cast<u16>(crc32(payload, len) & 0xFFFF);
    out.resize(kHeaderSize + len);
    std::memcpy(out.data(), &h, kHeaderSize);
    if (len) std::memcpy(out.data() + kHeaderSize, payload, len);
    return true;
}

bool decodePacket(const u8* data, std::size_t len, MsgType& type, const u8*& payload, std::size_t& payloadLen) {
    if (len < kHeaderSize) return false;
    PacketHeader h{};
    std::memcpy(&h, data, kHeaderSize);
    if (h.magic != kMagic) return false;
    if (h.version != static_cast<u16>(kProtocolVersion)) return false;
    if (len != kHeaderSize + h.len) return false;
    if (h.len > kMaxPayload) return false;
    payload = data + kHeaderSize;
    payloadLen = h.len;
    if (static_cast<u16>(crc32(payload, payloadLen) & 0xFFFF) != h.crc) return false;
    const u8 t = h.type;
    if (t == 0 || t > static_cast<u8>(MsgType::HostInfo)) return false;
    type = static_cast<MsgType>(t);
    return true;
}

}  // namespace nfc
