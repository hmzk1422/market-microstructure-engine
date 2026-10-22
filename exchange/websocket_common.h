#pragma once

#include <functional>
#include <string>
#include <vector>

void run_reconnecting_websocket(
    const std::string& host,
    const std::string& port,
    const std::string& target,
    const std::vector<std::string>& subscriptions,
    const std::string& venue,
    const std::function<void(const std::string&)>& on_message,
    const std::function<bool()>& stopping);
