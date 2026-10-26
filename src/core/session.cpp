#include "session.h"

#include <algorithm>
#include <cstdio>
#include <set>

#include "log.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

namespace nfc {

u64 nowMs() {
#ifdef _WIN32
    return static_cast<u64>(GetTickCount64());
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<u64>(ts.tv_sec) * 1000ull + static_cast<u64>(ts.tv_nsec / 1000000);
#endif
}

void sleepMs(u32 ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, nullptr);
#endif
}

namespace {

bool seqNewer(u32 a, u32 b) { return static_cast<i32>(a - b) > 0; }

std::string cleanName(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (out.size() >= 24) break;
        if (c >= 32 && c < 127) out.push_back(c);
    }
    if (out.empty()) out = "player";
    return out;
}

}  // namespace

// ===========================================================================
// HostSession
// ===========================================================================
HostSession::HostSession(IGameWorld& game, SessionConfig cfg) : game_(game), cfg_(std::move(cfg)) {
    w_.reset();
}

HostSession::~HostSession() { stop(); }

bool HostSession::start() {
    if (running_) return false;
    w_.reset();
    w_.sessionId = static_cast<u32>(mix64(static_cast<u64>(nowMs()), 0x4E46535532ull));
    w_.sessionSeed = static_cast<u32>(mix64(w_.sessionId, 0x5EEDC0DEull));
    w_.localSlot = 0;
    w_.hostName = cfg_.playerName;
    w_.sessionName = cfg_.sessionName;

    localCarId_ = allocId();
    w_.players[0].connected = true;
    w_.players[0].name = cfg_.playerName;
    w_.players[0].carId = localCarId_;
    w_.players[0].loaded = true;
    Entity& e = w_.upsert(localCarId_, EKind::Player);
    e.owner = 0;
    e.active = true;

    if (!link_.startServer(cfg_.port)) return false;
    running_ = true;
    NFC_INFO("host: session %08x seed %08x on port %u, name '%s'", w_.sessionId, w_.sessionSeed, cfg_.port,
             cfg_.playerName.c_str());
    game_.onNotice("co-op host started on port " + std::to_string(cfg_.port));
    return true;
}

void HostSession::stop() {
    if (!running_) return;
    broadcastNotice("host stopped");
    for (auto& kv : peers_)
        if (kv.second.welcomed) link_.disconnect(kv.second.linkId);
    link_.flush(120);
    running_ = false;
    link_.close();
    NFC_INFO("host: stopped");
}

PlayerId HostSession::freeSlot() const {
    for (int i = 1; i < kMaxPlayers; ++i)
        if (!w_.players[i].connected) return static_cast<PlayerId>(i);
    return kNoPlayer;
}

HostSession::Peer* HostSession::peerForSlot(PlayerId slot) {
    for (auto& kv : peers_)
        if (kv.second.slot == slot) return &kv.second;
    return nullptr;
}

void HostSession::tick(u32 nowMsArg) {
    if (!running_) return;
    nowMs_ = nowMsArg;
    w_.hostMs = nowMsArg;

    std::vector<NetEvent> events;
    link_.poll(events);
    for (const NetEvent& ev : events) handleEvent(ev);

    updateLocalCar(nowMsArg);
    if (cfg_.publishGameEntities && nowMsArg - lastEntityScanMs_ >= 100) {
        lastEntityScanMs_ = nowMsArg;
        mirrorGameEntities(nowMsArg);
    }
    dropStaleEntities(nowMsArg);
    driveRace(nowMsArg);

    const u32 snapshotMs = std::max(10u, 1000u / std::max(1u, cfg_.snapshotHz));
    if (nowMsArg - lastSnapshotMs_ >= snapshotMs) {
        lastSnapshotMs_ = nowMsArg;
        for (auto& kv : peers_)
            if (kv.second.welcomed) sendSnapshot(kv.second, nowMsArg);
    }
    if (nowMsArg - lastDigestMs_ >= kDigestMs) {
        lastDigestMs_ = nowMsArg;
        broadcastDigest();
    }
    if (nowMsArg - lastPingMs_ >= 2000) {
        lastPingMs_ = nowMsArg;
        BitWriter w(16);
        w.u32v(nowMsArg);
        w.u32v(static_cast<u32>(w_.connectedCount()));
        link_.broadcast(MsgType::HostInfo, Channel::Control, w, false);
    }
}

