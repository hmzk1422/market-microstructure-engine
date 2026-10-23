#pragma once

#ifndef METRICS_COLLECTOR_H
#define METRICS_COLLECTOR_H

#include <cstdint>
#include <limits>
#include <string>

namespace metrics {

// Generic Yosys counts are not target FPGA resource estimates. Zero capacity
// means that the corresponding device metric was not measured.
struct HardwareMetrics {
    std::uint32_t luts_used = 0;
    std::uint32_t luts_available = 0;
    float luts_utilization = 0.0f;
    std::uint32_t flops_used = 0;
    std::uint32_t flops_available = 0;
    float flops_utilization = 0.0f;
    std::uint32_t bram_used = 0;
    std::uint32_t bram_available = 0;
    float bram_utilization = 0.0f;
    std::uint32_t dsp_used = 0;
    std::uint32_t dsp_available = 0;
    float dsp_utilization = 0.0f;
    float fmax_mhz = std::numeric_limits<float>::quiet_NaN();
    float wns_ns = std::numeric_limits<float>::quiet_NaN();
    float whs_ns = std::numeric_limits<float>::quiet_NaN();
    std::uint64_t cell_count = 0;
    std::uint64_t wire_count = 0;
    bool generic_synthesis_available = false;
};

struct SoftwareMetrics {
    double cpu_usage_percent = std::numeric_limits<double>::quiet_NaN();
    double memory_usage_mb = std::numeric_limits<double>::quiet_NaN();
    std::uint64_t cycles_elapsed = 0;
    std::uint64_t transactions_processed = 0;
    double avg_latency_ns = std::numeric_limits<double>::quiet_NaN();
};

// Kept for source compatibility. An assumed route length is not a latency
// measurement and is deliberately not used by MetricsCollector.
struct FiberLatencyConfig {
    double distance_km = 0.0;
    double ns_per_km = 0.0;
};

struct LatencyResults {
    double fiber_tx_ns = std::numeric_limits<double>::quiet_NaN();
    double fiber_rx_ns = std::numeric_limits<double>::quiet_NaN();
    double fiber_total_ns = std::numeric_limits<double>::quiet_NaN();
    double cpp_latency_ns = std::numeric_limits<double>::quiet_NaN();
    double verilog_ping_pong_ns = std::numeric_limits<double>::quiet_NaN();
    double total_latency_ns = std::numeric_limits<double>::quiet_NaN();
};

struct BenchmarkReport {
    std::string timestamp;
    HardwareMetrics hw;
    SoftwareMetrics sw;
    LatencyResults latency;
    bool valid = false;
};

class MetricsCollector {
public:
    // Extract only generic cell and wire counts from a Yosys text report.
    static HardwareMetrics parse_synthesis_report(const std::string& report_path);
    static SoftwareMetrics collect_software_metrics();
    static LatencyResults calculate_fiber_latency(
        const FiberLatencyConfig& config, double cpp_latency_cycles,
        double clock_freq_mhz);
    static BenchmarkReport generate_report(
        const std::string& synthesis_report_path,
        const FiberLatencyConfig& fiber_config, double cpp_latency_cycles,
        double clock_freq_mhz, std::uint64_t transactions_processed);
    static std::string get_timestamp();
};

} // namespace metrics

#endif // METRICS_COLLECTOR_H
