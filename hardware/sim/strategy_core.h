#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace strategy {

class RollingZScore {
public:
    static constexpr std::size_t capacity = 50;

    double score(double current) const {
        if (count_ < 2) return 0.0;
        const double mean = sum_ / static_cast<double>(count_);
        const double variance = std::max(
            0.0, (sum_squares_ - sum_ * sum_ / static_cast<double>(count_)) /
                     static_cast<double>(count_ - 1));
        const double deviation = std::sqrt(variance);
        return deviation == 0.0 ? 0.0 : (current - mean) / deviation;
    }

    void push(double value) {
        if (count_ == capacity) {
            const double old = values_[next_];
            sum_ -= old;
            sum_squares_ -= old * old;
        } else {
            ++count_;
        }
        values_[next_] = value;
        sum_ += value;
        sum_squares_ += value * value;
        next_ = (next_ + 1) % capacity;
    }

    std::size_t size() const noexcept { return count_; }
    bool ready() const noexcept { return count_ == capacity; }

private:
    std::array<double, capacity> values_{};
    std::size_t count_ = 0;
    std::size_t next_ = 0;
    double sum_ = 0.0;
    double sum_squares_ = 0.0;
};

struct RiskDecision {
    bool accepted;
    const char* reason;
};

inline RiskDecision evaluate_risk(bool rtl_candidate, bool history_ready,
                                  double zscore, double threshold) {
    if (!rtl_candidate) return {false, "RTL_NO_CANDIDATE"};
    if (!history_ready) return {false, "ZSCORE_WARMUP"};
    if (zscore < threshold) return {false, "ZSCORE_BELOW_THRESHOLD"};
    return {true, "RTL_AND_ZSCORE"};
}

} // namespace strategy