void HostSession::handleEvent(const NetEvent& ev) {
    switch (ev.type) {
        case NetEvent::Type::Connected: {
            Peer p;
            p.linkId = ev.peer;
            p.lastHeardMs = nowMs_;
            peers_[ev.peer] = p;
            NFC_INFO("host: link %u connected", ev.peer);
            break;
        }
        case NetEvent::Type::Disconnected: {
            auto it = peers_.find(ev.peer);
            if (it == peers_.end()) break;
            const PlayerId slot = it->second.slot;
            peers_.erase(it);
            if (slot != kNoPlayer && slot < kMaxPlayers) {
                w_.players[slot].connected = false;
                w_.players[slot].ready = false;
                w_.players[slot].loaded = false;
                if (w_.race.p[slot].joined && !w_.race.p[slot].finished && !w_.race.p[slot].dnf)
                    w_.race.p[slot].dnf = true;
                NFC_INFO("host: slot %u disconnected", slot);
                broadcastPlayerList();
                broadcastRace();
                game_.onNotice(w_.players[slot].name + " disconnected");
            }
            break;
        }
        case NetEvent::Type::Received: {
            auto it = peers_.find(ev.peer);
            if (it == peers_.end()) break;
            it->second.lastHeardMs = nowMs_;
            stats_.received++;
            stats_.receivedBytes += ev.payload.size();
            BitReader r(ev.payload);
            switch (ev.msg) {
                case MsgType::Hello: onHello(ev); break;
                case MsgType::PlayerState: {
                    BitReader rr(ev.payload);
                    onPlayerState(it->second, rr);
                    break;
                }
                case MsgType::RaceReady: {
                    BitReader rr(ev.payload);
                    onRaceReady(it->second, rr);
                    break;
                }
                case MsgType::RaceProgress: {
                    BitReader rr(ev.payload);
                    onRaceProgress(it->second, rr);
                    break;
                }
                case MsgType::ProgressEvent: {
                    BitReader rr(ev.payload);
                    onProgressEvent(it->second, rr);
                    break;
                }
                case MsgType::Chat: {
                    BitReader rr(ev.payload);
                    onChat(it->second, rr);
                    break;
                }
                case MsgType::Zone: {
                    BitReader rr(ev.payload);
                    onZone(it->second, rr);
                    break;
                }
                case MsgType::RepairRequest: {
                    BitReader rr(ev.payload);
                    onRepairRequest(it->second, rr);
                    break;
                }
                case MsgType::Digest: {
                    BitReader rr(ev.payload);
                    onDigest(it->second, rr);
                    break;
                }
                case MsgType::Ping: {
                    BitReader rr(ev.payload);
                    const u32 nonce = rr.u32v();
                    const u32 clientMs = rr.u32v();
                    BitWriter w(16);
                    w.u32v(nonce);
                    w.u32v(clientMs);
                    w.u32v(nowMs_);
                    link_.send(ev.peer, MsgType::Pong, Channel::Control, w, false);
                    break;
                }
                default:
                    break;
            }
            break;
        }
    }
}

void HostSession::onHello(const NetEvent& ev) {
    auto it = peers_.find(ev.peer);
    if (it == peers_.end()) return;
    Peer& p = it->second;
    if (p.welcomed) return;

    BitReader r(ev.payload);
    HelloMsg hello;
    if (!hello.decode(r)) {
        stats_.badPackets++;
        NFC_WARN("host: bad hello from link %u", ev.peer);
        link_.disconnect(ev.peer, true);
        return;
    }
    if (hello.protocol != kProtocolVersion) {
        BitWriter w(64);
        w.u8v(1);
        w.str("protocol mismatch: host " + std::string(versionString()) + ", you sent " +
              std::to_string(hello.protocol >> 16) + "." + std::to_string((hello.protocol >> 8) & 0xFF) + "." +
              std::to_string(hello.protocol & 0xFF));
        link_.send(ev.peer, MsgType::Reject, Channel::Control, w, true);
        link_.disconnect(ev.peer);
        NFC_WARN("host: rejected link %u: protocol %08x != %08x", ev.peer, hello.protocol, kProtocolVersion);
        return;
    }
    const PlayerId slot = freeSlot();
    if (slot == kNoPlayer) {
        BitWriter w(64);
        w.u8v(2);
        w.str("session full (8 players)");
        link_.send(ev.peer, MsgType::Reject, Channel::Control, w, true);
        link_.disconnect(ev.peer);
        return;
    }

    p.slot = slot;
    p.welcomed = true;
    p.carId = allocId();
    p.lastSeq = 0;

    PlayerInfo& pi = w_.players[slot];
    pi.slot = slot;
    pi.connected = true;
    pi.loaded = false;
    pi.name = cleanName(hello.name);
    pi.carId = p.carId;
    pi.zoneId = 0;
    pi.ready = false;
    Entity& car = w_.upsert(p.carId, EKind::Player);
    car.owner = slot;
    car.active = true;

    sendWelcome(p);
    sendRace(slot);
    sendProgress(slot);
    sendSnapshot(p, nowMs_);
    broadcastPlayerList();
    game_.onNotice(pi.name + " joined");
    NFC_INFO("host: slot %u = '%s' (link %u)", slot, pi.name.c_str(), ev.peer);
}

void HostSession::sendWelcome(Peer& p) {
    WelcomeMsg msg;
    msg.slot = p.slot;
    msg.sessionId = w_.sessionId;
    msg.seed = w_.sessionSeed;
    msg.hostMs = nowMs_;
    msg.snapshotHz = static_cast<u16>(cfg_.snapshotHz);
    msg.features = 1 | 2 | 4;  // career, races, traffic mirror
    msg.hostName = w_.hostName;
    msg.sessionName = w_.sessionName;
    msg.yourCar = p.carId;
    BitWriter w(128);
    msg.encode(w);
    link_.send(p.linkId, MsgType::Welcome, Channel::Control, w, true);
}

void HostSession::sendRace(PlayerId slot) {
    Peer* p = peerForSlot(slot);
    if (!p) return;
    BitWriter w(160);
    w_.race.encode(w);
    link_.send(p->linkId, MsgType::RaceCmd, Channel::Control, w, true);
}

void HostSession::broadcastRace() {
    BitWriter w(160);
    w_.race.encode(w);
    for (auto& kv : peers_)
        if (kv.second.welcomed) link_.send(kv.second.linkId, MsgType::RaceCmd, Channel::Control, w, true);
}

void HostSession::sendProgress(PlayerId slot) {
    Peer* p = peerForSlot(slot);
    if (!p) return;
    BitWriter w(512);
    w_.progress.encode(w);
    link_.send(p->linkId, MsgType::ProgressSnapshot, Channel::Control, w, true);
}

void HostSession::broadcastProgress() {
    BitWriter w(512);
    w_.progress.encode(w);
    for (auto& kv : peers_)
        if (kv.second.welcomed) link_.send(kv.second.linkId, MsgType::ProgressSnapshot, Channel::Control, w, true);
}

