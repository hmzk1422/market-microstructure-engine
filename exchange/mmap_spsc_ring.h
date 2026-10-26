#pragma once

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

// Named shared-memory SPSC ring used as a software stand-in for a PCIe/DMA
// window. The payload must remain fixed-size and trivially copyable.
template <typename T, std::size_t Capacity>
class MmapSpscRing {
    static_assert(Capacity > 1);
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(std::atomic<std::size_t>::is_always_lock_free);

    struct SharedState {
        std::atomic<std::size_t> write{0};
        std::atomic<std::size_t> read{0};
        std::atomic<std::size_t> wake_sequence{0};
        T slots[Capacity]{};
    };

public:
    MmapSpscRing() {
#ifdef _WIN32
        static std::atomic<std::uint64_t> sequence{0};
        name_ = "Local\\market_microstructure_dma_" +
                std::to_string(GetCurrentProcessId()) + "_" +
                std::to_string(sequence.fetch_add(1));
        const auto bytes = sizeof(SharedState);
        mapping_ = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr,
                                      PAGE_READWRITE,
                                      static_cast<DWORD>(bytes >> 32),
                                      static_cast<DWORD>(bytes), name_.c_str());
        if (!mapping_) throw_system_error("CreateFileMapping");
        state_ = static_cast<SharedState*>(MapViewOfFile(
            mapping_, FILE_MAP_ALL_ACCESS, 0, 0, bytes));
        if (!state_) {
            const auto error = GetLastError();
            CloseHandle(mapping_);
            mapping_ = nullptr;
            throw std::runtime_error("MapViewOfFile failed: " +
                                     std::to_string(error));
        }
#else
        static std::atomic<std::uint64_t> sequence{0};
        name_ = "/market_microstructure_dma_" +
                std::to_string(static_cast<long long>(getpid())) + "_" +
                std::to_string(sequence.fetch_add(1));
        descriptor_ = shm_open(name_.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
        if (descriptor_ < 0) throw_system_error("shm_open");
        if (ftruncate(descriptor_, static_cast<off_t>(sizeof(SharedState))) != 0) {
            const auto error = errno;
            close(descriptor_);
            descriptor_ = -1;
            shm_unlink(name_.c_str());
            throw std::runtime_error("ftruncate failed: " +
                                     std::to_string(error));
        }
        void* view = mmap(nullptr, sizeof(SharedState), PROT_READ | PROT_WRITE,
                          MAP_SHARED, descriptor_, 0);
        if (view == MAP_FAILED) {
            const auto error = errno;
            close(descriptor_);
            descriptor_ = -1;
            shm_unlink(name_.c_str());
            throw std::runtime_error("mmap failed: " +
                                     std::to_string(error));
        }
        state_ = static_cast<SharedState*>(view);
#endif
        new (state_) SharedState{};
    }

    ~MmapSpscRing() {
        if (state_) state_->~SharedState();
#ifdef _WIN32
        if (state_) UnmapViewOfFile(state_);
        if (mapping_) CloseHandle(mapping_);
#else
        if (state_) munmap(state_, sizeof(SharedState));
        if (descriptor_ >= 0) close(descriptor_);
        if (!name_.empty()) shm_unlink(name_.c_str());
#endif
    }

    MmapSpscRing(const MmapSpscRing&) = delete;
    MmapSpscRing& operator=(const MmapSpscRing&) = delete;

    bool try_push(const T& value) noexcept {
        const auto write = state_->write.load(std::memory_order_relaxed);
        const auto next = (write + 1) % Capacity;
        if (next == state_->read.load(std::memory_order_acquire)) return false;
        state_->slots[write] = value;
        state_->write.store(next, std::memory_order_release);
        state_->wake_sequence.fetch_add(1, std::memory_order_release);
        state_->wake_sequence.notify_one();
        return true;
    }

    bool try_pop(T& value) noexcept {
        const auto read = state_->read.load(std::memory_order_relaxed);
        if (read == state_->write.load(std::memory_order_acquire)) return false;
        value = state_->slots[read];
        state_->read.store((read + 1) % Capacity, std::memory_order_release);
        return true;
    }

    std::size_t wake_sequence() const noexcept {
        return state_->wake_sequence.load(std::memory_order_relaxed);
    }

    void wait_for_data(std::size_t observed) const noexcept {
        state_->wake_sequence.wait(observed, std::memory_order_relaxed);
    }

    void wake() noexcept {
        state_->wake_sequence.fetch_add(1, std::memory_order_release);
        state_->wake_sequence.notify_all();
    }

    const std::string& name() const noexcept { return name_; }

private:
    [[noreturn]] void throw_system_error(const char* operation) {
#ifdef _WIN32
        const auto error = GetLastError();
        throw std::runtime_error(std::string(operation) + " failed: " +
                                 std::to_string(error));
#else
        const auto error = errno;
        throw std::runtime_error(std::string(operation) + " failed: " +
                                 std::to_string(error));
#endif
    }

    SharedState* state_ = nullptr;
    std::string name_;
#ifdef _WIN32
    HANDLE mapping_ = nullptr;
#else
    int descriptor_ = -1;
#endif
};
