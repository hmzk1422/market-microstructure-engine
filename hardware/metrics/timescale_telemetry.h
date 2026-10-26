#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace metrics {

// Nonblocking single-producer/single-consumer telemetry sink. The market loop
// only copies fixed-size records into RAM; libpq runs on the writer thread.
class TimescaleTelemetry {
public:
    explicit TimescaleTelemetry(std::string connection_string,
                                std::size_t capacity = 65'536);
    ~TimescaleTelemetry();
    void stop();
    TimescaleTelemetry(const TimescaleTelemetry&) = delete;
    TimescaleTelemetry& operator=(const TimescaleTelemetry&) = delete;

    bool publish_price(const char* symbol, const char* venue,
                       std::uint32_t bid, std::uint32_t ask,
                       std::int64_t received_at_ms,
                       std::int64_t exchange_time_ms);
    bool publish_roundtrip(const char* symbol, std::int64_t timestamp_ms,
                           std::uint32_t transaction_id,
                           std::int64_t t_send_steady_ns,
                           std::int64_t t_receive_steady_ns,
                           double software_roundtrip_ns,
                           double ws_interarrival_jitter_us,
                           double modeled_singapore_total_ns,
                           double modeled_tokyo_total_ns,
                           double modeled_midpoint_total_ns,
                           double pareto_tail_spike_ns);
    bool publish_metrics(std::int64_t sampled_at_ms, double process_cpu_pct,
                         double system_cpu_pct, double process_ram_mb,
                         double system_ram_used_mb, double latency_p50_ns,
                         double latency_p99_ns, double roundtrip_p05_ns,
                         double roundtrip_p50_ns,
                         double roundtrip_p95_ns, double roundtrip_p99_ns,
                         double roundtrip_max_ns,
                         double modeled_singapore_p50_ns,
                         double modeled_tokyo_p50_ns,
                         double modeled_midpoint_p50_ns,
                         double modeled_singapore_p99_ns,
                         double modeled_tokyo_p99_ns,
                         double modeled_midpoint_p99_ns,
                         double ws_interarrival_jitter_us,
                         std::uint64_t trades, double cumulative_pnl);
    bool publish_decision(const char* symbol, std::int64_t timestamp_ms,
                          bool rtl_candidate, bool accepted,
                          std::uint8_t direction, std::uint16_t history_samples,
                          std::uint16_t bybit_fee_bps,
                          std::uint16_t binance_fee_bps,
                          std::uint16_t paper_slippage_bps, double zscore,
                          double net_profit);
    std::uint64_t dropped() const noexcept;

private:
    struct Event {
        std::int64_t timestamp_ms = 0;
        std::int64_t exchange_time_ms = 0;
        std::uint8_t type = 0;
        char symbol[32]{};
        char venue[12]{};
        std::uint32_t bid = 0;
        std::uint32_t ask = 0;
        std::uint32_t transaction_id = 0;
        std::int64_t t_send_steady_ns = 0;
        std::int64_t t_receive_steady_ns = 0;
        double process_cpu_pct = 0;
        double system_cpu_pct = 0;
        double process_ram_mb = 0;
        double system_ram_used_mb = 0;
        double latency_p50_ns = 0;
        double latency_p99_ns = 0;
        double roundtrip_p05_ns = 0;
        double roundtrip_p50_ns = 0;
        double roundtrip_p95_ns = 0;
        double roundtrip_p99_ns = 0;
        double roundtrip_max_ns = 0;
        double modeled_singapore_p50_ns = 0;
        double modeled_tokyo_p50_ns = 0;
        double modeled_midpoint_p50_ns = 0;
        double modeled_singapore_p99_ns = 0;
        double modeled_tokyo_p99_ns = 0;
        double modeled_midpoint_p99_ns = 0;
        double ws_interarrival_jitter_us = 0;
        std::uint64_t trades = 0;
        double cumulative_pnl = 0;
        bool rtl_candidate = false;
        bool accepted = false;
        std::uint8_t direction = 0;
        std::uint16_t history_samples = 0;
        std::uint16_t bybit_fee_bps = 0;
        std::uint16_t binance_fee_bps = 0;
        std::uint16_t paper_slippage_bps = 0;
        double zscore = 0;
        double net_profit = 0;
        double software_roundtrip_ns = 0;
        double modeled_singapore_total_ns = 0;
        double modeled_tokyo_total_ns = 0;
        double modeled_midpoint_total_ns = 0;
        double pareto_tail_spike_ns = 0;
    };

    bool publish(const Event& event);
    bool publish_metric_event(const Event& event);
    bool pop(Event& event);
    bool pop_decision(Event& event);
    bool pop_metric(Event& event);
    void run();

    std::string connection_string_;
    std::size_t capacity_;
    std::unique_ptr<Event[]> slots_;
    std::unique_ptr<Event[]> decision_slots_;
    std::unique_ptr<Event[]> metric_slots_;
    alignas(64) std::atomic<std::size_t> write_{0};
    alignas(64) std::atomic<std::size_t> read_{0};
    alignas(64) std::atomic<std::size_t> decision_write_{0};
    alignas(64) std::atomic<std::size_t> decision_read_{0};
    alignas(64) std::atomic<std::size_t> metric_write_{0};
    alignas(64) std::atomic<std::size_t> metric_read_{0};
    std::atomic<std::size_t> wake_sequence_{0};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<bool> stopping_{false};
    std::thread thread_;
};

} // namespace metrics