void HostSession::broadcastPlayerList() {
    BitWriter w(512);
    u8 count = 0;
    for (const PlayerInfo& p : w_.players)
        if (p.connected) ++count;
    w.u8v(count);
    for (const PlayerInfo& p : w_.players)
        if (p.connected) p.encode(w);
    for (auto& kv : peers_)
        if (kv.second.welcomed) link_.send(kv.second.linkId, MsgType::PlayerList, Channel::Control, w, true);
}

void HostSession::broadcastDigest() {
    DigestMsg d;
    d.hostMs = nowMs_;
    d.tick = nowMs_ / 1000;
    d.progressRevision = w_.progress.revision;
    d.raceVersion = w_.race.version;
    d.entities = 0;  // per-client entity sets are culled, so not comparable
    d.race = w_.race.digest();
    d.progress = w_.progress.digest();
    BitWriter w(64);
    d.encode(w);
    for (auto& kv : peers_)
        if (kv.second.welcomed) link_.send(kv.second.linkId, MsgType::Digest, Channel::Control, w, true);
}

void HostSession::broadcastNotice(const std::string& text) {
    BitWriter w(256);
    w.u8v(kNoPlayer);
    w.str(text);
    for (auto& kv : peers_)
        if (kv.second.welcomed) link_.send(kv.second.linkId, MsgType::Chat, Channel::Control, w, true);
}

void HostSession::updateLocalCar(u32 nowMsArg) {
    CarState st;
    if (game_.readLocalCar(st) && st.sane()) {
        Entity& e = w_.upsert(localCarId_, EKind::Player);
        e.kind = EKind::Player;
        e.owner = 0;
        e.active = true;
        e.observe(st, nowMsArg);
        w_.players[0].loaded = true;
        w_.players[0].zoneId = game_.zoneId();
    } else {
        w_.players[0].loaded = false;
    }
}

void HostSession::mirrorGameEntities(u32 nowMsArg) {
    std::vector<GameEntityView> views;
    game_.readGameEntities(views);
    std::set<u32> seen;
    u32 traffic = static_cast<u32>(w_.activeCount(EKind::Traffic));
    u32 bots = static_cast<u32>(w_.activeCount(EKind::Bot));
    for (const GameEntityView& v : views) {
        if (!v.st.sane() || v.handle == 0) continue;
        auto known = handleToEntity_.find(v.handle);
        if (known != handleToEntity_.end()) {
            Entity* e = w_.find(known->second);
            if (e) {
                e->observe(v.st, nowMsArg);
                e->model = v.model;
                e->active = true;
                lastSeenMs_[e->id] = nowMsArg;
                seen.insert(v.handle);
            }
            continue;
        }
        if (v.kind == EKind::Traffic && traffic >= cfg_.maxTraffic) continue;
        if (v.kind == EKind::Bot && bots >= cfg_.maxBots) continue;
        const EntityId id = allocId();
        Entity& e = w_.upsert(id, v.kind);
        e.kind = v.kind;
        e.model = v.model;
        e.owner = 0;  // the host mirrors what its own engine simulates
        e.active = true;
        e.observe(v.st, nowMsArg);
        handleToEntity_[v.handle] = id;
        entityToHandle_[id] = v.handle;
        lastSeenMs_[id] = nowMsArg;
        seen.insert(v.handle);
        if (v.kind == EKind::Traffic) ++traffic;
        if (v.kind == EKind::Bot) ++bots;
        NFC_DEBUG("host: spawn %s id %u model %u", toString(v.kind), id, v.model);
    }
    // The engine despawned a car: tell clients so the puppet goes away now
    // instead of after the timeout.
    for (auto it = handleToEntity_.begin(); it != handleToEntity_.end();) {
        if (seen.count(it->first)) {
            ++it;
            continue;
        }
        const EntityId id = it->second;
        BitWriter w(8);
        w.u32v(id);
        for (auto& kv : peers_)
            if (kv.second.welcomed) link_.send(kv.second.linkId, MsgType::EntityRemove, Channel::Control, w, true);
        w_.remove(id);
        entityToHandle_.erase(id);
        lastSeenMs_.erase(id);
        it = handleToEntity_.erase(it);
    }
}

void HostSession::dropStaleEntities(u32 nowMsArg) {
    std::vector<EntityId> gone;
    for (auto& kv : w_.entities) {
        Entity& e = kv.second;
        if (e.kind == EKind::Player) continue;
        auto it = lastSeenMs_.find(e.id);
        if (it == lastSeenMs_.end() || nowMsArg - it->second > 2000) gone.push_back(e.id);
    }
    for (EntityId id : gone) {
        w_.remove(id);
        lastSeenMs_.erase(id);
        auto h = entityToHandle_.find(id);
        if (h != entityToHandle_.end()) {
            handleToEntity_.erase(h->second);
            entityToHandle_.erase(h);
        }
    }
}

