#include "websocket_feed.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <thread>
#include <utility>

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
    std::deque<QuoteUpdate> updates;
    mutable std::mutex mutex;
    std::condition_variable available;
    std::atomic<bool> stopping{false};
    std::atomic<std::size_t> dropped{0};
    std::atomic<std::size_t> malformed{0};
    std::thread bybit_thread;
    std::thread binance_thread;

    void publish(QuoteUpdate update) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (updates.size() == queue_capacity) {
                updates.pop_front();
                dropped.fetch_add(1, std::memory_order_relaxed);
            }
            updates.push_back(std::move(update));
        }
        available.notify_one();
    }

    void start() {
        const auto publish_update = [this](QuoteUpdate update) {
            publish(std::move(update));
        };
        const auto record_malformed = [this] {
            malformed.fetch_add(1, std::memory_order_relaxed);
        };
        const auto is_stopping = [this] {
            return stopping.load(std::memory_order_relaxed);
        };

        bybit_thread = std::thread([=, this] {
            run_bybit_connector(symbols, publish_update, record_malformed,
                                is_stopping);
        });
        binance_thread = std::thread([=, this] {
            run_binance_connector(symbols, publish_update, record_malformed,
                                  is_stopping);
        });
    }

    void stop() {
        stopping.store(true, std::memory_order_relaxed);
        available.notify_all();
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
    std::unique_lock<std::mutex> lock(impl_->mutex);
    impl_->available.wait_for(lock, timeout, [this] {
        return !impl_->updates.empty() ||
               impl_->stopping.load(std::memory_order_relaxed);
    });
    if (impl_->updates.empty()) {
        return false;
    }
    update = std::move(impl_->updates.front());
    impl_->updates.pop_front();
    return true;
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
