#include "network_latency_model.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "network_latency_model_test: FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
}

int main() {
    using namespace latency_model;
    const auto singapore_to_tokyo = Model::great_circle_km(
        Model::location(FirmLocation::Singapore),
        Model::location(FirmLocation::Tokyo));
    require(singapore_to_tokyo > 5'000 && singapore_to_tokyo < 5'500,
            "Singapore-Tokyo great-circle distance is plausible");
    require(Model::round_trip_fiber_floor_ns(
                FirmLocation::Singapore, VenueEndpoint::BybitSingapore) == 0,
            "same-city path has a zero propagation floor");
    const auto cross_city_floor = Model::round_trip_fiber_floor_ns(
        FirmLocation::Singapore, VenueEndpoint::BinanceTokyo);
    require(cross_city_floor > 65'000'000 && cross_city_floor < 75'000'000,
            "cross-city fiber RTT uses refractive-index propagation and route stretch");

    const auto midpoint_bybit = Model::round_trip_fiber_floor_ns(
        FirmLocation::Midpoint, VenueEndpoint::BybitSingapore);
    const auto midpoint_binance = Model::round_trip_fiber_floor_ns(
        FirmLocation::Midpoint, VenueEndpoint::BinanceTokyo);
    require(std::max(midpoint_bybit, midpoint_binance) < cross_city_floor,
            "midpoint reduces the slowest modeled leg");

    Model first(1234, 1.35, 0.01, 2.5);
    Model second(1234, 1.35, 0.01, 2.5);
    bool saw_tail = false;
    for (int i = 0; i < 20'000; ++i) {
        const auto a = first.sample(100'000, 100.0);
        const auto b = second.sample(100'000, 100.0);
        if (a.singapore_total_ns != b.singapore_total_ns ||
            a.tokyo_total_ns != b.tokyo_total_ns ||
            a.midpoint_total_ns != b.midpoint_total_ns) {
            std::cerr << "sample " << i << " first="
                      << a.singapore_total_ns << ',' << a.tokyo_total_ns << ','
                      << a.midpoint_total_ns << " second="
                      << b.singapore_total_ns << ',' << b.tokyo_total_ns << ','
                      << b.midpoint_total_ns << '\n';
        }
        require(a.singapore_total_ns == b.singapore_total_ns &&
                    a.tokyo_total_ns == b.tokyo_total_ns &&
                    a.midpoint_total_ns == b.midpoint_total_ns,
                "fixed seed produces reproducible route samples");
        require(a.singapore_total_ns >= 100'000 &&
                    a.tokyo_total_ns >= 100'000 &&
                    a.midpoint_total_ns >= 100'000,
                "modeled totals never fall below software time");
        saw_tail = saw_tail || a.tail_spike_ns > 250'000.0;
    }
    require(saw_tail, "Pareto component produces rare positive tail spikes");

    std::cout << "network_latency_model_test: PASS (route floors, midpoint, "
                 "determinism, Pareto tails)\n";
    return EXIT_SUCCESS;
}
