#include "link.h"

#include <enet/enet.h>

#include <cstring>

#include "log.h"

namespace nfc {
namespace {

bool ensureEnet() {
    static bool ready = false;
    if (!ready) {
        if (enet_initialize() != 0) {
            NFC_ERROR("enet_initialize failed");
            return false;
        }
        ready = true;
    }
    return true;
}

u32 peerIdOf(_ENetPeer* peer) {
    return static_cast<u32>(reinterpret_cast<std::uintptr_t>(peer->data));
}

}  // namespace

_ENetPeer* Link::findPeer(u32 id) const {
    auto it = peers_.find(id);
    return it == peers_.end() ? nullptr : it->second;
}

bool Link::startServer(u16 port, u32 maxPeers) {
    if (!ensureEnet()) return false;
    if (host_) close();
    ENetAddress address{};
    address.host = ENET_HOST_ANY;
    address.port = port;
    host_ = enet_host_create(&address, maxPeers, 2, 0, 0);
    if (!host_) {
        NFC_ERROR("enet_host_create(server) failed on port %u", port);
        return false;
    }
    isServer_ = true;
    peers_.clear();
    nextPeerId_ = 1;
    NFC_INFO("link: listening on udp/%u (max %u peers)", port, maxPeers);
    return true;
}

bool Link::startClient() {
    if (!ensureEnet()) return false;
    if (host_) close();
    host_ = enet_host_create(nullptr, 1, 2, 0, 0);
    if (!host_) {
        NFC_ERROR("enet_host_create(client) failed");
        return false;
    }
    isServer_ = false;
    peers_.clear();
    nextPeerId_ = 1;
    return true;
}

bool Link::connect(const std::string& hostName, u16 port) {
    if (!host_) return false;
    ENetAddress address{};
    address.port = port;
    if (enet_address_set_host(&address, hostName.c_str()) != 0) {
        NFC_ERROR("link: cannot resolve %s", hostName.c_str());
        return false;
    }
    ENetPeer* peer = enet_host_connect(host_, &address, 2, 0);
    if (!peer) {
        NFC_ERROR("link: enet_host_connect failed");
        return false;
    }
    peers_[1] = peer;
    NFC_INFO("link: connecting to %s:%u", hostName.c_str(), port);
    return true;
}

void Link::close() {
    if (host_) {
        for (auto& kv : peers_)
            if (kv.second) enet_peer_reset(kv.second);
        enet_host_destroy(host_);
        host_ = nullptr;
    }
    peers_.clear();
}

void Link::poll(std::vector<NetEvent>& out) {
    if (!host_) return;
    for (;;) {
        ENetEvent ev;
        const int rc = enet_host_service(host_, &ev, 0);
        if (rc <= 0) break;
        switch (ev.type) {
            case ENET_EVENT_TYPE_CONNECT: {
                u32 id;
                if (isServer_) {
                    id = nextPeerId_++;
                    peers_[id] = ev.peer;
                } else {
                    id = 1;
                    peers_[1] = ev.peer;
                }
                ev.peer->data = reinterpret_cast<void*>(static_cast<std::uintptr_t>(id));
                // 15 s of silence ends a peer; the session reports it as a leave.
                enet_peer_timeout(ev.peer, 4, 5000, kDisconnectTimeoutMs);
                NetEvent e;
                e.type = NetEvent::Type::Connected;
                e.peer = id;
                out.push_back(e);
                break;
            }
            case ENET_EVENT_TYPE_DISCONNECT: {
                const u32 id = peerIdOf(ev.peer);
                NetEvent e;
                e.type = NetEvent::Type::Disconnected;
                e.peer = id;
                out.push_back(e);
                peers_.erase(id);
                ev.peer->data = nullptr;
                break;
            }
            case ENET_EVENT_TYPE_RECEIVE: {
                const u32 id = peerIdOf(ev.peer);
                MsgType type;
                const u8* payload = nullptr;
                std::size_t plen = 0;
                if (decodePacket(ev.packet->data, ev.packet->dataLength, type, payload, plen)) {
                    NetEvent e;
                    e.type = NetEvent::Type::Received;
                    e.peer = id;
                    e.msg = type;
                    e.rttMs = ev.peer ? ev.peer->roundTripTime : 0;
                    e.payload.assign(payload, payload + plen);
                    out.push_back(e);
                } else {
                    NFC_DEBUG("link: dropped malformed packet from peer %u (%zu bytes)", id,
                              static_cast<std::size_t>(ev.packet->dataLength));
                }
                enet_packet_destroy(ev.packet);
                break;
            }
            default:
                break;
        }
    }
}

bool Link::send(u32 peer, MsgType type, Channel ch, const void* payload, std::size_t len, bool reliable) {
    if (!host_) return false;
    _ENetPeer* p = findPeer(peer);
    if (!p) return false;
    std::vector<u8> buf;
    if (!encodePacket(type, static_cast<const u8*>(payload), len, buf)) {
        NFC_WARN("link: packet %s too large (%zu bytes)", toString(type), len);
        return false;
    }
    const enet_uint32 flags = reliable ? ENET_PACKET_FLAG_RELIABLE : 0;
    ENetPacket* packet = enet_packet_create(buf.data(), buf.size(), flags);
    if (!packet) return false;
    if (enet_peer_send(p, static_cast<enet_uint8>(ch), packet) != 0) {
        enet_packet_destroy(packet);
        return false;
    }
    return true;
}

void Link::broadcast(MsgType type, Channel ch, const void* payload, std::size_t len, bool reliable, u32 except) {
    for (const auto& kv : peers_) {
        if (kv.first == except) continue;
        send(kv.first, type, ch, payload, len, reliable);
    }
}

void Link::disconnect(u32 peer, bool immediate) {
    _ENetPeer* p = findPeer(peer);
    if (!p) return;
    if (immediate)
        enet_peer_disconnect_now(p, 0);
    else
        enet_peer_disconnect(p, 0);
}

void Link::flush(u32 ms) {
    if (!host_) return;
    const u64 end = nowMs() + ms;
    while (nowMs() < end) {
        ENetEvent ev;
        while (enet_host_service(host_, &ev, 0) > 0) {
            if (ev.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(ev.packet);
        }
        sleepMs(5);
    }
}

bool Link::peerConnected(u32 peer) const { return findPeer(peer) != nullptr; }
u32 Link::peerRtt(u32 peer) const {
    _ENetPeer* p = findPeer(peer);
    return p ? p->roundTripTime : 0;
}

std::vector<u32> Link::peerIds() const {
    std::vector<u32> v;
    v.reserve(peers_.size());
    for (const auto& kv : peers_) v.push_back(kv.first);
    return v;
}

}  // namespace nfc
