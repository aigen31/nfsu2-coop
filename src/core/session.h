// Session layer: one host, up to 7 guests. The host owns the truth (race
// director, career ledger, traffic/bot mirror); a guest owns its own car.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "common.h"
#include "link.h"
#include "protocol.h"
#include "types.h"

namespace nfc {

struct SessionConfig {
    u16 port = kDefaultPort;
    std::string serverHost = "127.0.0.1";
    u16 serverPort = kDefaultPort;
    std::string playerName = "player";
    std::string sessionName = "career";
    u32 snapshotHz = kSnapshotHz;
    u32 interpDelayMs = 100;
    u32 countdownMs = kCountdownMs;
    u32 lobbySettleMs = 750;
    u32 maxTraffic = 20;
    u32 maxBots = 6;
    u32 cullDistanceM = 350;
    bool hostMode = false;
    bool publishGameEntities = true;
    bool hideLocalTraffic = true;
};

struct GameEntityView {
    EKind kind = EKind::Traffic;
    u8 model = 0;
    u32 handle = 0;
    CarState st;
};

struct SessionStats {
    u64 sent = 0;
    u64 received = 0;
    u64 sentBytes = 0;
    u64 receivedBytes = 0;
    u64 staleDropped = 0;
    u64 badPackets = 0;
    u64 digestChecks = 0;
    u64 digestMismatches = 0;
    u64 repairs = 0;
    u64 demotions = 0;
    u64 raceStarts = 0;
};

// What a session wants from the game. The plugin implements this over game
// memory; the tests implement it over a fake world. Nothing here touches the
// engine directly.
class IGameWorld {
public:
    virtual ~IGameWorld() = default;

    // False while the game is in a menu or the car pointer is not resolved.
    virtual bool readLocalCar(CarState& out) = 0;
    virtual u32 zoneId() const = 0;
    virtual u32 carModel() const { return 0; }
    virtual u32 carPaint() const { return 0; }
    // Cars this machine's engine simulates itself: traffic and AI racers.
    virtual void readGameEntities(std::vector<GameEntityView>& out) = 0;

    // Remote cars to show. A spawn may arrive for an id already known.
    virtual void onSpawn(EntityId id, EKind kind, u8 model, const CarState& st) = 0;
    virtual void onUpdate(EntityId id, EKind kind, const CarState& st) = 0;
    virtual void onDespawn(EntityId id, EKind kind) = 0;

    // Race director state reached the game (countdown, GO, results).
    virtual void onRace(const RaceState& race, RacePhase previous) = 0;
    // The shared career ledger changed (host applied an event).
    virtual void onCareer(const ProgressLedger& ledger) = 0;
    virtual void onNotice(const std::string& text) = 0;
    virtual void onConnection(bool connected, const std::string& reason) { (void)connected; (void)reason; }
};

class HostSession {
public:
    HostSession(IGameWorld& game, SessionConfig cfg);
    ~HostSession();

    bool start();
    void stop();
    bool running() const { return running_; }

    void tick(u32 nowMs);

    World& world() { return w_; }
    const World& world() const { return w_; }
    const SessionConfig& config() const { return cfg_; }
    SessionStats& stats() { return stats_; }

    bool openRace(u16 eventId, u32 routeId, u16 checkpointCount);
    bool forceStart();
    bool abortRace();
    bool applyCareer(u32 key);
    // The host's own car crossed a checkpoint / the finish line.
    bool reportLocalProgress(u16 checkpoint, u16 lap, u32 timeMs);
    bool reportLocalFinished(u32 timeMs);
    void chat(const std::string& text);
    void kick(PlayerId slot);
    void resync(PlayerId slot);

    std::string statusText() const;
    u32 nowMs() const { return nowMs_; }

private:
    struct Peer {
        u32 linkId = 0;
        PlayerId slot = kNoPlayer;
        u32 lastHeardMs = 0;
        u32 lastSeq = 0;
        bool welcomed = false;
        EntityId carId = kNoEntity;
        bool seen[8] = {false, false, false, false, false, false, false, false};
    };

