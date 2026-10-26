#include "timescale_telemetry.h"

#include <libpq-fe.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sys/resource.h>
#include <sched.h>
#endif

namespace metrics {
namespace {

std::string number(double value) {
    if (!std::isfinite(value)) return "NULL";
    std::ostringstream output;
    output.precision(12);
    output << value;
    return output.str();
}

bool execute(PGconn* connection, const std::string& sql) {
    PGresult* result = PQexec(connection, sql.c_str());
    if (!result) {
        std::cerr << "[TELEMETRY] " << PQerrorMessage(connection);
        return false;
    }
    const auto status = PQresultStatus(result);
    const bool ok = status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK;
    if (!ok) std::cerr << "[TELEMETRY] " << PQerrorMessage(connection);
    PQclear(result);
    return ok;
}

} // namespace

TimescaleTelemetry::TimescaleTelemetry(std::string connection_string,
                                       std::size_t capacity)
    : connection_string_(std::move(connection_string)),
      capacity_(std::max<std::size_t>(capacity, 2)),
      slots_(std::make_unique<Event[]>(capacity_)),
      decision_slots_(std::make_unique<Event[]>(capacity_)),
      metric_slots_(std::make_unique<Event[]>(capacity_)),
      thread_(&TimescaleTelemetry::run, this) {}

TimescaleTelemetry::~TimescaleTelemetry() {
    stop();
}

void TimescaleTelemetry::stop() {
    if (!thread_.joinable()) return;
    stopping_.store(true, std::memory_order_release);
    wake_sequence_.fetch_add(1, std::memory_order_release);
    wake_sequence_.notify_all();
    thread_.join();
}

bool TimescaleTelemetry::publish(const Event& event) {
    const auto write = write_.load(std::memory_order_relaxed);
    const auto next = (write + 1) % capacity_;
    if (next == read_.load(std::memory_order_acquire)) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    slots_[write] = event;
    write_.store(next, std::memory_order_release);
    wake_sequence_.fetch_add(1, std::memory_order_release);
    wake_sequence_.notify_one();
    return true;
}

bool TimescaleTelemetry::pop(Event& event) {
    const auto read = read_.load(std::memory_order_relaxed);
    if (read == write_.load(std::memory_order_acquire)) return false;
    event = slots_[read];
    read_.store((read + 1) % capacity_, std::memory_order_release);
    return true;
}

bool TimescaleTelemetry::publish_decision(
    const char* symbol, std::int64_t timestamp_ms, bool rtl_candidate,
    bool accepted, std::uint8_t direction, std::uint16_t history_samples,
    std::uint16_t bybit_fee_bps, std::uint16_t binance_fee_bps,
    std::uint16_t paper_slippage_bps, double zscore, double net_profit) {
    Event event;
    event.type = 3;
    event.timestamp_ms = timestamp_ms;
    event.rtl_candidate = rtl_candidate;
    event.accepted = accepted;
    event.direction = direction;
    event.history_samples = history_samples;
    event.bybit_fee_bps = bybit_fee_bps;
    event.binance_fee_bps = binance_fee_bps;
    event.paper_slippage_bps = paper_slippage_bps;
    event.zscore = zscore;
    event.net_profit = net_profit;
    std::strncpy(event.symbol, symbol, sizeof(event.symbol) - 1);
    const auto write = decision_write_.load(std::memory_order_relaxed);
    const auto next = (write + 1) % capacity_;
    if (next == decision_read_.load(std::memory_order_acquire)) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    decision_slots_[write] = event;
    decision_write_.store(next, std::memory_order_release);
    wake_sequence_.fetch_add(1, std::memory_order_release);
    wake_sequence_.notify_one();
    return true;
}

bool TimescaleTelemetry::pop_decision(Event& event) {
    const auto read = decision_read_.load(std::memory_order_relaxed);
    if (read == decision_write_.load(std::memory_order_acquire)) return false;
    event = decision_slots_[read];
    decision_read_.store((read + 1) % capacity_, std::memory_order_release);
    return true;
}

bool TimescaleTelemetry::publish_metric_event(const Event& event) {
    const auto write = metric_write_.load(std::memory_order_relaxed);
    const auto next = (write + 1) % capacity_;
    if (next == metric_read_.load(std::memory_order_acquire)) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    metric_slots_[write] = event;
    metric_write_.store(next, std::memory_order_release);
    wake_sequence_.fetch_add(1, std::memory_order_release);
    wake_sequence_.notify_one();
    return true;
}

bool TimescaleTelemetry::pop_metric(Event& event) {
    const auto read = metric_read_.load(std::memory_order_relaxed);
    if (read == metric_write_.load(std::memory_order_acquire)) return false;
    event = metric_slots_[read];
    metric_read_.store((read + 1) % capacity_, std::memory_order_release);
    return true;
}

bool TimescaleTelemetry::publish_price(const char* symbol, const char* venue,
                                       std::uint32_t bid, std::uint32_t ask,
                                       std::int64_t received_at_ms,
                                       std::int64_t exchange_time_ms) {
    Event event;
    event.type = 1;
    event.timestamp_ms = received_at_ms;
    event.exchange_time_ms = exchange_time_ms;
    event.bid = bid;
    event.ask = ask;
    std::strncpy(event.symbol, symbol, sizeof(event.symbol) - 1);
    std::strncpy(event.venue, venue, sizeof(event.venue) - 1);
    return publish(event);
}

bool TimescaleTelemetry::publish_roundtrip(
    const char* symbol, std::int64_t timestamp_ms,
    std::uint32_t transaction_id, std::int64_t t_send_steady_ns,
    std::int64_t t_receive_steady_ns, double software_roundtrip_ns,
    double ws_interarrival_jitter_us, double modeled_singapore_total_ns,
    double modeled_tokyo_total_ns, double modeled_midpoint_total_ns,
    double pareto_tail_spike_ns) {
    Event event;
    event.type = 4;
    event.timestamp_ms = timestamp_ms;
    event.transaction_id = transaction_id;
    event.t_send_steady_ns = t_send_steady_ns;
    event.t_receive_steady_ns = t_receive_steady_ns;
    event.software_roundtrip_ns = software_roundtrip_ns;
    event.ws_interarrival_jitter_us = ws_interarrival_jitter_us;
    event.modeled_singapore_total_ns = modeled_singapore_total_ns;
    event.modeled_tokyo_total_ns = modeled_tokyo_total_ns;
    event.modeled_midpoint_total_ns = modeled_midpoint_total_ns;
    event.pareto_tail_spike_ns = pareto_tail_spike_ns;
    std::strncpy(event.symbol, symbol, sizeof(event.symbol) - 1);
    return publish(event);
}

bool TimescaleTelemetry::publish_metrics(
    std::int64_t sampled_at_ms, double process_cpu_pct,
    double system_cpu_pct, double process_ram_mb, double system_ram_used_mb,
    double latency_p50_ns, double latency_p99_ns, double roundtrip_p05_ns,
    double roundtrip_p50_ns,
    double roundtrip_p95_ns, double roundtrip_p99_ns, double roundtrip_max_ns,
    double modeled_singapore_p50_ns, double modeled_tokyo_p50_ns,
    double modeled_midpoint_p50_ns, double modeled_singapore_p99_ns,
    double modeled_tokyo_p99_ns, double modeled_midpoint_p99_ns,
    double ws_interarrival_jitter_us,
    std::uint64_t trades, double cumulative_pnl) {
    Event event;
    event.type = 2;
    event.timestamp_ms = sampled_at_ms;
    event.process_cpu_pct = process_cpu_pct;
    event.system_cpu_pct = system_cpu_pct;
    event.process_ram_mb = process_ram_mb;
    event.system_ram_used_mb = system_ram_used_mb;
    event.latency_p50_ns = latency_p50_ns;
    event.latency_p99_ns = latency_p99_ns;
    event.roundtrip_p05_ns = roundtrip_p05_ns;
    event.roundtrip_p50_ns = roundtrip_p50_ns;
    event.roundtrip_p95_ns = roundtrip_p95_ns;
    event.roundtrip_p99_ns = roundtrip_p99_ns;
    event.roundtrip_max_ns = roundtrip_max_ns;
    event.modeled_singapore_p50_ns = modeled_singapore_p50_ns;
    event.modeled_tokyo_p50_ns = modeled_tokyo_p50_ns;
    event.modeled_midpoint_p50_ns = modeled_midpoint_p50_ns;
    event.modeled_singapore_p99_ns = modeled_singapore_p99_ns;
    event.modeled_tokyo_p99_ns = modeled_tokyo_p99_ns;
    event.modeled_midpoint_p99_ns = modeled_midpoint_p99_ns;
    event.ws_interarrival_jitter_us = ws_interarrival_jitter_us;
    event.trades = trades;
    event.cumulative_pnl = cumulative_pnl;
    return publish_metric_event(event);
}

std::uint64_t TimescaleTelemetry::dropped() const noexcept {
    return dropped_.load(std::memory_order_relaxed);
}

void TimescaleTelemetry::run() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#elif defined(__linux__)
    sched_param priority{};
    pthread_setschedparam(pthread_self(), SCHED_IDLE, &priority);
    setpriority(PRIO_PROCESS, 0, 10);
#endif
    PGconn* connection = nullptr;
    bool schema_ready = false;
    bool prefer_decisions = false;
    std::vector<Event> batch;
    batch.reserve(256);
    while (true) {
        const auto observed = wake_sequence_.load(std::memory_order_relaxed);
        Event event;
        const auto count_queued_drops = [this, &event] {
            std::uint64_t queued = 0;
            while (pop(event) || pop_decision(event) || pop_metric(event)) ++queued;
            dropped_.fetch_add(queued, std::memory_order_relaxed);
        };
        while (batch.size() < 256) {
            const bool got_event = prefer_decisions
                ? (pop_decision(event) || pop(event) || pop_metric(event))
                : (pop(event) || pop_metric(event) || pop_decision(event));
            if (!got_event) break;
            batch.push_back(event);
            prefer_decisions = !prefer_decisions;
        }

        if (!batch.empty()) {
            if (!connection || PQstatus(connection) != CONNECTION_OK) {
                if (connection) PQfinish(connection);
                connection = PQconnectdb(connection_string_.c_str());
                schema_ready = false;
                if (!connection || PQstatus(connection) != CONNECTION_OK) {
                    std::cerr << "[TELEMETRY] TimescaleDB unavailable; queued data will be retried, overflow is counted\n";
                    if (connection) PQfinish(connection);
                    connection = nullptr;
                    if (stopping_.load(std::memory_order_acquire)) {
                        dropped_.fetch_add(batch.size(), std::memory_order_relaxed);
                        batch.clear();
                        count_queued_drops();
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    continue;
                }
            }
            if (!schema_ready) {
                schema_ready =
                    execute(connection, "CREATE TABLE IF NOT EXISTS market_prices (time TIMESTAMPTZ NOT NULL, symbol TEXT NOT NULL, venue TEXT NOT NULL, bid_scaled BIGINT NOT NULL, ask_scaled BIGINT NOT NULL, exchange_time TIMESTAMPTZ, exchange_event_age_ms DOUBLE PRECISION);") &&
                    execute(connection, "SELECT create_hypertable('market_prices','time',if_not_exists=>TRUE);") &&
                    execute(connection, "ALTER TABLE market_prices ADD COLUMN IF NOT EXISTS exchange_time TIMESTAMPTZ;") &&
                    execute(connection, "ALTER TABLE market_prices ADD COLUMN IF NOT EXISTS exchange_event_age_ms DOUBLE PRECISION;") &&
                    execute(connection, "CREATE TABLE IF NOT EXISTS runtime_metrics (time TIMESTAMPTZ NOT NULL, process_cpu_pct DOUBLE PRECISION, system_cpu_pct DOUBLE PRECISION, process_ram_mb DOUBLE PRECISION, system_ram_used_mb DOUBLE PRECISION, latency_p50_ns DOUBLE PRECISION, latency_p99_ns DOUBLE PRECISION, trades BIGINT NOT NULL, cumulative_pnl DOUBLE PRECISION NOT NULL);") &&
                    execute(connection, "SELECT create_hypertable('runtime_metrics','time',if_not_exists=>TRUE);") &&
                    execute(connection, "ALTER TABLE runtime_metrics ADD COLUMN IF NOT EXISTS roundtrip_p50_ns DOUBLE PRECISION;") &&
                    execute(connection, "ALTER TABLE runtime_metrics ADD COLUMN IF NOT EXISTS roundtrip_p05_ns DOUBLE PRECISION;") &&
                    execute(connection, "ALTER TABLE runtime_metrics ADD COLUMN IF NOT EXISTS roundtrip_p95_ns DOUBLE PRECISION;") &&
                    execute(connection, "ALTER TABLE runtime_metrics ADD COLUMN IF NOT EXISTS roundtrip_p99_ns DOUBLE PRECISION;") &&
                    execute(connection, "ALTER TABLE runtime_metrics ADD COLUMN IF NOT EXISTS roundtrip_max_ns DOUBLE PRECISION;") &&
                    execute(connection, "ALTER TABLE runtime_metrics ADD COLUMN IF NOT EXISTS modeled_singapore_p50_ns DOUBLE PRECISION;") &&
                    execute(connection, "ALTER TABLE runtime_metrics ADD COLUMN IF NOT EXISTS modeled_tokyo_p50_ns DOUBLE PRECISION;") &&
                    execute(connection, "ALTER TABLE runtime_metrics ADD COLUMN IF NOT EXISTS modeled_midpoint_p50_ns DOUBLE PRECISION;") &&
                    execute(connection, "ALTER TABLE runtime_metrics ADD COLUMN IF NOT EXISTS modeled_singapore_p99_ns DOUBLE PRECISION;") &&
                    execute(connection, "ALTER TABLE runtime_metrics ADD COLUMN IF NOT EXISTS modeled_tokyo_p99_ns DOUBLE PRECISION;") &&
                    execute(connection, "ALTER TABLE runtime_metrics ADD COLUMN IF NOT EXISTS modeled_midpoint_p99_ns DOUBLE PRECISION;") &&
                    execute(connection, "ALTER TABLE runtime_metrics ADD COLUMN IF NOT EXISTS ws_interarrival_jitter_us DOUBLE PRECISION;") &&
                    execute(connection, "CREATE TABLE IF NOT EXISTS latency_samples (time TIMESTAMPTZ NOT NULL, transaction_id BIGINT NOT NULL, symbol TEXT NOT NULL, t_send_steady_ns BIGINT NOT NULL, t_receive_steady_ns BIGINT NOT NULL, software_roundtrip_ns DOUBLE PRECISION NOT NULL, ws_interarrival_jitter_us DOUBLE PRECISION, modeled_singapore_total_ns DOUBLE PRECISION, modeled_tokyo_total_ns DOUBLE PRECISION, modeled_midpoint_total_ns DOUBLE PRECISION, pareto_tail_spike_ns DOUBLE PRECISION);") &&
                    execute(connection, "SELECT create_hypertable('latency_samples','time',if_not_exists=>TRUE);") &&
                    execute(connection, "CREATE TABLE IF NOT EXISTS strategy_decisions (time TIMESTAMPTZ NOT NULL, symbol TEXT NOT NULL, rtl_candidate BOOLEAN NOT NULL, accepted BOOLEAN NOT NULL, direction SMALLINT NOT NULL, history_samples SMALLINT NOT NULL DEFAULT 0, bybit_fee_bps SMALLINT NOT NULL DEFAULT 0, binance_fee_bps SMALLINT NOT NULL DEFAULT 0, paper_slippage_bps SMALLINT NOT NULL DEFAULT 0, zscore DOUBLE PRECISION NOT NULL, net_profit DOUBLE PRECISION NOT NULL);") &&
                    execute(connection, "SELECT create_hypertable('strategy_decisions','time',if_not_exists=>TRUE);") &&
                    execute(connection, "ALTER TABLE strategy_decisions ADD COLUMN IF NOT EXISTS history_samples SMALLINT NOT NULL DEFAULT 0;") &&
                    execute(connection, "ALTER TABLE strategy_decisions ADD COLUMN IF NOT EXISTS bybit_fee_bps SMALLINT NOT NULL DEFAULT 0;") &&
                    execute(connection, "ALTER TABLE strategy_decisions ADD COLUMN IF NOT EXISTS binance_fee_bps SMALLINT NOT NULL DEFAULT 0;") &&
                    execute(connection, "ALTER TABLE strategy_decisions ADD COLUMN IF NOT EXISTS paper_slippage_bps SMALLINT NOT NULL DEFAULT 0;");
                if (!schema_ready) {
                    PQfinish(connection);
                    connection = nullptr;
                    if (stopping_.load(std::memory_order_acquire)) {
                        dropped_.fetch_add(batch.size(), std::memory_order_relaxed);
                        batch.clear();
                        count_queued_drops();
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    continue;
                }
            }

            std::string sql = "BEGIN;";
            for (const auto& row : batch) {
                const auto timestamp = "to_timestamp(" +
                    number(static_cast<double>(row.timestamp_ms) / 1000.0) + ")";
                if (row.type == 1) {
                    char* symbol = PQescapeLiteral(connection, row.symbol,
                                                   std::strlen(row.symbol));
                    char* venue = PQescapeLiteral(connection, row.venue,
                                                  std::strlen(row.venue));
                    if (!symbol || !venue) continue;
                    sql += "INSERT INTO market_prices VALUES (" + timestamp +
                           "," + symbol + "," + venue + "," +
                           std::to_string(row.bid) + "," +
                           std::to_string(row.ask) + "," +
                           (row.exchange_time_ms > 0
                                ? "to_timestamp(" + number(static_cast<double>(row.exchange_time_ms) / 1000.0) + ")"
                                : "NULL") + "," +
                           (row.exchange_time_ms > 0
                                ? number(static_cast<double>(row.timestamp_ms - row.exchange_time_ms))
                                : "NULL") + ");";
                    PQfreemem(symbol);
                    PQfreemem(venue);
                } else if (row.type == 2) {
                    sql += "INSERT INTO runtime_metrics (time,process_cpu_pct,system_cpu_pct,process_ram_mb,system_ram_used_mb,latency_p50_ns,latency_p99_ns,trades,cumulative_pnl,roundtrip_p05_ns,roundtrip_p50_ns,roundtrip_p95_ns,roundtrip_p99_ns,roundtrip_max_ns,modeled_singapore_p50_ns,modeled_tokyo_p50_ns,modeled_midpoint_p50_ns,modeled_singapore_p99_ns,modeled_tokyo_p99_ns,modeled_midpoint_p99_ns,ws_interarrival_jitter_us) VALUES (" + timestamp +
                           "," + number(row.process_cpu_pct) + "," +
                           number(row.system_cpu_pct) + "," +
                           number(row.process_ram_mb) + "," +
                           number(row.system_ram_used_mb) + "," +
                           number(row.latency_p50_ns) + "," +
                           number(row.latency_p99_ns) + "," +
                           std::to_string(row.trades) + "," +
                           number(row.cumulative_pnl) + "," +
                           number(row.roundtrip_p05_ns) + "," +
                           number(row.roundtrip_p50_ns) + "," +
                           number(row.roundtrip_p95_ns) + "," +
                           number(row.roundtrip_p99_ns) + "," +
                           number(row.roundtrip_max_ns) + "," +
                           number(row.modeled_singapore_p50_ns) + "," +
                           number(row.modeled_tokyo_p50_ns) + "," +
                           number(row.modeled_midpoint_p50_ns) + "," +
                           number(row.modeled_singapore_p99_ns) + "," +
                           number(row.modeled_tokyo_p99_ns) + "," +
                           number(row.modeled_midpoint_p99_ns) + "," +
                           number(row.ws_interarrival_jitter_us) + ");";
                } else if (row.type == 3) {
                    char* symbol = PQescapeLiteral(connection, row.symbol,
                                                   std::strlen(row.symbol));
                    if (!symbol) continue;
                    sql += "INSERT INTO strategy_decisions (time,symbol,rtl_candidate,accepted,direction,history_samples,bybit_fee_bps,binance_fee_bps,paper_slippage_bps,zscore,net_profit) VALUES (" + timestamp +
                           "," + symbol + "," + (row.rtl_candidate ? "TRUE" : "FALSE") +
                           "," + (row.accepted ? "TRUE" : "FALSE") + "," +
                           std::to_string(row.direction) + "," +
                           std::to_string(row.history_samples) + "," +
                           std::to_string(row.bybit_fee_bps) + "," +
                           std::to_string(row.binance_fee_bps) + "," +
                           std::to_string(row.paper_slippage_bps) + "," +
                           number(row.zscore) +
                           "," + number(row.net_profit) + ");";
                    PQfreemem(symbol);
                } else {
                    char* symbol = PQescapeLiteral(connection, row.symbol,
                                                   std::strlen(row.symbol));
                    if (!symbol) continue;
                    sql += "INSERT INTO latency_samples (time,transaction_id,symbol,t_send_steady_ns,t_receive_steady_ns,software_roundtrip_ns,ws_interarrival_jitter_us,modeled_singapore_total_ns,modeled_tokyo_total_ns,modeled_midpoint_total_ns,pareto_tail_spike_ns) VALUES (" + timestamp + "," + std::to_string(row.transaction_id) + "," + symbol + "," + std::to_string(row.t_send_steady_ns) + "," + std::to_string(row.t_receive_steady_ns) + "," + number(row.software_roundtrip_ns) + "," + number(row.ws_interarrival_jitter_us) + "," + number(row.modeled_singapore_total_ns) + "," + number(row.modeled_tokyo_total_ns) + "," + number(row.modeled_midpoint_total_ns) + "," + number(row.pareto_tail_spike_ns) + ");";
                    PQfreemem(symbol);
                }
            }
            sql += "COMMIT;";
            PGresult* result = PQexec(connection, sql.c_str());
            const bool committed = result &&
                PQresultStatus(result) == PGRES_COMMAND_OK &&
                std::strcmp(PQcmdStatus(result), "COMMIT") == 0;
            if (result) PQclear(result);
            if (!committed) {
                execute(connection, "ROLLBACK;");
                schema_ready = false;
                dropped_.fetch_add(batch.size(), std::memory_order_relaxed);
            }
            batch.clear();
        }

        if (stopping_.load(std::memory_order_acquire)) {
            while (pop(event) || pop_decision(event) || pop_metric(event)) batch.push_back(event);
            if (batch.empty()) break;
        }
        if (batch.empty()) wake_sequence_.wait(observed, std::memory_order_relaxed);
    }
    if (connection) PQfinish(connection);
}

} // namespace metrics
