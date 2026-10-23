#include "../websocket_common.h"
#include "../file_logger.h"
#include "../websocket_feed.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::vector<std::string> build_subscriptions(
    const std::vector<std::string>& symbols) {
    std::vector<std::string> subscriptions;
    for (std::size_t offset = 0; offset < symbols.size(); offset += 10) {
        nlohmann::json args = nlohmann::json::array();
        const auto end = std::min(offset + 10, symbols.size());
        for (std::size_t i = offset; i < end; ++i) {
            args.push_back("tickers." + symbols[i]);
        }
        subscriptions.push_back(
            nlohmann::json{{"op", "subscribe"}, {"args", args}}.dump());
    }
    return subscriptions;
}

bool parse_quote(const std::string& raw, QuoteUpdate& update) {
    const auto message = nlohmann::json::parse(raw);
    if (!message.contains("topic") || !message.contains("data") ||
        !message["data"].is_object()) {
        return false;
    }

    const std::string topic = message["topic"].get<std::string>();
    constexpr char prefix[] = "tickers.";
    if (topic.compare(0, sizeof(prefix) - 1, prefix) != 0) {
        return false;
    }

    const auto& data = message["data"];
    const std::string symbol = data.value("symbol", data.value("s", ""));
    const std::string bid_text = data.value("bid1Price", "");
    const std::string ask_text = data.value("ask1Price", "");
    if (symbol.empty() || bid_text.empty() || ask_text.empty()) {
        return false;
    }

    const auto bid = std::stold(bid_text);
    const auto ask = std::stold(ask_text);
    constexpr long double scale = 10'000.0L;
    if (!(bid > 0.0L) || !(ask > bid) || bid * scale > 0xFFFFFFFFu ||
        ask * scale > 0xFFFFFFFFu) {
        return false;
    }

    update.venue = Venue::Bybit;
    update.symbol = symbol;
    update.bid = static_cast<std::uint32_t>(bid * scale + 0.5L);
    update.ask = static_cast<std::uint32_t>(ask * scale + 0.5L);
    update.exchange_time_ms = message.value("ts", std::int64_t{0});
    update.received_at = std::chrono::steady_clock::now();
    return update.exchange_time_ms > 0;
}

} // namespace

void run_bybit_connector(
    const std::vector<std::string>& symbols,
    const std::function<void(QuoteUpdate)>& publish,
    const std::function<void()>& malformed,
    const std::function<bool()>& stopping) {
    const auto subscriptions = build_subscriptions(symbols);

    while (!stopping()) {
        run_reconnecting_websocket(
            "stream.bybit.com", "443", "/v5/public/linear", subscriptions,
            "BYBIT",
            [&](const std::string& raw) {
                const auto received_at = std::chrono::steady_clock::now();
                try {
                    QuoteUpdate update{};
                    if (parse_quote(raw, update)) {
                        update.received_at = received_at;
                        exchange_log("BYBIT", "quote_parsed symbol=" + update.symbol + " bid_raw=" + std::to_string(update.bid) + " ask_raw=" + std::to_string(update.ask));
                        publish(std::move(update));
                    } else {
                        exchange_log("BYBIT", "message_ignored: " + raw.substr(0, 300));
                    }
                } catch (const std::exception&) {
                    exchange_log("BYBIT", "parse_exception: " + raw.substr(0, 300));
                    malformed();
                }
            },
            stopping);
    }
}
