#include "timescale_writer.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <cstdint>   // ADD THIS for int64_t
#include <stdexcept> // ADD THIS for std::exception

namespace market_ingestion {

TimescaleWriter::TimescaleWriter(const std::string& conn_str) {
    conn_ = PQconnectdb(conn_str.c_str());
    if (PQstatus(conn_) != CONNECTION_OK) {
        std::cerr << "[FATAL] TimescaleDB connection failed: " << PQerrorMessage(conn_) << std::endl;
        PQfinish(conn_);
        conn_ = nullptr;
    } else {
        std::cout << "[INFO] Connected to TimescaleDB successfully." << std::endl;
    }
}

TimescaleWriter::~TimescaleWriter() {
    if (conn_) {
        PQfinish(conn_);
        conn_ = nullptr;
    }
}

bool TimescaleWriter::exec(const std::string& sql) {
    if (!conn_) return false;
    PGresult* res = PQexec(conn_, sql.c_str());
    ExecStatusType status = PQresultStatus(res);
    bool ok = (status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK);
    if (!ok) {
        std::cerr << "[ERROR] SQL exec failed: " << PQerrorMessage(conn_) << std::endl;
        std::cerr << "[ERROR] SQL: " << sql.substr(0, 256) << "..." << std::endl;
    }
    PQclear(res);
    return ok;
}

bool TimescaleWriter::init_schema() {
    // Create orderbook hypertable
    bool ok = exec(R"(
        CREATE TABLE IF NOT EXISTS orderbook (
            time        TIMESTAMPTZ       NOT NULL,
            symbol      TEXT              NOT NULL,
            side        TEXT              NOT NULL,  -- 'bid' or 'ask'
            price       DOUBLE PRECISION  NOT NULL,
            size        DOUBLE PRECISION  NOT NULL,
            update_id   BIGINT            NOT NULL,
            is_snapshot BOOLEAN           NOT NULL DEFAULT true,
            seq         BIGINT            NOT NULL DEFAULT 0
        );
    )");
    if (!ok) return false;

    ok = exec(R"(
        SELECT create_hypertable('orderbook', 'time', if_not_exists => TRUE);
    )");
    if (!ok) return false;

    // Create trades hypertable
    ok = exec(R"(
        CREATE TABLE IF NOT EXISTS trades (
            time          TIMESTAMPTZ       NOT NULL,
            symbol        TEXT              NOT NULL,
            price         DOUBLE PRECISION  NOT NULL,
            size          DOUBLE PRECISION  NOT NULL,
            trade_id      TEXT              NOT NULL,
            is_buyer_maker BOOLEAN          NOT NULL,
            side          TEXT              NOT NULL
        );
    )");
    if (!ok) return false;

    ok = exec(R"(
        SELECT create_hypertable('trades', 'time', if_not_exists => TRUE);
    )");
    if (!ok) return false;

    // Create indexes for common query patterns
    exec(R"(CREATE INDEX IF NOT EXISTS idx_orderbook_symbol_time ON orderbook (symbol, time DESC);)");
    exec(R"(CREATE INDEX IF NOT EXISTS idx_trades_symbol_time ON trades (symbol, time DESC);)");

    std::cout << "[INFO] TimescaleDB schema initialized." << std::endl;
    return true;
}

bool TimescaleWriter::insert_orderbook(const json& msg) {
    // Bybit orderbook format (delta or snapshot):
    // {
    //   "topic": "orderbook.50.BTCUSDT",
    //   "type": "snapshot" or "delta",
    //   "ts": 123456789,
    //   "data": {
    //     "s": "BTCUSDT",
    //     "b": [["price","size","update_id"], ...],
    //     "a": [["price","size","update_id"], ...],
    //     "u": 123,
    //     "seq": 456
    //   }
    // }

    try {
        auto data = msg["data"];
        std::string symbol = data["s"].get<std::string>();
        int64_t ts = msg["ts"].get<int64_t>();
        int64_t seq = data.value("seq", 0LL);
        int64_t update_id = data.value("u", 0LL);
        bool is_snapshot = (msg["type"].get<std::string>() == "snapshot");

        // Format timestamp (Bybit gives ms, convert to TIMESTAMPTZ string)
        // Bybit ts is in milliseconds
        auto time_s = std::to_string(ts / 1000) + "." + std::to_string(ts % 1000);

        // Insert bids
        for (const auto& level : data["b"]) {
            double price = level[0].get<double>();
            double size  = level[1].get<double>();
            int64_t uid  = level[2].get<int64_t>();

            std::ostringstream sql;
            sql << "INSERT INTO orderbook (time, symbol, side, price, size, update_id, is_snapshot, seq) VALUES "
                << "(to_timestamp(" << time_s << "), "
                << "'" << symbol << "', "
                << "'bid', "
                << price << ", "
                << size << ", "
                << uid << ", "
                << (is_snapshot ? "true" : "false") << ", "
                << seq << ");";
            if (!exec(sql.str())) return false;
        }

        // Insert asks
        for (const auto& level : data["a"]) {
            double price = level[0].get<double>();
            double size  = level[1].get<double>();
            int64_t uid  = level[2].get<int64_t>();

            std::ostringstream sql;
            sql << "INSERT INTO orderbook (time, symbol, side, price, size, update_id, is_snapshot, seq) VALUES "
                << "(to_timestamp(" << time_s << "), "
                << "'" << symbol << "', "
                << "'ask', "
                << price << ", "
                << size << ", "
                << uid << ", "
                << (is_snapshot ? "true" : "false") << ", "
                << seq << ");";
            if (!exec(sql.str())) return false;
        }

        return true;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Failed to parse orderbook message: " << e.what() << std::endl;
        return false;
    }
}

bool TimescaleWriter::insert_trade(const json& msg) {
    // Bybit public trade format:
    // {
    //   "topic": "publicTrade.BTCUSDT",
    //   "ts": 123456789,
    //   "data": [{
    //     "T": 123456789,       // trade time (ms)
    //     "s": "BTCUSDT",
    //     "S": "Buy" or "Sell", // side
    //     "v": "0.001",         // size
    //     "p": "50000",         // price
    //     "i": "trade_id_xxx",  // trade id
    //     "L": "0.001",         // block trade
    //     "BT": false           // is buyer maker
    //   }]
    // }

    try {
        const auto& data_arr = msg["data"];
        for (const auto& trade : data_arr) {
            int64_t trade_ts = trade["T"].get<int64_t>();
            std::string symbol = trade["s"].get<std::string>();
            std::string side = trade["S"].get<std::string>();  // "Buy" or "Sell"
            double price = std::stod(trade["p"].get<std::string>());
            double size  = std::stod(trade["v"].get<std::string>());
            std::string trade_id = trade["i"].get<std::string>();
            bool is_buyer_maker = trade.value("BT", false);

            auto time_s = std::to_string(trade_ts / 1000) + "." + std::to_string(trade_ts % 1000);

            std::ostringstream sql;
            sql << "INSERT INTO trades (time, symbol, price, size, trade_id, is_buyer_maker, side) VALUES "
                << "(to_timestamp(" << time_s << "), "
                << "'" << symbol << "', "
                << price << ", "
                << size << ", "
                << "'" << trade_id << "', "
                << (is_buyer_maker ? "true" : "false") << ", "
                << "'" << side << "');";
            if (!exec(sql.str())) return false;
        }
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Failed to parse trade message: " << e.what() << std::endl;
        return false;
    }
}

} // namespace market_ingestion