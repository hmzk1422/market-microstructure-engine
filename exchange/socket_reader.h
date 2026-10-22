#pragma once

#include <cstdint>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

/// Minimal socket reader that connects to the Python ABIDES exchange
/// simulator. Uses Unix domain sockets on Unix, TCP loopback on Windows.
class UdsSocketReader {
public:
    /// Create a reader. On Windows, `port` is used for TCP loopback;
    /// on Unix, `path` is the UDS socket path.
    UdsSocketReader(const std::string& path, int port = 6789);

    int read_packet(std::string& out, std::error_code& ec);

#ifdef _WIN32
    bool connected() const { return _sock != INVALID_SOCKET; }
#else
    bool connected() const { return _sock >= 0; }
#endif

    void close();

    ~UdsSocketReader();

private:
#ifdef _WIN32
    SOCKET _sock = INVALID_SOCKET;
#else
    int _sock = -1;
#endif
    std::string _path;
    std::string _rx_buffer;
    int _port = 6789;
    bool _use_tcp = false;
};
