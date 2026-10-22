#include "websocket_common.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>

namespace beast = boost::beast;
namespace net = boost::asio;
namespace ssl = net::ssl;
namespace websocket = beast::websocket;
using tcp = net::ip::tcp;

namespace {

bool pause_or_stop(const std::function<bool()>& stopping,
                   std::chrono::milliseconds duration) {
    constexpr auto interval = std::chrono::milliseconds(100);
    while (!stopping() && duration > std::chrono::milliseconds::zero()) {
        const auto sleep_for = std::min(interval, duration);
        std::this_thread::sleep_for(sleep_for);
        duration -= sleep_for;
    }
    return stopping();
}

void run_session(
    const std::string& host,
    const std::string& port,
    const std::string& target,
    const std::vector<std::string>& subscriptions,
    const std::string& venue,
    const std::function<void(const std::string&)>& on_message,
    const std::function<bool()>& stopping) {
    net::io_context ioc;
    ssl::context tls_context{ssl::context::tls_client};
    tls_context.set_default_verify_paths();

    tcp::resolver resolver{ioc};
    websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws{ioc,
                                                               tls_context};
    const auto endpoints = resolver.resolve(host, port);

    if (!SSL_set_tlsext_host_name(ws.next_layer().native_handle(),
                                  host.c_str())) {
        throw beast::system_error(
            beast::error_code(static_cast<int>(ERR_get_error()),
                              net::error::get_ssl_category()),
            "setting TLS SNI hostname");
    }
    ws.next_layer().set_verify_mode(ssl::verify_peer);
    ws.next_layer().set_verify_callback(ssl::host_name_verification(host));

    beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(15));
    beast::get_lowest_layer(ws).connect(endpoints);
    beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(15));
    ws.next_layer().handshake(ssl::stream_base::client);
    beast::get_lowest_layer(ws).expires_never();

    ws.set_option(websocket::stream_base::timeout::suggested(
        beast::role_type::client));
    ws.handshake(host, target);
    std::cout << "[SOURCE] " << venue << " websocket connected\n";

    for (const auto& subscription : subscriptions) {
        if (stopping()) {
            break;
        }
        ws.write(net::buffer(subscription));
        if (pause_or_stop(stopping, std::chrono::milliseconds(250))) {
            break;
        }
    }
    if (stopping()) {
        boost::system::error_code ignored;
        ws.close(websocket::close_code::normal, ignored);
        return;
    }

    while (!stopping()) {
        beast::flat_buffer buffer;
        ws.read(buffer);
        on_message(beast::buffers_to_string(buffer.data()));
    }

    boost::system::error_code ignored;
    ws.close(websocket::close_code::normal, ignored);
}

} // namespace

void run_reconnecting_websocket(
    const std::string& host,
    const std::string& port,
    const std::string& target,
    const std::vector<std::string>& subscriptions,
    const std::string& venue,
    const std::function<void(const std::string&)>& on_message,
    const std::function<bool()>& stopping) {
    auto retry_delay = std::chrono::milliseconds(500);
    constexpr auto max_retry_delay = std::chrono::seconds(30);
    while (!stopping()) {
        try {
            run_session(host, port, target, subscriptions, venue, on_message,
                        stopping);
            retry_delay = std::chrono::milliseconds(500);
        } catch (const std::exception& error) {
            if (!stopping()) {
                std::cerr << "[SOURCE] " << venue << " websocket: "
                          << error.what() << "; reconnecting in "
                          << retry_delay.count() << "ms\n";
            }
        }

        if (pause_or_stop(stopping, retry_delay)) {
            break;
        }
        retry_delay = std::min(retry_delay * 2,
                               std::chrono::duration_cast<
                                   std::chrono::milliseconds>(max_retry_delay));
    }
}
