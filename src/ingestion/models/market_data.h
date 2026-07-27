#pragma once

#include <string>
#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>
#include <nlohmann/json.hpp>

namespace market_ingestion {

using json = nlohmann::json;
using timestamp_t = int64_t;  // microseconds since epoch

// ============================================================
// Market Data Models
// ============================================================

/// Represents a single level in the order book
struct OrderBookLevel {
    double price;
    double size;
    int64_t update_id;

    json to_json() const {
        return {{"price", price}, {"size", size}, {"update_id", update_id}};
    }

    static OrderBookLevel from_json(const json& j) {
        return {
            j.at("price").get<double>(),
            j.at("size").get<double>(),
            j.at("update_id").get<int64_t>()
        };
    }
};

/// Order book snapshot (full depth)
struct OrderBookSnapshot {
    std::string symbol;
    timestamp_t timestamp_us;
    int64_t sequence_number;
    std::vector<OrderBookLevel> bids;
    std::vector<OrderBookLevel> asks;

    json to_json() const {
        json j;
        j["symbol"] = symbol;
        j["timestamp_us"] = timestamp_us;
        j["sequence_number"] = sequence_number;
        j["bids"] = json::array();
        j["asks"] = json::array();
        for (const auto& b : bids) j["bids"].push_back(b.to_json());
        for (const auto& a : asks) j["asks"].push_back(a.to_json());
        return j;
    }
};

/// Order book delta (incremental update)
struct OrderBookDelta {
    std::string symbol;
    timestamp_t timestamp_us;
    int64_t sequence_number;
    std::vector<OrderBookLevel> bids;
    std::vector<OrderBookLevel> asks;
    bool is_snapshot{false};

    json to_json() const {
        json j;
        j["symbol"] = symbol;
        j["timestamp_us"] = timestamp_us;
        j["sequence_number"] = sequence_number;
        j["is_snapshot"] = is_snapshot;
        j["bids"] = json::array();
        j["asks"] = json::array();
        for (const auto& b : bids) j["bids"].push_back(b.to_json());
        for (const auto& a : asks) j["asks"].push_back(a.to_json());
        return j;
    }
};

/// Trade data
struct Trade {
    std::string symbol;
    timestamp_t timestamp_us;
    double price;
    double size;
    std::string trade_id;
    bool is_buyer_maker;  // false = buy, true = sell

    json to_json() const {
        return {
            {"symbol", symbol},
            {"timestamp_us", timestamp_us},
            {"price", price},
            {"size", size},
            {"trade_id", trade_id},
            {"is_buyer_maker", is_buyer_maker}
        };
    }

    static Trade from_json(const json& j) {
        return {
            j.at("symbol").get<std::string>(),
            j.at("timestamp_us").get<timestamp_t>(),
            j.at("price").get<double>(),
            j.at("size").get<double>(),
            j.at("trade_id").get<std::string>(),
            j.at("is_buyer_maker").get<bool>()
        };
    }
};

/// Ticker / 24hr stats
struct Ticker {
    std::string symbol;
    timestamp_t timestamp_us;
    double last_price;
    double volume_24h;
    double turnover_24h;
    double high_24h;
    double low_24h;
    double open_24h;
    double price_change_pct;
    std::optional<double> bid_price;
    std::optional<double> ask_price;

    json to_json() const {
        json j;
        j["symbol"] = symbol;
        j["timestamp_us"] = timestamp_us;
        j["last_price"] = last_price;
        j["volume_24h"] = volume_24h;
        j["turnover_24h"] = turnover_24h;
        j["high_24h"] = high_24h;
        j["low_24h"] = low_24h;
        j["open_24h"] = open_24h;
        j["price_change_pct"] = price_change_pct;
        if (bid_price) j["bid_price"] = *bid_price;
        if (ask_price) j["ask_price"] = *ask_price;
        return j;
    }
};

/// Kline / Candlestick data
struct Kline {
    std::string symbol;
    std::string interval;  // 1m, 5m, 15m, 30m, 1h, 4h, 1d, 1w, 1M
    timestamp_t start_time_us;
    timestamp_t end_time_us;
    double open;
    double high;
    double low;
    double close;
    double volume;
    double turnover;
    bool is_closed;

    json to_json() const {
        return {
            {"symbol", symbol},
            {"interval", interval},
            {"start_time_us", start_time_us},
            {"end_time_us", end_time_us},
            {"open", open},
            {"high", high},
            {"low", low},
            {"close", close},
            {"volume", volume},
            {"turnover", turnover},
            {"is_closed", is_closed}
        };
    }
};

/// Raw WebSocket message wrapper (for audit trail)
struct RawMessage {
    std::string topic;
    std::string channel;
    timestamp_t received_at_us;
    std::string payload;  // raw JSON string
    bool parsed_successfully{false};
    std::string parse_error;
};

/// Ingested record — unified envelope for all data types
struct IngestedRecord {
    enum class Type : uint8_t {
        UNKNOWN = 0,
        ORDER_BOOK_SNAPSHOT = 1,
        ORDER_BOOK_DELTA = 2,
        TRADE = 3,
        TICKER = 4,
        KLINE = 5
    };

    Type type{Type::UNKNOWN};
    std::string symbol;
    timestamp_t exchange_timestamp_us{0};
    timestamp_t ingestion_timestamp_us{0};
    int64_t sequence_number{0};
    std::string raw_json;
    std::string checksum;

    // Specific payloads (only one is active per record)
    std::optional<OrderBookSnapshot> ob_snapshot;
    std::optional<OrderBookDelta> ob_delta;
    std::optional<Trade> trade;
    std::optional<Ticker> ticker;
    std::optional<Kline> kline;

    json to_json() const {
        json j;
        j["type"] = static_cast<int>(type);
        j["symbol"] = symbol;
        j["exchange_timestamp_us"] = exchange_timestamp_us;
        j["ingestion_timestamp_us"] = ingestion_timestamp_us;
        j["sequence_number"] = sequence_number;
        j["checksum"] = checksum;
        if (ob_snapshot) j["ob_snapshot"] = ob_snapshot->to_json();
        if (ob_delta) j["ob_delta"] = ob_delta->to_json();
        if (trade) j["trade"] = trade->to_json();
        if (ticker) j["ticker"] = ticker->to_json();
        if (kline) j["kline"] = kline->to_json();
        return j;
    }
};

} // namespace market_ingestion