// Every message on the wire. Framing, message ids and per-message encoding live
// here so the host and the client cannot drift apart.
#pragma once

#include <cstring>
#include <string>
#include <vector>

#include "bitstream.h"
#include "common.h"
#include "types.h"

namespace nfc {

enum class MsgType : u8 {
    Hello = 1,        // client -> host, first packet
    Welcome = 2,      // host -> client, slot + session
    Reject = 3,       //
    Ping = 4,
    Pong = 5,
    Ownership = 6,    // OwnMsg (host -> all, client -> host ack)
    PlayerState = 7,  // owner -> host (unreliable)
    Snapshot = 8,     // host -> client, chunked entities
    EntityRemove = 9,
    TrafficSpawn = 10,
    TrafficDespawn = 11,
    RaceCmd = 12,     // host -> all, full RaceState
    RaceReady = 13,   // client -> host
    RaceProgress = 14,
    ProgressEvent = 15,   // client -> host (request) / host -> all (applied)
    ProgressSnapshot = 16,
    Digest = 17,
    RepairRequest = 18,
    RepairData = 19,
    Zone = 20,
    PlayerList = 21,
    Chat = 22,
    HostInfo = 23,
    Profile = 24,     // client -> host: the owner's car model/paint
};

enum class Channel : u8 { Control = 0, State = 1 };

const char* toString(MsgType t);

#pragma pack(push, 1)
struct PacketHeader {
    u32 magic;
    u16 version;
    u8 type;
    u8 flags;   // reserved, keep 0
    u16 len;    // payload bytes
    u16 crc;    // crc32(payload) low 16 bits
};
#pragma pack(pop)

constexpr std::size_t kHeaderSize = sizeof(PacketHeader);  // 12

// Wraps a payload in the packet header. Returns false if it would not fit.
bool encodePacket(MsgType type, const u8* payload, std::size_t len, std::vector<u8>& out);
// Validates magic/version/length/crc. On success points at the payload inside `data`.
bool decodePacket(const u8* data, std::size_t len, MsgType& type, const u8*& payload, std::size_t& payloadLen);

// --- shared message writers -------------------------------------------------
struct HelloMsg {
    u32 protocol = kProtocolVersion;
    u32 nonce = 0;
    std::string name;
    u32 flags = 0;
    u32 model = 0;  // car model id (a stable .data table offset on this build)
    u32 paint = 0;  // 0x00RRGGBB, 0 = unknown
    void encode(BitWriter& w) const {
        w.u32v(protocol);
        w.u32v(nonce);
        w.str(name);
        w.u32v(flags);
        w.u32v(model);
        w.u32v(paint);
    }
    bool decode(BitReader& r) {
        protocol = r.u32v();
        nonce = r.u32v();
        name = r.str();
        flags = r.u32v();
        model = r.u32v();
        paint = r.u32v();
        return r.ok() && name.size() <= 64;
    }
};

struct WelcomeMsg {
    u8 slot = kNoPlayer;
    u32 sessionId = 0;
    u32 seed = 0;
    u32 hostMs = 0;
    u16 snapshotHz = static_cast<u16>(kSnapshotHz);
    u32 features = 0;
    std::string hostName;
    std::string sessionName;
    EntityId yourCar = kNoEntity;
    void encode(BitWriter& w) const {
        w.u8v(slot);
        w.u32v(sessionId);
        w.u32v(seed);
        w.u32v(hostMs);
        w.u16v(snapshotHz);
        w.u32v(features);
        w.str(hostName);
        w.str(sessionName);
        w.u32v(yourCar);
    }
    bool decode(BitReader& r) {
        slot = r.u8v();
        sessionId = r.u32v();
        seed = r.u32v();
        hostMs = r.u32v();
        snapshotHz = r.u16v();
        features = r.u32v();
        hostName = r.str();
        sessionName = r.str();
        yourCar = r.u32v();
        return r.ok() && hostName.size() <= 64 && sessionName.size() <= 64;
    }
};

struct OwnMsgWire {
    u8 kind = 0;
    EntityId id = 0;
    u32 epoch = 0;
    u8 owner = kNoPlayer;
    u8 prev = kNoPlayer;
    void encode(BitWriter& w) const {
        w.u8v(kind);
        w.u32v(id);
        w.u32v(epoch);
        w.u8v(owner);
        w.u8v(prev);
    }
    bool decode(BitReader& r) {
        kind = r.u8v();
        id = r.u32v();
        epoch = r.u32v();
        owner = r.u8v();
        prev = r.u8v();
        return r.ok();
    }
};

struct EntityView {
    EntityId id = kNoEntity;
    EKind kind = EKind::Traffic;
    u8 model = 0;
    u8 owner = kNoPlayer;
    u32 epoch = 0;
    CarState st;
    void encode(BitWriter& w) const {
        w.u32v(id);
        w.u8v(static_cast<u8>(kind));
        w.u8v(model);
        w.u8v(owner);
        w.u32v(epoch);
        st.encode(w);
    }
    bool decode(BitReader& r) {
        id = r.u32v();
        kind = static_cast<EKind>(r.u8v());
        model = r.u8v();
        owner = r.u8v();
        epoch = r.u32v();
        st.decode(r);
        if (!r.ok()) return false;
        if (static_cast<u8>(kind) > static_cast<u8>(EKind::Bot)) return false;
        return true;
    }
};

struct DigestMsg {
    u32 hostMs = 0;
    u32 tick = 0;
    u32 progressRevision = 0;
    u32 raceVersion = 0;
    u64 entities = 0;
    u64 race = 0;
    u64 progress = 0;
    void encode(BitWriter& w) const {
        w.u32v(hostMs);
        w.u32v(tick);
        w.u32v(progressRevision);
        w.u32v(raceVersion);
        w.u64v(entities);
        w.u64v(race);
        w.u64v(progress);
    }
    bool decode(BitReader& r) {
        hostMs = r.u32v();
        tick = r.u32v();
        progressRevision = r.u32v();
        raceVersion = r.u32v();
        entities = r.u64v();
        race = r.u64v();
        progress = r.u64v();
        return r.ok();
    }
};

}  // namespace nfc
