#pragma once

#include <string>
#include <memory>
#include <libpq-fe.h>
#include <nlohmann/json.hpp>

namespace market_ingestion {

using json = nlohmann::json;

/// Simple TimescaleDB/PostgreSQL writer for market data
class TimescaleWriter {
public:
    TimescaleWriter(const std::string& conn_str);
    ~TimescaleWriter();

    bool connected() const { return conn_ != nullptr; }

    /// Initialize schema (create hypertables if not exist)
    bool init_schema();

    /// Insert an order book snapshot or delta
    bool insert_orderbook(const json& msg);

    /// Insert a public trade (aggressive order)
    bool insert_trade(const json& msg);

private:
    PGconn* conn_{nullptr};

    bool exec(const std::string& sql);
};

} // namespace market_ingestion