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
    char header[20] = {0};
#ifdef _WIN32
    int n = static_cast<int>(recv(_sock, header, sizeof(header), 0));
#else
    ssize_t n = read(_sock, header, sizeof(header));
#endif
    if (n <= 0) {
#ifdef _WIN32
        ec = std::error_code(WSAGetLastError(), std::system_category());
#else
        ec = std::error_code(errno, std::generic_category());
#endif
        return -1;
    }

    const uint32_t magic = *reinterpret_cast<const uint32_t*>(header);
    const uint32_t payload_len = *reinterpret_cast<const uint32_t*>(header + 4);
    const uint32_t msg_type = *reinterpret_cast<const uint32_t*>(header + 8);
    const uint64_t timestamp_ns = *reinterpret_cast<const uint64_t*>(header + 12);
    (void)msg_type;
    (void)timestamp_ns;

    if (magic != 0x4D4B5254) {  // "MKRT"
        ec = std::error_code(EPROTO, std::generic_category());
        return -1;
    }

    if (payload_len > 1024 * 1024) {
        ec = std::error_code(E2BIG, std::generic_category());
        return -1;
    }

    out.resize(payload_len);
    int total = 0;
    while (total < static_cast<int>(payload_len)) {
#ifdef _WIN32
        int r = static_cast<int>(recv(_sock, out.data() + total,
                                       payload_len - total, 0));
#else
        ssize_t r = read(_sock, out.data() + total, payload_len - total);
#endif
        if (r <= 0) {
#ifdef _WIN32
            ec = std::error_code(WSAGetLastError(), std::system_category());
#else
            ec = std::error_code(errno, std::generic_category());
#endif
            return -1;
        }
        total += r;
    }

    return total;
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
