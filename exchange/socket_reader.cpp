#include "socket_reader.h"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>

#include "config.h"

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/un.h>
#endif

UdsSocketReader::UdsSocketReader(const std::string& path, int port)
    : _path(path), _port(port) {
#ifdef _WIN32
    WSAData wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "[UDS] WSAStartup failed: " << WSAGetLastError() << "\n";
        return;
    }
    _sock = socket(AF_INET, SOCK_STREAM, 0);
    _use_tcp = true;
#else
    _sock = socket(AF_UNIX, SOCK_STREAM, 0);
#endif
    if (_sock < 0) {
        std::cerr << "[UDS] Failed to create socket: " << strerror(errno) << "\n";
        return;
    }

    // Set a read timeout so we don't block forever.
#ifdef _WIN32
    DWORD timeout = (DWORD)(Config{}.read_timeout_sec * 1000);
    setsockopt(_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(_port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "[UDS] connect to 127.0.0.1:" << _port
                  << " failed: " << WSAGetLastError() << "\n";
        close();
        return;
    }
#else
    struct timeval tv;
    tv.tv_sec = (long)Config{}.read_timeout_sec;
    tv.tv_usec = 0;
    setsockopt(_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (_path.size() >= sizeof(addr.sun_path)) {
        std::cerr << "[UDS] Socket path too long: " << _path << "\n";
        close();
        return;
    }
    strncpy(addr.sun_path, _path.c_str(), sizeof(addr.sun_path) - 1);

    socklen_t addr_len = sizeof(addr);
    if (connect(_sock, reinterpret_cast<struct sockaddr*>(&addr), addr_len) < 0) {
        std::cerr << "[UDS] connect to " << _path << " failed: " << strerror(errno) << "\n";
        close();
        return;
    }
#endif

    std::cout << "[UDS] Connected to exchange: " << _path << "\n";
}

int UdsSocketReader::read_packet(std::string& out, std::error_code& ec) {
    constexpr std::size_t header_size = 20;
    constexpr std::uint32_t max_payload_size = 1024 * 1024;
    char chunk[8192];
    ec.clear();

    const auto receive_more = [&]() -> bool {
#ifdef _WIN32
        const int received = recv(_sock, chunk, sizeof(chunk), 0);
#else
        const ssize_t received = read(_sock, chunk, sizeof(chunk));
#endif
        if (received <= 0) {
#ifdef _WIN32
            ec = std::error_code(WSAGetLastError(), std::system_category());
#else
            ec = std::error_code(errno, std::generic_category());
#endif
            return false;
        }
        _rx_buffer.append(chunk, static_cast<std::size_t>(received));
        return true;
    };

    while (_rx_buffer.size() < header_size) {
        if (!receive_more()) {
            return -1;
        }
    }

    std::uint32_t magic = 0;
    std::uint32_t payload_len = 0;
    std::memcpy(&magic, _rx_buffer.data(), sizeof(magic));
    std::memcpy(&payload_len, _rx_buffer.data() + 4, sizeof(payload_len));

    if (magic != 0x4D4B5254) {  // "MKRT"
        ec = std::error_code(EPROTO, std::generic_category());
        _rx_buffer.clear();
        return -1;
    }

    if (payload_len > max_payload_size) {
        ec = std::error_code(E2BIG, std::generic_category());
        _rx_buffer.clear();
        return -1;
    }

    const std::size_t packet_size = header_size + payload_len;
    while (_rx_buffer.size() < packet_size) {
        if (!receive_more()) {
            return -1;
        }
    }

    out.assign(_rx_buffer.data() + header_size, payload_len);
    _rx_buffer.erase(0, packet_size);
    return static_cast<int>(payload_len);
}

void UdsSocketReader::close() {
#ifdef _WIN32
    if (_sock != INVALID_SOCKET) {
        closesocket(_sock);
        _sock = INVALID_SOCKET;
    }
    WSACleanup();
#else
    if (_sock >= 0) {
        ::close(_sock);
        _sock = -1;
    }
#endif
}

UdsSocketReader::~UdsSocketReader() {
    close();
}
