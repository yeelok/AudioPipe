#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace audiopipe {
namespace win {

// Minimal unicast UDP sender (spec §7). send() is best-effort and never
// throws/blocks fatally: a briefly-down network just drops datagrams (§5.7).
// Call WSAStartup once before open() (the app does this).
class UdpSender {
public:
    ~UdpSender();

    bool open(const std::string& ip, std::uint16_t port, std::string& err);
    void send(const std::uint8_t* data, std::size_t len);
    void close();

private:
    std::uintptr_t sock_ = ~std::uintptr_t(0); // INVALID_SOCKET
    std::uint32_t  addr_ = 0;                   // dest, network byte order
    std::uint16_t  port_ = 0;                   // dest, network byte order
};

} // namespace win
} // namespace audiopipe
