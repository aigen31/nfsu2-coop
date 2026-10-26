// ENet wrapper: one host, up to 8 peers, two channels (control = reliable
// ordered, state = unreliable sequenced). ENet owns retransmit, fragmentation
// and RTT; this class only turns its events into plain C++ events.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "common.h"
#include "protocol.h"

struct _ENetHost;
struct _ENetPeer;

namespace nfc {

struct NetEvent {
    enum class Type { Connected, Disconnected, Received } type = Type::Received;
    u32 peer = 0;
    u32 rttMs = 0;
    MsgType msg = MsgType::Hello;
    std::vector<u8> payload;
};

class Link {
public:
    Link() = default;
    ~Link() { close(); }
    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;

    bool startServer(u16 port, u32 maxPeers = kMaxPlayers);
    bool startClient();
    bool connect(const std::string& hostName, u16 port);
    void close();
    bool running() const { return host_ != nullptr; }
    bool isServer() const { return isServer_; }

    // Drains every queued event. Non-blocking.
    void poll(std::vector<NetEvent>& out);

    bool send(u32 peer, MsgType type, Channel ch, const void* payload, std::size_t len, bool reliable);
    void broadcast(MsgType type, Channel ch, const void* payload, std::size_t len, bool reliable,
                   u32 except = 0xFFFFFFFFu);

    void disconnect(u32 peer, bool immediate = false);
    // Pumps the host for `ms` so queued disconnects and packets actually leave.
    void flush(u32 ms);

    bool peerConnected(u32 peer) const;
    u32 peerRtt(u32 peer) const;
    std::vector<u32> peerIds() const;
    std::size_t peerCount() const { return peers_.size(); }

    // Helpers that serialize a BitWriter.
    bool send(u32 peer, MsgType type, Channel ch, const BitWriter& w, bool reliable) {
        return send(peer, type, ch, w.data(), w.size(), reliable);
    }
    void broadcast(MsgType type, Channel ch, const BitWriter& w, bool reliable, u32 except = 0xFFFFFFFFu) {
        broadcast(type, ch, w.data(), w.size(), reliable, except);
    }

private:
    _ENetHost* host_ = nullptr;
    bool isServer_ = false;
    std::map<u32, _ENetPeer*> peers_;
    u32 nextPeerId_ = 1;
    _ENetPeer* findPeer(u32 id) const;
};

}  // namespace nfc