    void handleEvent(const NetEvent& ev);
    void onHello(const NetEvent& ev);
    void onPlayerState(const Peer& p, BitReader& r);
    void onRaceReady(const Peer& p, BitReader& r);
    void onRaceProgress(const Peer& p, BitReader& r);
    void onProgressEvent(const Peer& p, BitReader& r);
    void onChat(const Peer& p, BitReader& r);
    void onZone(const Peer& p, BitReader& r);
    void onProfile(const Peer& p, BitReader& r);
    void onRepairRequest(const Peer& p, BitReader& r);
    void onDigest(const Peer& p, BitReader& r);

    void sendWelcome(Peer& p);
    void sendRace(PlayerId slot);
    void broadcastRace();
    void sendProgress(PlayerId slot);
    void broadcastProgress();
    void broadcastPlayerList();
    void sendSnapshot(Peer& p, u32 nowMs);
    void broadcastDigest();
    void broadcastNotice(const std::string& text);

    void updateLocalCar(u32 nowMs);
    void mirrorGameEntities(u32 nowMs);
    void dropStaleEntities(u32 nowMs);
    void driveRace(u32 nowMs);

    EntityId allocId() { return nextEntityId_++; }
    Peer* peerForSlot(PlayerId slot);
    PlayerId freeSlot() const;

    IGameWorld& game_;
    SessionConfig cfg_;
    Link link_;
    World w_;
    SessionStats stats_;
    bool running_ = false;
    u32 nowMs_ = 0;
    u32 nextEntityId_ = 1;
    EntityId localCarId_ = kNoEntity;
    u32 lastSnapshotMs_ = 0;
    u32 lastDigestMs_ = 0;
    u32 lastPingMs_ = 0;
    u32 lastEntityScanMs_ = 0;
    u32 outSeq_ = 0;
    bool resultsApplied_ = false;
    std::map<u32, Peer> peers_;                 // link id -> peer
    std::map<u32, EntityId> handleToEntity_;    // game handle -> net id
    std::map<EntityId, u32> entityToHandle_;
    std::map<EntityId, u32> lastSeenMs_;
};

class ClientSession {
public:
    ClientSession(IGameWorld& game, SessionConfig cfg);
    ~ClientSession();

    bool start();
    void stop();
    bool running() const { return running_; }
    bool connected() const { return connected_; }

    void tick(u32 nowMs);

    World& world() { return w_; }
    const World& world() const { return w_; }
    const SessionConfig& config() const { return cfg_; }
    SessionStats& stats() { return stats_; }

    bool setReady(bool ready);
    void reportProgress(u16 checkpoint, u16 lap, u32 timeMs);
    void reportFinished(u32 timeMs);
    void reportCareerEvent(u32 key);
    void chat(const std::string& text);
    void requestResync();

    u32 localSlot() const { return w_.localSlot; }
    u32 hostNowMs(u32 nowMs) const { return nowMs + offsetMs_; }
    u32 offsetMs() const { return offsetMs_; }
    u32 rttMs() const { return rttMs_; }
    std::string statusText() const;

private:
    void handleEvent(const NetEvent& ev);
    void applyWelcome(BitReader& r);
    void applySnapshot(BitReader& r);
    void applyRace(BitReader& r);
    void applyProgress(BitReader& r);
    void applyPlayerList(BitReader& r);
    void applyRepair(BitReader& r);
    void applyDigest(BitReader& r);
    void publishLocalCar(u32 nowMs);
    void maybeFinishCountdown(u32 nowMs);
    void checkTimeouts(u32 nowMs);
    void sendPing(u32 nowMs);

    IGameWorld& game_;
    SessionConfig cfg_;
    Link link_;
    World w_;
    SessionStats stats_;
    bool running_ = false;
    bool connecting_ = false;
    bool connected_ = false;
    bool welcomed_ = false;
    u32 nowMs_ = 0;
    u32 connectStartedMs_ = 0;
    u32 lastConnectAttemptMs_ = 0;
    u32 lastPacketMs_ = 0;
    u32 lastStateMs_ = 0;
    u32 lastPingMs_ = 0;
    u32 lastDigestMs_ = 0;
    u32 sentProfileModel_ = 0;
    u32 seq_ = 0;
    u32 myCarId_ = kNoEntity;
    u32 offsetMs_ = 0;
    u32 rttMs_ = 0;
    u32 pingNonce_ = 0;
    u32 pingSentMs_ = 0;
    u32 badDigests_ = 0;
    bool raceNotified_ = false;
    RacePhase lastPhase_ = RacePhase::Idle;
    std::map<EntityId, u32> lastSeenMs_;
};

}  // namespace nfc
