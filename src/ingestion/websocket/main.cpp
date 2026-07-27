/**
 * @file bybit_arbitrage_client.cpp
 * @brief Synchronous WebSocket client for ingesting Bybit market data.
 * 
 * Dependencies:
 * - Boost.Asio: Manages underlying transport layers, DNS resolution, and TCP sockets.
 * - Boost.Beast: Implements HTTP/1 and WebSocket protocols for live data streaming.
 * - libpq (PostgreSQL): Writes parsed market data to TimescaleDB.
 */

#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "../storage/timescale_writer.h"

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;
using json = nlohmann::json;

/// Parse and dispatch a WebSocket message to the appropriate TimescaleDB writer method.
void process_message(const std::string& raw, market_ingestion::TimescaleWriter& db) {
    try {
        auto j = json::parse(raw);

        // Handle subscription response ack
        if (j.contains("op") && j["op"] == "subscribe") {
            return;
        }

        // Handle pong / heartbeat
        if (j.contains("type") && j["type"] == "pong") return;
        if (j.contains("ret_msg") && j["ret_msg"] == "pong") return;

        // Route by topic
        if (j.contains("topic")) {
            std::string topic = j["topic"];

            if (topic.rfind("orderbook.", 0) == 0) {
                db.insert_orderbook(j);
            } else if (topic.rfind("publicTrade.", 0) == 0) {
                db.insert_trade(j);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Failed to parse/insert message: " << e.what() << std::endl;
    }
}

/// Build the JSON subscription payload for both orderbook and public trade streams.
std::string build_subscription(const std::vector<std::string>& symbols, int depth) {
    std::string msg = "{\"op\": \"subscribe\", \"args\": [";
    for (size_t i = 0; i < symbols.size(); ++i) {
        msg += "\"orderbook." + std::to_string(depth) + "." + symbols[i] + "\"";
        msg += ", ";
        msg += "\"publicTrade." + symbols[i] + "\"";
        if (i < symbols.size() - 1) {
            msg += ", ";
        }
    }
    msg += "]}";
    return msg;
}

int main() {
    try {
        // Target Bybit V5 Public Linear API endpoint.
        const std::string host = "stream.bybit.com";
        const std::string port = "443";
        const std::string target = "/v5/public/linear";

        // Define highly liquid instruments for cross-margin or funding-rate arbitrage.
        // BTCUSDT  = USDT-Margined Perpetual
        // BTC-PERP = USDC-Margined Perpetual
        const std::vector<std::string> symbols = {"BTCUSDT", "BTCPERP"};
        const int depth = 50;

        // Connect to TimescaleDB.
        const char* conn_str = std::getenv("TIMESCALE_CONN");
        if (!conn_str) {
            conn_str = "host=timescaledb port=5432 dbname=market_data user=market password=market";
        }
        market_ingestion::TimescaleWriter db(conn_str);
        if (!db.connected()) {
            std::cerr << "[WARN] TimescaleDB not available – running without database persistence." << std::endl;
        } else {
            db.init_schema();
        }

        // Construct the JSON subscription payload.
        std::string sub_message = build_subscription(symbols, depth);

        // Initialize the core I/O context for network operations.
        net::io_context ioc;

        // Configure the SSL context utilizing TLS v1.2 as the required baseline protocol.
        ssl::context ctx{ssl::context::tlsv12_client};
        
        // TODO(Production): Enforce strict SSL peer verification to prevent MITM attacks.
        // ctx.set_default_verify_paths();
        // ctx.set_verify_mode(ssl::verify_peer);

        // Instantiate the TCP resolver and secure WebSocket stream.
        tcp::resolver resolver{ioc};
        websocket::stream<beast::ssl_stream<tcp::socket>> ws{ioc, ctx};

        // Resolve the host domain to an IP address.
        auto const results = resolver.resolve(host, port);

        // Establish the underlying TCP connection to the resolved endpoint.
        auto ep = net::connect(get_lowest_layer(ws), results);

        // Configure Server Name Indication (SNI) on the SSL stream to ensure correct TLS routing.
        if(! SSL_set_tlsext_host_name(ws.next_layer().native_handle(), host.c_str())) {
            throw beast::system_error(
                beast::error_code(
                    static_cast<int>(::ERR_get_error()),
                    net::error::get_ssl_category()),
                "Failed to configure SNI Hostname");
        }

        // Execute the TLS handshake.
        ws.next_layer().handshake(ssl::stream_base::client);

        // Apply an HTTP decorator to set a custom User-Agent header for the WebSocket upgrade request.
        ws.set_option(websocket::stream_base::decorator(
            [](websocket::request_type& req) {
                req.set(http::field::user_agent, std::string(BOOST_BEAST_VERSION_STRING) + " bybit-arb-client");
            }));

        // Execute the WebSocket protocol upgrade handshake.
        ws.handshake(host, target);
        std::cout << "[INFO] Successfully established secure WebSocket connection to Bybit." << std::endl;

        // Transmit the subscription payload to the exchange.
        ws.write(net::buffer(sub_message));
        std::cout << "[INFO] Transmitted subscription payload: " << sub_message << std::endl;

        // Enter the synchronous event loop to process incoming WebSocket messages.
        while (true) {
            beast::flat_buffer buffer;
            
            // Block until a complete message is read into the buffer.
            ws.read(buffer);
            
            std::string raw = beast::buffers_to_string(buffer.data());

            // Log raw message to stdout (unchanged from original).
            std::cout << raw << std::endl;

            // Persist to TimescaleDB if connected.
            if (db.connected()) {
                process_message(raw, db);
            }
        }
    } catch(std::exception const& e) {
        std::cerr << "[FATAL] Exception caught: " << e.what() << "\n";
        return EXIT_FAILURE; 
    }
    
    return EXIT_SUCCESS;
}