void HostSession::driveRace(u32 nowMsArg) {
    const RacePhase before = w_.race.phase;

    if (before == RacePhase::Lobby) {
        bool allReady = true;
        int joined = 0;
        for (int i = 0; i < kMaxPlayers; ++i) {
            if (!w_.race.p[i].joined) continue;
            ++joined;
            if (!w_.race.p[i].ready && w_.players[i].connected) allReady = false;
        }
        const bool timedOut = nowMsArg - w_.race.openAtMs > 20000;
        if (joined > 0 && ((allReady && nowMsArg - w_.race.openAtMs >= cfg_.lobbySettleMs) || timedOut)) {
            raceBeginCountdown(w_, nowMsArg, cfg_.countdownMs);
            stats_.raceStarts++;
            NFC_INFO("host: countdown (allReady=%d joined=%d)", allReady ? 1 : 0, joined);
        }
    }

    // A race nobody can finish (no checkpoint binding, a player stuck in a
    // menu) must not lock the session forever.
    if (w_.race.phase == RacePhase::Racing && nowMsArg - w_.race.startAtMs > 20u * 60u * 1000u) {
        NFC_WARN("host: race %u timed out after 20 minutes, aborting", w_.race.eventId);
        raceAbort(w_, nowMsArg);
        broadcastNotice("race timed out");
    }
    raceTick(w_, nowMsArg);

    if (w_.race.phase != before) {
        broadcastRace();
        game_.onRace(w_.race, before);
        if (w_.race.phase == RacePhase::Racing) broadcastNotice("GO!");
        if (w_.race.phase == RacePhase::Finished) broadcastNotice("race finished");
    }
    // Whoever crossed the line, the results are applied exactly once, here.
    if (w_.race.phase == RacePhase::Finished && !resultsApplied_) {
        resultsApplied_ = true;
        raceApplyResults(w_, nowMsArg);
        broadcastProgress();
        game_.onCareer(w_.progress);
    }
}

void HostSession::sendSnapshot(Peer& p, u32 nowMsArg) {
    if (p.slot == kNoPlayer) return;
    Vec3 center{};
    bool haveCenter = false;
    if (const Entity* own = w_.find(p.carId)) {
        center = own->st.pos;
        haveCenter = true;
    }
    std::vector<EntityView> list;
    list.reserve(w_.entities.size());
    for (const auto& kv : w_.entities) {
        const Entity& e = kv.second;
        if (!e.active) continue;
        if (e.id == p.carId) continue;
        if (e.kind != EKind::Player && haveCenter &&
            length(e.st.pos - center) > static_cast<f32>(cfg_.cullDistanceM))
            continue;
        EntityView v;
        v.id = e.id;
        v.kind = e.kind;
        v.model = e.model;
        v.owner = e.owner;
        v.epoch = e.epoch;
        v.st = e.st;
        list.push_back(v);
    }
    const u32 tick = nowMsArg / std::max(10u, 1000u / std::max(1u, cfg_.snapshotHz));
    for (std::size_t i = 0; i < list.size();) {
        const std::size_t n = std::min<std::size_t>(kSnapshotEntitiesPerPacket, list.size() - i);
        BitWriter w(1100);
        w.u32v(tick);
        w.u32v(nowMsArg);
        w.u8v(static_cast<u8>(n));
        for (std::size_t j = 0; j < n; ++j) list[i + j].encode(w);
        link_.send(p.linkId, MsgType::Snapshot, Channel::State, w, false);
        stats_.sent++;
        stats_.sentBytes += w.size();
        i += n;
    }
    if (list.empty()) {
        BitWriter w(16);
        w.u32v(tick);
        w.u32v(nowMsArg);
        w.u8v(0);
        link_.send(p.linkId, MsgType::Snapshot, Channel::State, w, false);
    }
}

void HostSession::onPlayerState(const Peer& p, BitReader& r) {
    if (p.slot == kNoPlayer) return;
    const u8 slot = r.u8v();
    const u32 seq = r.u32v();
    const u32 carId = r.u32v();
    const u32 zone = r.u32v();
    const u16 cp = r.u16v();
    const u16 lap = r.u16v();
    const u32 t = r.u32v();
    const u8 finished = r.u8v();
    CarState st;
    st.decode(r);
    if (!r.ok() || !st.sane()) {
        stats_.badPackets++;
        return;
    }
    if (slot != p.slot || carId != p.carId) {
        stats_.badPackets++;
        NFC_WARN("host: slot %u sent state for slot %u car %u (expected %u)", p.slot, slot, carId, p.carId);
        return;
    }
    Peer* mp = const_cast<Peer*>(&p);
    if (mp->lastSeq != 0 && !seqNewer(seq, mp->lastSeq)) {
        stats_.staleDropped++;
        return;
    }
    mp->lastSeq = seq;

    Entity& e = w_.upsert(p.carId, EKind::Player);
    e.kind = EKind::Player;
    e.owner = p.slot;
    e.active = true;
    e.observe(st, nowMs_);
    // Let the host's game adapter track the guest car too (puppet bookkeeping,
    // and the future render path), not just the snapshots.
    if (!e.spawnSent) {
        e.spawnSent = true;
        game_.onSpawn(p.carId, EKind::Player, e.model, st);
    } else {
        game_.onUpdate(p.carId, EKind::Player, st);
    }
    PlayerInfo& pi = w_.players[p.slot];
    pi.loaded = true;
    pi.zoneId = zone;
    if (w_.race.phase == RacePhase::Racing && !pi.finished) {
        pi.checkpoint = cp;
        pi.lap = lap;
        pi.raceTimeMs = t;
        pi.finished = finished != 0;
    }
}

void HostSession::onRaceReady(const Peer& p, BitReader& r) {
    const u16 eventId = r.u16v();
    const u8 join = r.u8v();
    const u8 ready = r.u8v();
    if (!r.ok() || p.slot == kNoPlayer) return;
    if (join && w_.race.phase == RacePhase::Lobby && eventId == w_.race.eventId) w_.race.p[p.slot].joined = true;
    if (raceSetReady(w_, p.slot, ready != 0, eventId)) broadcastRace();
}

void HostSession::onRaceProgress(const Peer& p, BitReader& r) {
    const u16 cp = r.u16v();
    const u16 lap = r.u16v();
    const u32 t = r.u32v();
    const u8 finished = r.u8v();
    if (!r.ok() || p.slot == kNoPlayer) return;
    bool changed = false;
    if (finished)
        changed = raceSetFinished(w_, p.slot, t);
    else
        changed = raceSetProgress(w_, p.slot, cp, lap, t);
    if (changed) broadcastRace();
}

