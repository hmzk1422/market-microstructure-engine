#include "metrics_collector.h"

#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

namespace metrics {

namespace {

double not_measured() {
    return std::numeric_limits<double>::quiet_NaN();
}

bool parse_count_after_colon(const std::string& line, std::uint64_t& value) {
    const auto colon = line.find(':');
    if (colon == std::string::npos) {
        return false;
    }
    std::istringstream input(line.substr(colon + 1));
    return static_cast<bool>(input >> value);
}

} // namespace

std::string MetricsCollector::get_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &time);
#else
    localtime_r(&time, &local);
#endif
    std::ostringstream output;
    output << std::put_time(&local, "%Y-%m-%d %H:%M:%S");
    return output.str();
}

HardwareMetrics MetricsCollector::parse_synthesis_report(
    const std::string& report_path) {
    HardwareMetrics metrics;
    std::ifstream report(report_path);
    if (!report) {
        return metrics;
    }

    bool saw_cells = false;
    bool saw_wires = false;
    std::string line;
    while (std::getline(report, line)) {
        if (line.find("Number of cells:") != std::string::npos) {
            saw_cells = parse_count_after_colon(line, metrics.cell_count);
        } else if (line.find("Number of wires:") != std::string::npos) {
            saw_wires = parse_count_after_colon(line, metrics.wire_count);
        }
    }
    metrics.generic_synthesis_available = saw_cells && saw_wires;
    // Generic Yosys cells are not FPGA LUT/FF/BRAM/DSP counts. Leave target
    // resource and timing fields unavailable until a device-specific flow runs.
    return metrics;
}

SoftwareMetrics MetricsCollector::collect_software_metrics() {
    SoftwareMetrics metrics;
    metrics.cpu_usage_percent = not_measured();
    metrics.memory_usage_mb = not_measured();
    metrics.avg_latency_ns = not_measured();
    return metrics;
}

LatencyResults MetricsCollector::calculate_fiber_latency(
    const FiberLatencyConfig&, double, double) {
    // Do not turn an assumed route length, speed of light, or clock target
    // into a measured end-to-end latency claim.
    LatencyResults results;
    results.fiber_tx_ns = not_measured();
    results.fiber_rx_ns = not_measured();
    results.fiber_total_ns = not_measured();
    results.cpp_latency_ns = not_measured();
    results.verilog_ping_pong_ns = not_measured();
    results.total_latency_ns = not_measured();
    return results;
}

BenchmarkReport MetricsCollector::generate_report(
    const std::string& synthesis_report_path,
    const FiberLatencyConfig& fiber_config, double cpp_latency_cycles,
    double clock_freq_mhz, std::uint64_t transactions_processed) {
    BenchmarkReport report;
    report.timestamp = get_timestamp();
    report.hw = parse_synthesis_report(synthesis_report_path);
    report.sw = collect_software_metrics();
    report.sw.transactions_processed = transactions_processed;
    report.latency = calculate_fiber_latency(
        fiber_config, cpp_latency_cycles, clock_freq_mhz);
    report.valid = false;
    return report;
}

} // namespace metrics
