#include "websocket_feed.h"
#include "spsc_ring.h"

#include <algorithm>
#include <atomic>
#include <functional>
#include <iostream>
#include <semaphore>
#include <thread>
#include <utility>
#include <vector>

void run_bybit_connector(
    const std::vector<std::string>& symbols,
    const std::function<void(QuoteUpdate)>& publish,
    const std::function<void()>& malformed,
    const std::function<bool()>& stopping);

void run_binance_connector(
    const std::vector<std::string>& symbols,
    const std::function<void(QuoteUpdate)>& publish,
    const std::function<void()>& malformed,
    const std::function<bool()>& stopping);

struct WebSocketFeed::Impl {
    explicit Impl(std::vector<std::string> configured_symbols)
        : symbols(std::move(configured_symbols)) {}

    static constexpr std::size_t queue_capacity = 32768;
    std::vector<std::string> symbols;
    SpscRing<QuoteUpdate, queue_capacity> bybit_updates;
    SpscRing<QuoteUpdate, queue_capacity> binance_updates;
    std::counting_semaphore<65536> available{0};
    std::atomic<bool> bybit_first{true};
    std::atomic<bool> stopping{false};
    std::atomic<std::size_t> dropped{0};
    std::atomic<std::size_t> malformed{0};
    std::thread bybit_thread;
    std::thread binance_thread;

    void publish(Venue venue, QuoteUpdate update) {
        const bool queued = venue == Venue::Bybit
                                ? bybit_updates.try_push(std::move(update))
                                : binance_updates.try_push(std::move(update));
        if (!queued) {
            dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        available.release();
    }

    void start() {
        const auto publish_bybit = [this](QuoteUpdate update) {
            publish(Venue::Bybit, std::move(update));
        };
        const auto publish_binance = [this](QuoteUpdate update) {
            publish(Venue::Binance, std::move(update));
        };
        const auto record_malformed = [this] {
            malformed.fetch_add(1, std::memory_order_relaxed);
        };
        const auto is_stopping = [this] {
            return stopping.load(std::memory_order_relaxed);
        };

        bybit_thread = std::thread([=, this] {
            run_bybit_connector(symbols, publish_bybit, record_malformed,
                                is_stopping);
        });
        binance_thread = std::thread([=, this] {
            run_binance_connector(symbols, publish_binance, record_malformed,
                                  is_stopping);
        });
    }

    void stop() {
        stopping.store(true, std::memory_order_relaxed);
        available.release();
        if (bybit_thread.joinable()) {
            bybit_thread.join();
        }
        if (binance_thread.joinable()) {
            binance_thread.join();
        }
    }
};

WebSocketFeed::WebSocketFeed(std::vector<std::string> symbols)
    : impl_(std::make_unique<Impl>(std::move(symbols))) {}

WebSocketFeed::~WebSocketFeed() {
    stop();
}

void WebSocketFeed::start() {
    impl_->start();
}

bool WebSocketFeed::wait_for_update(QuoteUpdate& update,
                                    std::chrono::milliseconds timeout) {
    if (!impl_->available.try_acquire_for(timeout)) return false;
    const bool check_bybit = impl_->bybit_first.exchange(
        !impl_->bybit_first.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    return check_bybit
               ? (impl_->bybit_updates.try_pop(update) ||
                  impl_->binance_updates.try_pop(update))
               : (impl_->binance_updates.try_pop(update) ||
                  impl_->bybit_updates.try_pop(update));
}

void WebSocketFeed::stop() {
    if (impl_) {
        impl_->stop();
    }
}

std::size_t WebSocketFeed::dropped_updates() const {
    return impl_->dropped.load(std::memory_order_relaxed);
}

std::size_t WebSocketFeed::malformed_messages() const {
    return impl_->malformed.load(std::memory_order_relaxed);
}