void HostSession::onProgressEvent(const Peer& p, BitReader& r) {
    const u32 key = r.u32v();
    if (!r.ok() || p.slot == kNoPlayer) return;
    applyCareer(key);
    (void)p;
}

bool HostSession::applyCareer(u32 key) {
    const u32 type = (key >> 16) & 0xFF;
    const u32 id = key & 0xFFFF;
    if (type < 1 || type > 6 || id > 1024) {
        NFC_WARN("career: rejected key %08x", key);
        return false;
    }
    if (!w_.progress.apply(key)) return false;
    NFC_INFO("career: applied %s (rev %u cash %u rep %u)", careerEventName(key), w_.progress.revision,
             w_.progress.cash, w_.progress.rep);
    broadcastProgress();
    broadcastPlayerList();
    game_.onCareer(w_.progress);
    broadcastNotice(std::string("career: ") + careerEventName(key) + " done");
    return true;
}

void HostSession::onChat(const Peer& p, BitReader& r) {
    const std::string text = r.str();
    if (!r.ok() || text.empty()) return;
    BitWriter w(300);
    w.u8v(p.slot);
    w.str(text);
    for (auto& kv : peers_)
        if (kv.second.welcomed) link_.send(kv.second.linkId, MsgType::Chat, Channel::Control, w, true);
    game_.onNotice(w_.players[p.slot].name + ": " + text);
}

void HostSession::onZone(const Peer& p, BitReader& r) {
    const u32 zone = r.u32v();
    if (!r.ok()) return;
    if (p.slot != kNoPlayer) w_.players[p.slot].zoneId = zone;
}

void HostSession::onRepairRequest(const Peer& p, BitReader& r) {
    const u8 category = r.u8v();
    if (!r.ok() && p.slot == kNoPlayer) return;
    stats_.repairs++;
    if (category == 1 || category == 0) sendProgress(p.slot);
    if (category == 2 || category == 0) sendRace(p.slot);
    if (category == 3 || category == 0) {
        Peer* mp = peerForSlot(p.slot);
        if (mp) sendSnapshot(*mp, nowMs_);
    }
    NFC_INFO("host: repair category %u sent to slot %u", category, p.slot);
}

void HostSession::onDigest(const Peer& p, BitReader& r) {
    DigestMsg d;
    if (!d.decode(r) || p.slot == kNoPlayer) return;
    stats_.digestChecks++;
    const bool raceOk = d.race == w_.race.digest();
    const bool progOk = d.progressRevision == w_.progress.revision;
    if (!raceOk || !progOk) {
        stats_.digestMismatches++;
        NFC_WARN("host: slot %u digest mismatch (race %d progress %d)", p.slot, raceOk ? 1 : 0, progOk ? 1 : 0);
        sendRace(p.slot);
        sendProgress(p.slot);
    }
}

bool HostSession::openRace(u16 eventId, u32 routeId, u16 checkpointCount) {
    if (!raceOpen(w_, eventId, routeId, checkpointCount, nowMs_)) return false;
    resultsApplied_ = false;
    if (w_.race.p[0].joined) {
        w_.race.p[0].ready = true;
        w_.players[0].ready = true;
    }
    w_.race.version++;
    broadcastRace();
    game_.onRace(w_.race, RacePhase::Idle);
    broadcastNotice("event " + std::to_string(eventId) + " open: press F5 to join");
    return true;
}

bool HostSession::reportLocalProgress(u16 checkpoint, u16 lap, u32 timeMs) {
    if (raceSetProgress(w_, 0, checkpoint, lap, timeMs)) {
        broadcastRace();
        return true;
    }
    return false;
}

bool HostSession::reportLocalFinished(u32 timeMs) {
    if (raceSetFinished(w_, 0, timeMs)) {
        broadcastRace();
        return true;
    }
    return false;
}

bool HostSession::forceStart() {
    if (w_.race.phase != RacePhase::Lobby) return false;
    if (!w_.race.p[0].joined && w_.players[0].loaded) {
        w_.race.p[0].joined = true;
        w_.race.p[0].ready = true;
        w_.players[0].ready = true;
    }
    if (!raceBeginCountdown(w_, nowMs_)) return false;
    broadcastRace();
    game_.onRace(w_.race, RacePhase::Lobby);
    return true;
}

bool HostSession::abortRace() {
    if (!raceAbort(w_, nowMs_)) return false;
    broadcastRace();
    game_.onRace(w_.race, RacePhase::Racing);
    return true;
}

void HostSession::chat(const std::string& text) {
    BitWriter w(300);
    w.u8v(kNoPlayer);
    w.str(text);
    for (auto& kv : peers_)
        if (kv.second.welcomed) link_.send(kv.second.linkId, MsgType::Chat, Channel::Control, w, true);
    game_.onNotice(text);
}

void HostSession::kick(PlayerId slot) {
    Peer* p = peerForSlot(slot);
    if (p) link_.disconnect(p->linkId, true);
}

void HostSession::resync(PlayerId slot) {
    if (slot == kNoPlayer) {
        broadcastRace();
        broadcastProgress();
        return;
    }
    sendRace(slot);
    sendProgress(slot);
    Peer* p = peerForSlot(slot);
    if (p) sendSnapshot(*p, nowMs_);
}

std::string HostSession::statusText() const {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "HOST '%s' | %d players | %s%s%u | traffic %zu bots %zu | %s", cfg_.playerName.c_str(),
                  w_.connectedCount(), toString(w_.race.phase),
                  w_.race.phase == RacePhase::Idle ? "" : " event ", w_.race.eventId,
                  w_.activeCount(EKind::Traffic), w_.activeCount(EKind::Bot), w_.sessionName.c_str());
    return buf;
}

