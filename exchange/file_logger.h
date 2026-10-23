#pragma once

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>

inline void exchange_log(const std::string& component,
                         const std::string& message) {
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    std::filesystem::create_directories("logs/exchange");
    const auto now = std::chrono::system_clock::now();
    const auto stamp = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &stamp);
#else
    localtime_r(&stamp, &tm);
#endif
    std::ostringstream line;
    line << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << " [" << component
         << "] " << message << '\n';
    std::ofstream out("logs/exchange/" + component + ".log", std::ios::app);
    out << line.str();
}
