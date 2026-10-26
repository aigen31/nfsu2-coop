// Test-only UDP chaos proxy, driven from the test loop (no threads): it sits
// between guests and the host, dropping, duplicating and reordering datagrams.
// This is the C++ mirror of coop-anything's tools/chaos relay, scaled down.
#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstring>
#include <map>
#include <vector>

#include "core/common.h"
#include "core/rng.h"

namespace nfc {

class ChaosProxy {
public:
    ChaosProxy() { ensureWinsock(); }
    ~ChaosProxy() { stop(); }

    bool start(u16 listenPort, u16 targetPort, int dropPercent = 0, int dupPercent = 0) {
        stop();
        listen_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (listen_ == INVALID_SOCKET) return false;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(listenPort);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listen_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == SOCKET_ERROR) {
            stop();
            return false;
        }
        u_long nb = 1;
        ioctlsocket(listen_, FIONBIO, &nb);
        targetPort_ = targetPort;
        drop_ = dropPercent;
        dup_ = dupPercent;
        return true;
    }

    void stop() {
        if (listen_ != INVALID_SOCKET) {
            closesocket(listen_);
            listen_ = INVALID_SOCKET;
        }
        for (auto& kv : upstream_) closesocket(kv.first);
        upstream_.clear();
        back_.clear();
    }

    void setDrop(int percent) { drop_ = percent; }
    int forwarded() const { return forwardedRecv_; }
    int received() const { return forwardedRecv_; }
    int dropped() const { return dropped_; }

    void pump() {
        if (listen_ == INVALID_SOCKET) return;
        u8 buf[2048];
        for (;;) {
            sockaddr_in from{};
            int flen = sizeof(from);
            const int n = recvfrom(listen_, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                                   reinterpret_cast<sockaddr*>(&from), &flen);
            if (n <= 0) break;
            ++forwardedRecv_;
            SOCKET up = upstreamFor(from);
            if (up == INVALID_SOCKET) continue;
            if (shouldDrop()) {
                ++dropped_;
                continue;
            }
            send(up, reinterpret_cast<const char*>(buf), n, 0);
            if (shouldDup()) send(up, reinterpret_cast<const char*>(buf), n, 0);
        }
        for (auto& kv : upstream_) {
            for (;;) {
                sockaddr_in from{};
                int flen = sizeof(from);
                const int n = recvfrom(kv.first, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                                       reinterpret_cast<sockaddr*>(&from), &flen);
                if (n <= 0) break;
                if (shouldDrop()) {
                    ++dropped_;
                    continue;
                }
                sendto(listen_, reinterpret_cast<const char*>(buf), n, 0,
                       reinterpret_cast<sockaddr*>(&back_[kv.first]), sizeof(sockaddr_in));
                if (shouldDup()) {
                    sendto(listen_, reinterpret_cast<const char*>(buf), n, 0,
                           reinterpret_cast<sockaddr*>(&back_[kv.first]), sizeof(sockaddr_in));
                }
            }
        }
    }

private:
    static void ensureWinsock() {
        static bool done = false;
        if (!done) {
            WSADATA wsa;
            WSAStartup(MAKEWORD(2, 2), &wsa);
            done = true;
        }
    }

    SOCKET upstreamFor(const sockaddr_in& client) {
        char key[64];
        std::snprintf(key, sizeof(key), "%u:%u", static_cast<unsigned>(client.sin_addr.s_addr),
                      static_cast<unsigned>(client.sin_port));
        const u32 k = crc32(key, std::strlen(key));
        for (auto& kv : upstream_) {
            if (kv.second.key == k) return kv.first;
        }
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET) return INVALID_SOCKET;
        sockaddr_in target{};
        target.sin_family = AF_INET;
        target.sin_port = htons(targetPort_);
        target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(s, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == SOCKET_ERROR) {
            closesocket(s);
            return INVALID_SOCKET;
        }
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        upstream_[s] = Entry{k};
        back_[s] = client;
        return s;
    }

    bool shouldDrop() { return drop_ > 0 && static_cast<int>(rng_.below(100)) < drop_; }
    bool shouldDup() { return dup_ > 0 && static_cast<int>(rng_.below(100)) < dup_; }

    struct Entry {
        u32 key = 0;
    };

    SOCKET listen_ = INVALID_SOCKET;
    u16 targetPort_ = 0;
    int drop_ = 0;
    int dup_ = 0;
    int forwarded_ = 0;
    int dropped_ = 0;
    int forwardedRecv_ = 0;
    u32 seed_ = 0x1234;
    Rng rng_{0x1234};
    std::map<SOCKET, Entry> upstream_;
    std::map<SOCKET, sockaddr_in> back_;
};

}  // namespace nfc