// ===========================================================================
// ClientSession
// ===========================================================================
ClientSession::ClientSession(IGameWorld& game, SessionConfig cfg) : game_(game), cfg_(std::move(cfg)) {
    w_.reset();
}

ClientSession::~ClientSession() { stop(); }

bool ClientSession::start() {
    if (running_) return false;
    w_.reset();
    w_.localSlot = kNoPlayer;
    if (!link_.startClient()) return false;
    running_ = true;
    connecting_ = true;
    lastConnectAttemptMs_ = 0;
    connectStartedMs_ = static_cast<u32>(nowMs());
    return true;
}

void ClientSession::stop() {
    if (!running_) return;
    running_ = false;
    connected_ = false;
    welcomed_ = false;
    link_.close();
}

void ClientSession::tick(u32 nowMsArg) {
    if (!running_) return;
    nowMs_ = nowMsArg;

    std::vector<NetEvent> events;
    link_.poll(events);
    for (const NetEvent& ev : events) handleEvent(ev);

    if (!connected_ && nowMsArg - lastConnectAttemptMs_ >= 3000) {
        lastConnectAttemptMs_ = nowMsArg;
        if (!link_.running()) link_.startClient();
        if (!link_.connect(cfg_.serverHost, cfg_.serverPort)) {
            link_.close();
        } else {
            connecting_ = true;
            NFC_INFO("client: connecting to %s:%u", cfg_.serverHost.c_str(), cfg_.serverPort);
        }
    }

    if (connected_) {
        publishLocalCar(nowMsArg);
        maybeFinishCountdown(nowMsArg);
        checkTimeouts(nowMsArg);
        sendPing(nowMsArg);
        if (nowMsArg - lastDigestMs_ >= kDigestMs) {
            lastDigestMs_ = nowMsArg;
            DigestMsg d;
            d.hostMs = hostNowMs(nowMsArg);
            d.tick = nowMsArg / 1000;
            d.progressRevision = w_.progress.revision;
            d.raceVersion = w_.race.version;
            d.race = w_.race.digest();
            d.progress = w_.progress.digest();
            BitWriter w(64);
            d.encode(w);
            link_.send(1, MsgType::Digest, Channel::Control, w, true);
        }
        // Forget puppets the host stopped mentioning.
        std::vector<EntityId> gone;
        for (auto& kv : w_.entities) {
            const Entity& e = kv.second;
            if (e.kind == EKind::Player) continue;
            auto it = lastSeenMs_.find(e.id);
            if (it == lastSeenMs_.end() || nowMsArg - it->second > 4000) gone.push_back(e.id);
        }
        for (EntityId id : gone) {
            const Entity* e = w_.find(id);
            EKind kind = e ? e->kind : EKind::Traffic;
            game_.onDespawn(id, kind);
            w_.remove(id);
            lastSeenMs_.erase(id);
        }
    }
}

void ClientSession::handleEvent(const NetEvent& ev) {
    switch (ev.type) {
        case NetEvent::Type::Connected: {
            HelloMsg hello;
            hello.name = cfg_.playerName;
            hello.protocol = kProtocolVersion;
            hello.nonce = static_cast<u32>(mix64(nowMs_, reinterpret_cast<std::uintptr_t>(this)));
            BitWriter w(96);
            hello.encode(w);
            link_.send(ev.peer, MsgType::Hello, Channel::Control, w, true);
            NFC_INFO("client: link up, hello sent");
            break;
        }
        case NetEvent::Type::Disconnected: {
            const bool was = connected_;
            connected_ = false;
            welcomed_ = false;
            link_.close();
            w_.race.reset();
            lastPhase_ = RacePhase::Idle;
            if (was) game_.onConnection(false, "disconnected");
            game_.onNotice("disconnected from host");
            NFC_INFO("client: disconnected");
            break;
        }
        case NetEvent::Type::Received: {
            lastPacketMs_ = nowMs_;
            stats_.received++;
            stats_.receivedBytes += ev.payload.size();
            BitReader r(ev.payload);
            switch (ev.msg) {
                case MsgType::Welcome: applyWelcome(r); break;
                case MsgType::Reject: {
                    const u8 reason = r.u8v();
                    const std::string text = r.str();
                    NFC_ERROR("client: rejected (%u): %s", reason, text.c_str());
                    game_.onNotice("join rejected: " + text);
                    connected_ = false;
                    link_.close();
                    break;
                }
                case MsgType::Snapshot: applySnapshot(r); break;
                case MsgType::EntityRemove: {
                    const EntityId id = r.u32v();
                    if (r.ok()) {
                        const Entity* e = w_.find(id);
                        EKind kind = e ? e->kind : EKind::Traffic;
                        game_.onDespawn(id, kind);
                        w_.remove(id);
                        lastSeenMs_.erase(id);
                    }
                    break;
                }
                case MsgType::RaceCmd: applyRace(r); break;
                case MsgType::ProgressSnapshot: applyProgress(r); break;
                case MsgType::PlayerList: applyPlayerList(r); break;
                case MsgType::RepairData: applyRepair(r); break;
                case MsgType::Digest: applyDigest(r); break;
                case MsgType::Chat: {
                    const u8 slot = r.u8v();
                    const std::string text = r.str();
                    if (r.ok()) game_.onNotice(slot == kNoPlayer ? text : (w_.players[slot].name + ": " + text));
                    break;
                }
                case MsgType::Pong: {
                    const u32 nonce = r.u32v();
                    const u32 clientMs = r.u32v();
                    const u32 hostMs = r.u32v();
                    if (r.ok() && nonce == pingNonce_) {
                        const u32 now = static_cast<u32>(nowMs_);
                        const u32 rtt = now - clientMs;
                        rttMs_ = rtt;
                        const u32 guess = hostMs + rtt / 2 - now;
                        if (rtt <= 120 || offsetMs_ == 0) offsetMs_ = guess;
                    }
                    break;
                }
                default:
                    break;
            }
            break;
        }
    }
}

