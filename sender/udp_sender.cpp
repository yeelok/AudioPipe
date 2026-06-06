// Winsock UDP sender. Built on Windows only.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include "udp_sender.h"

namespace audiopipe {
namespace win {

UdpSender::~UdpSender() { close(); }

bool UdpSender::open(const std::string& ip, std::uint16_t port, std::string& err) {
    const SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        err = "socket() failed";
        return false;
    }
    in_addr a{};
    if (::InetPtonA(AF_INET, ip.c_str(), &a) != 1) {
        ::closesocket(s);
        err = "invalid dest_ip: " + ip;
        return false;
    }
    addr_ = a.s_addr;
    port_ = ::htons(port);
    sock_ = static_cast<std::uintptr_t>(s);
    return true;
}

void UdpSender::send(const std::uint8_t* data, std::size_t len) {
    if (sock_ == ~std::uintptr_t(0)) {
        return;
    }
    sockaddr_in dst{};
    dst.sin_family      = AF_INET;
    dst.sin_addr.s_addr = addr_;
    dst.sin_port        = port_;
    // Best-effort: ignore errors so a transient network outage never crashes
    // or stalls the sender (spec §5.7).
    ::sendto(static_cast<SOCKET>(sock_), reinterpret_cast<const char*>(data),
             static_cast<int>(len), 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
}

void UdpSender::close() {
    if (sock_ != ~std::uintptr_t(0)) {
        ::closesocket(static_cast<SOCKET>(sock_));
        sock_ = ~std::uintptr_t(0);
    }
}

} // namespace win
} // namespace audiopipe
