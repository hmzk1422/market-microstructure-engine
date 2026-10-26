#include "../websocket_common.h"
#include "../websocket_feed.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::vector<std::string> build_subscriptions(
    const std::vector<std::string>& symbols) {
    std::vector<std::string> subscriptions;
    for (std::size_t offset = 0; offset < symbols.size(); offset += 100) {
        nlohmann::json params = nlohmann::json::array();
        const auto end = std::min(offset + 100, symbols.size());
        for (std::size_t i = offset; i < end; ++i) {
            std::string symbol = symbols[i];
            std::transform(symbol.begin(), symbol.end(), symbol.begin(),
                           [](unsigned char c) {
                               return static_cast<char>(std::tolower(c));
                           });
            params.push_back(symbol + "@bookTicker");
        }
        subscriptions.push_back(nlohmann::json{
            {"method", "SUBSCRIBE"}, {"params", params},
            {"id", static_cast<std::uint32_t>(subscriptions.size() + 1)}
        }.dump());
    }
    return subscriptions;
}

bool parse_quote(const std::string& raw, QuoteUpdate& update) {
    const auto message = nlohmann::json::parse(raw);
    // The /ws endpoint returns raw stream events. The /stream endpoint wraps
    // them in {"stream":"...","data":{...}}, so accept both shapes.
    const auto& data = message.contains("data") && message["data"].is_object()
                           ? message["data"]
                           : message;
    if (!data.contains("s") || !data.contains("b") || !data.contains("a")) {
        return false;
    }

    const auto as_number = [](const nlohmann::json& value) {
        return std::stold(value.is_string() ? value.get<std::string>()
                                            : value.dump());
    };
    const auto bid = as_number(data["b"]);
    const auto ask = as_number(data["a"]);
    constexpr long double scale = 10'000.0L;
    if (!(bid > 0.0L) || !(ask > bid) || bid * scale > 0xFFFFFFFFu ||
        ask * scale > 0xFFFFFFFFu) {
        return false;
    }

    update.venue = Venue::Binance;
    const auto symbol = data["s"].get<std::string>();
    if (symbol.empty() || symbol.size() >= update.symbol.size()) return false;
    std::memcpy(update.symbol.data(), symbol.data(), symbol.size());
    update.bid = static_cast<std::uint32_t>(bid * scale + 0.5L);
    update.ask = static_cast<std::uint32_t>(ask * scale + 0.5L);
    update.exchange_time_ms = data.value(
        "E", data.value(
                "T", std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count()));
    update.received_at = std::chrono::steady_clock::now();
    return true;
}

} // namespace

void run_binance_connector(
    const std::vector<std::string>& symbols,
    const std::function<void(QuoteUpdate)>& publish,
    const std::function<void()>& malformed,
    const std::function<bool()>& stopping) {
    const auto subscriptions = build_subscriptions(symbols);

    while (!stopping()) {
        run_reconnecting_websocket(
            "stream.binance.com", "9443", "/ws", subscriptions, "BINANCE",
            [&](const std::string& raw) {
                const auto received_at = std::chrono::steady_clock::now();
                try {
                    QuoteUpdate update{};
                    if (parse_quote(raw, update)) {
                        update.received_at = received_at;
                        publish(std::move(update));
                    }
                } catch (const std::exception&) {
                    malformed();
                }
            },
            stopping);
    }
}
