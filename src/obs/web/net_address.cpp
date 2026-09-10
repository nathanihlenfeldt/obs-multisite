// SPDX-License-Identifier: GPL-3.0-or-later
#include "net_address.h"

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

#include <cstring>

namespace multisite_obs {

namespace {

#ifdef _WIN32
using sock_t = SOCKET;
constexpr sock_t kBadSocket = INVALID_SOCKET;
void close_socket(sock_t s) { ::closesocket(s); }
#else
using sock_t = int;
constexpr sock_t kBadSocket = -1;
void close_socket(sock_t s) { ::close(s); }
#endif

} // namespace

std::string local_ipv4() {
#ifdef _WIN32
    // Winsock is already up: the remote-control server starts it, and this is
    // only ever asked for while that server exists.
#endif

    // A UDP socket "connected" to a public address sends nothing — connect() on
    // a datagram socket only asks the kernel which interface it would use. That
    // is the question worth asking: an address on a disconnected VPN or a
    // docked ethernet port with no cable is not one anybody can reach.
    const sock_t s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s == kBadSocket) return "";

    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port   = htons(53);
    ::inet_pton(AF_INET, "8.8.8.8", &peer.sin_addr);

    std::string out;
    if (::connect(s, (sockaddr*)&peer, sizeof(peer)) == 0) {
        sockaddr_in self{};
        socklen_t len = sizeof(self);
        if (::getsockname(s, (sockaddr*)&self, &len) == 0) {
            char buf[INET_ADDRSTRLEN] = {0};
            if (::inet_ntop(AF_INET, &self.sin_addr, buf, sizeof(buf)))
                out = buf;
        }
    }
    close_socket(s);

    // 0.0.0.0 means the kernel had nothing to choose, which is the same answer
    // as having no address at all.
    if (out == "0.0.0.0") return "";
    return out;
}

} // namespace multisite_obs