void ClientSession::applyWelcome(BitReader& r) {
    WelcomeMsg msg;
    if (!msg.decode(r)) {
        stats_.badPackets++;
        return;
    }
    w_.sessionId = msg.sessionId;
    w_.sessionSeed = msg.seed;
    w_.sessionName = msg.sessionName;
    w_.hostName = msg.hostName;
    w_.localSlot = msg.slot;
    w_.players[msg.slot].slot = msg.slot;
    w_.players[msg.slot].connected = true;
    w_.players[msg.slot].name = cfg_.playerName;
    w_.players[msg.slot].carId = msg.yourCar;
    myCarId_ = msg.yourCar;
    Entity& own = w_.upsert(myCarId_, EKind::Player);
    own.owner = msg.slot;
    own.active = true;
    offsetMs_ = static_cast<u32>(static_cast<i32>(msg.hostMs) - static_cast<i32>(nowMs_));
    connected_ = true;
    welcomed_ = true;
    badDigests_ = 0;
    game_.onConnection(true, w_.hostName + " / " + w_.sessionName);
    game_.onNotice("joined " + w_.hostName + "'s session");
    NFC_INFO("client: welcome slot=%u car=%u session=%08x seed=%08x hostMs=%u", msg.slot, msg.yourCar, msg.sessionId,
             msg.seed, msg.hostMs);
}

void ClientSession::applySnapshot(BitReader& r) {
    const u32 tick = r.u32v();
    const u32 hostMs = r.u32v();
    const u8 count = r.u8v();
    if (!r.ok()) {
        stats_.badPackets++;
        return;
    }
    (void)tick;
    (void)hostMs;
    for (u8 i = 0; i < count && r.ok(); ++i) {
        EntityView v;
        if (!v.decode(r)) break;
        if (v.id == myCarId_) continue;
        Entity* e = w_.find(v.id);
        if (!e) {
            Entity& ne = w_.upsert(v.id, v.kind);
            ne.kind = v.kind;
            ne.model = v.model;
            ne.owner = v.owner;
            ne.epoch = v.epoch;
            ne.active = true;
            ne.observe(v.st, nowMs_);
            game_.onSpawn(v.id, v.kind, v.model, v.st);
        } else {
            e->kind = v.kind;
            e->model = v.model;
            e->owner = v.owner;
            e->epoch = v.epoch;
            e->observe(v.st, nowMs_);
            game_.onUpdate(v.id, v.kind, v.st);
        }
        lastSeenMs_[v.id] = nowMs_;
    }
    stats_.receivedBytes += r.pos();
}

void ClientSession::applyRace(BitReader& r) {
    RaceState st;
    if (!st.decode(r)) {
        stats_.badPackets++;
        return;
    }
    const RacePhase prev = lastPhase_;
    w_.race = st;
    if (st.phase != prev) {
        lastPhase_ = st.phase;
        raceNotified_ = true;
        game_.onRace(st, prev);
        if (st.phase == RacePhase::Racing && prev == RacePhase::Countdown) game_.onNotice("GO!");
        if (st.phase == RacePhase::Finished) game_.onNotice("race finished");
    }
}

void ClientSession::applyProgress(BitReader& r) {
    ProgressLedger ledger;
    if (!ledger.decode(r)) {
        stats_.badPackets++;
        return;
    }
    if (ledger.revision == w_.progress.revision && ledger.digest() == w_.progress.digest()) return;
    w_.progress = ledger;
    game_.onCareer(w_.progress);
    NFC_INFO("client: career rev %u cash %u rep %u", w_.progress.revision, w_.progress.cash, w_.progress.rep);
}

void ClientSession::applyPlayerList(BitReader& r) {
    const u8 count = r.u8v();
    if (!r.ok()) return;
    for (int i = 0; i < kMaxPlayers; ++i) {
        if (i == w_.localSlot) continue;
        w_.players[i].connected = false;
    }
    for (u8 i = 0; i < count && r.ok(); ++i) {
        PlayerInfo p;
        if (!p.decode(r)) break;
        if (p.slot >= kMaxPlayers) continue;
        if (p.slot != w_.localSlot) w_.players[p.slot] = p;
        else w_.players[p.slot].rttMs = p.rttMs;
    }
}

void ClientSession::applyRepair(BitReader& r) {
    const u8 category = r.u8v();
    if (!r.ok()) return;
    stats_.repairs++;
    if (category == 1) {
        ProgressLedger ledger;
        if (ledger.decode(r)) {
            w_.progress = ledger;
            game_.onCareer(w_.progress);
        }
    } else if (category == 2) {
        RaceState st;
        if (st.decode(r)) {
            const RacePhase prev = lastPhase_;
            w_.race = st;
            if (st.phase != prev) {
                lastPhase_ = st.phase;
                game_.onRace(st, prev);
            }
        }
    }
}

void ClientSession::applyDigest(BitReader& r) {
    DigestMsg d;
    if (!d.decode(r)) return;
    stats_.digestChecks++;
    const bool raceOk = d.race == w_.race.digest();
    const bool progOk = d.progressRevision == w_.progress.revision;
    if (raceOk && progOk) {
        badDigests_ = 0;
        return;
    }
    stats_.digestMismatches++;
    ++badDigests_;
    NFC_WARN("client: digest mismatch (race %d progress %d, rev %u vs %u)", raceOk ? 1 : 0, progOk ? 1 : 0,
             d.progressRevision, w_.progress.revision);
    if (badDigests_ >= 2) {
        badDigests_ = 0;
        BitWriter w(8);
        w.u8v(0);
        link_.send(1, MsgType::RepairRequest, Channel::Control, w, true);
    }
}

