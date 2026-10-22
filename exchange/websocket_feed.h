#pragma once

#include "market_data.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

class WebSocketFeed {
public:
    explicit WebSocketFeed(std::vector<std::string> symbols);
    ~WebSocketFeed();

    WebSocketFeed(const WebSocketFeed&) = delete;
    WebSocketFeed& operator=(const WebSocketFeed&) = delete;

    void start();
    bool wait_for_update(QuoteUpdate& update,
                         std::chrono::milliseconds timeout);
    void stop();
    std::size_t dropped_updates() const;
    std::size_t malformed_messages() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
