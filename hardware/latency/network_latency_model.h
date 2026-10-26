#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>

namespace latency_model {

enum class FirmLocation : std::size_t { Singapore = 0, Tokyo = 1, Midpoint = 2 };
enum class VenueEndpoint : std::size_t { BybitSingapore = 0, BinanceTokyo = 1 };

struct Coordinates {
    double latitude_deg;
    double longitude_deg;
};

struct ScenarioSample {
    double singapore_total_ns;
    double tokyo_total_ns;
    double midpoint_total_ns;
    double ws_interarrival_jitter_us;
    double tail_spike_ns;
};

class Model {
public:
    explicit Model(std::uint64_t seed = 42,
                   double route_stretch = 1.35,
                   double pareto_probability = 0.001,
                   double pareto_alpha = 2.5)
        : random_(seed), normal_(0.0, 1.0), uniform_(0.0, 1.0),
          route_stretch_(route_stretch),
          pareto_probability_(pareto_probability),
          pareto_alpha_(pareto_alpha) {}

    static constexpr Coordinates location(FirmLocation value) {
        switch (value) {
        case FirmLocation::Singapore: return {1.3521, 103.8198};
        case FirmLocation::Tokyo: return {35.6762, 139.6503};
        case FirmLocation::Midpoint: return {18.5145, 126.7351};
        }
        return {};
    }

    static constexpr Coordinates endpoint(VenueEndpoint value) {
        return value == VenueEndpoint::BybitSingapore
                   ? Coordinates{1.3521, 103.8198}
                   : Coordinates{35.6762, 139.6503};
    }

    static double great_circle_km(Coordinates a, Coordinates b) {
        constexpr double radians = 3.14159265358979323846 / 180.0;
        constexpr double earth_radius_km = 6371.0088;
        const double lat1 = a.latitude_deg * radians;
        const double lat2 = b.latitude_deg * radians;
        const double dlat = (b.latitude_deg - a.latitude_deg) * radians;
        const double dlon = (b.longitude_deg - a.longitude_deg) * radians;
        const double h = std::sin(dlat / 2.0) * std::sin(dlat / 2.0) +
                         std::cos(lat1) * std::cos(lat2) *
                             std::sin(dlon / 2.0) * std::sin(dlon / 2.0);
        return 2.0 * earth_radius_km *
               std::asin(std::sqrt(std::clamp(h, 0.0, 1.0)));
    }

    static double one_way_fiber_floor_ns(double distance_km,
                                          double route_stretch = 1.35,
                                          double refractive_index = 1.468) {
        constexpr double vacuum_speed_m_per_s = 299'792'458.0;
        if (distance_km < 0 || route_stretch < 1.0 || refractive_index < 1.0)
            return std::numeric_limits<double>::quiet_NaN();
        return distance_km * 1000.0 * route_stretch * refractive_index /
               vacuum_speed_m_per_s * 1.0e9;
    }

    static double round_trip_fiber_floor_ns(FirmLocation firm,
                                             VenueEndpoint venue,
                                             double route_stretch = 1.35) {
        const auto distance = great_circle_km(location(firm), endpoint(venue));
        return 2.0 * one_way_fiber_floor_ns(distance, route_stretch);
    }

    ScenarioSample sample(double software_round_trip_ns,
                          double ws_interarrival_jitter_us) {
        last_tail_spike_ns_ = 0.0;
        const double bybit_sg = sample_path(FirmLocation::Singapore,
                                            VenueEndpoint::BybitSingapore,
                                            ws_interarrival_jitter_us);
        const double binance_sg = sample_path(FirmLocation::Singapore,
                                              VenueEndpoint::BinanceTokyo,
                                              ws_interarrival_jitter_us);
        const double bybit_tokyo = sample_path(FirmLocation::Tokyo,
                                               VenueEndpoint::BybitSingapore,
                                               ws_interarrival_jitter_us);
        const double binance_tokyo = sample_path(FirmLocation::Tokyo,
                                                 VenueEndpoint::BinanceTokyo,
                                                 ws_interarrival_jitter_us);
        const double bybit_mid = sample_path(FirmLocation::Midpoint,
                                             VenueEndpoint::BybitSingapore,
                                             ws_interarrival_jitter_us);
        const double binance_mid = sample_path(FirmLocation::Midpoint,
                                               VenueEndpoint::BinanceTokyo,
                                               ws_interarrival_jitter_us);
        return {software_round_trip_ns + std::max(bybit_sg, binance_sg),
                software_round_trip_ns + std::max(bybit_tokyo, binance_tokyo),
                software_round_trip_ns + std::max(bybit_mid, binance_mid),
                ws_interarrival_jitter_us,
                last_tail_spike_ns_};
    }

private:
    double sample_path(FirmLocation firm, VenueEndpoint venue,
                       double observed_jitter_us) {
        const auto index = static_cast<std::size_t>(firm) * 2 +
                           static_cast<std::size_t>(venue);
        const double sigma_us = std::max(1.0, observed_jitter_us * 0.25);
        constexpr double mean_us = 50.0;
        constexpr double mean_reversion = 0.08;
        ou_state_us_[index] += mean_reversion *
                                   (mean_us - ou_state_us_[index]) +
                               sigma_us * normal_(random_);
        double jitter_us = std::max(0.0, ou_state_us_[index]);
        if (uniform_(random_) < pareto_probability_) {
            const double scale_us = std::max(50.0, observed_jitter_us * 5.0);
            const double u = std::max(1.0e-12, 1.0 - uniform_(random_));
            const double spike_us = scale_us *
                (std::pow(u, -1.0 / pareto_alpha_) - 1.0);
            jitter_us += spike_us;
            last_tail_spike_ns_ = spike_us * 1000.0;
        }
        return round_trip_fiber_floor_ns(firm, venue, route_stretch_) +
               jitter_us * 1000.0;
    }

    std::mt19937_64 random_;
    std::normal_distribution<double> normal_;
    std::uniform_real_distribution<double> uniform_;
    double route_stretch_;
    double pareto_probability_;
    double pareto_alpha_;
    std::array<double, 6> ou_state_us_{50.0, 50.0, 50.0,
                                      50.0, 50.0, 50.0};
    double last_tail_spike_ns_ = 0.0;
};

} // namespace latency_model