void ClientSession::publishLocalCar(u32 nowMsArg) {
    const u32 interval = std::max(10u, 1000u / std::max(1u, cfg_.snapshotHz));
    if (nowMsArg - lastStateMs_ < interval) return;
    CarState st;
    if (!game_.readLocalCar(st) || !st.sane()) return;
    lastStateMs_ = nowMsArg;
    Entity& own = w_.upsert(myCarId_, EKind::Player);
    own.owner = w_.localSlot;
    own.active = true;
    own.observe(st, nowMsArg);
    w_.players[w_.localSlot].loaded = true;
    w_.players[w_.localSlot].zoneId = game_.zoneId();

    BitWriter w(96);
    w.u8v(static_cast<u8>(w_.localSlot));
    w.u32v(++seq_);
    w.u32v(myCarId_);
    w.u32v(game_.zoneId());
    w.u16v(w_.players[w_.localSlot].checkpoint);
    w.u16v(w_.players[w_.localSlot].lap);
    w.u32v(w_.players[w_.localSlot].raceTimeMs);
    w.u8v(w_.players[w_.localSlot].finished ? 1 : 0);
    st.encode(w);
    link_.send(1, MsgType::PlayerState, Channel::State, w, false);
    stats_.sent++;
    stats_.sentBytes += w.size();
}

void ClientSession::maybeFinishCountdown(u32 nowMsArg) {
    if (w_.race.phase != RacePhase::Countdown || lastPhase_ == RacePhase::Racing) return;
    const u32 hn = hostNowMs(nowMsArg);
    if (static_cast<i32>(hn - w_.race.startAtMs) < 0) return;
    RaceState st = w_.race;
    st.phase = RacePhase::Racing;
    lastPhase_ = RacePhase::Racing;
    game_.onRace(st, RacePhase::Countdown);
}

void ClientSession::checkTimeouts(u32 nowMsArg) {
    if (!connected_) return;
    if (nowMsArg - lastPacketMs_ > 8000) {
        NFC_WARN("client: host silent for %ums, dropping", nowMsArg - lastPacketMs_);
        connected_ = false;
        welcomed_ = false;
        link_.close();
        game_.onConnection(false, "timeout");
        game_.onNotice("lost connection to host");
    }
}

void ClientSession::sendPing(u32 nowMsArg) {
    if (nowMsArg - lastPingMs_ < 1000) return;
    lastPingMs_ = nowMsArg;
    pingNonce_ = static_cast<u32>(mix64(nowMsArg, 0xABCDEF));
    pingSentMs_ = nowMsArg;
    BitWriter w(16);
    w.u32v(pingNonce_);
    w.u32v(nowMsArg);
    link_.send(1, MsgType::Ping, Channel::Control, w, false);
}

bool ClientSession::setReady(bool ready) {
    if (!connected_) return false;
    w_.players[w_.localSlot].ready = ready;
    BitWriter w(8);
    w.u16v(w_.race.eventId);
    w.u8v(w_.race.phase == RacePhase::Lobby ? 1 : 0);
    w.u8v(ready ? 1 : 0);
    return link_.send(1, MsgType::RaceReady, Channel::Control, w, true);
}

void ClientSession::reportProgress(u16 checkpoint, u16 lap, u32 timeMs) {
    if (!connected_) return;
    PlayerInfo& pi = w_.players[w_.localSlot];
    pi.checkpoint = checkpoint;
    pi.lap = lap;
    pi.raceTimeMs = timeMs;
    BitWriter w(16);
    w.u16v(checkpoint);
    w.u16v(lap);
    w.u32v(timeMs);
    w.u8v(0);
    link_.send(1, MsgType::RaceProgress, Channel::Control, w, true);
}

void ClientSession::reportFinished(u32 timeMs) {
    if (!connected_) return;
    PlayerInfo& pi = w_.players[w_.localSlot];
    pi.finished = true;
    pi.finishMs = timeMs;
    pi.raceTimeMs = timeMs;
    BitWriter w(16);
    w.u16v(w_.race.checkpointCount);
    w.u16v(w_.players[w_.localSlot].lap);
    w.u32v(timeMs);
    w.u8v(1);
    link_.send(1, MsgType::RaceProgress, Channel::Control, w, true);
}

void ClientSession::reportCareerEvent(u32 key) {
    if (!connected_) return;
    BitWriter w(8);
    w.u32v(key);
    link_.send(1, MsgType::ProgressEvent, Channel::Control, w, true);
    NFC_INFO("client: reported career event %s", careerEventName(key));
}

void ClientSession::chat(const std::string& text) {
    if (!connected_) return;
    BitWriter w(256);
    w.str(text);
    link_.send(1, MsgType::Chat, Channel::Control, w, true);
}

void ClientSession::requestResync() {
    if (!connected_) return;
    BitWriter w(8);
    w.u8v(0);
    link_.send(1, MsgType::RepairRequest, Channel::Control, w, true);
}

std::string ClientSession::statusText() const {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s '%s' | rtt %ums | %s%s%u | %d players", connected_ ? "CLIENT" : "OFFLINE",
                  cfg_.playerName.c_str(), rttMs_, toString(w_.race.phase),
                  w_.race.phase == RacePhase::Idle ? "" : " event ", w_.race.eventId, w_.connectedCount());
    return buf;
}

}  // namespace nfc



