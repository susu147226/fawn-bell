#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <windows.h>

namespace luling::fs {
class EnumerationCancelled : public std::runtime_error {
public:
    EnumerationCancelled() : std::runtime_error("Directory enumeration cancelled") {}
};

// Separate quotas keep stalled network providers from consuming local capacity.
// State belongs to the provider thread until its I/O has actually completed.
inline std::atomic<unsigned> active_local_enumerations{0};
inline std::atomic<unsigned> active_network_enumerations{0};
// Shell namespace folders (a phone's storages over WPD, cloud roots) get their
// own small quota: a device that stops answering must not eat local slots.
inline std::atomic<unsigned> active_shell_enumerations{0};

template<class Result, class Provider, class Cancelled>
Result RunBoundedEnumeration(bool network, Provider provider, Cancelled cancelled,
    std::chrono::milliseconds budget = std::chrono::seconds(15), bool shell = false) {
    if (cancelled()) throw EnumerationCancelled();
    if (shell) budget = std::max(budget, std::chrono::milliseconds(30000));
    auto* active = shell ? &active_shell_enumerations
                        : network ? &active_network_enumerations : &active_local_enumerations;
    const unsigned limit = shell ? 2u : 4u;
    if (active->fetch_add(1) >= limit) {
        --*active;
        throw std::runtime_error("Directory provider capacity exhausted");
    }
    struct State {
        std::atomic_bool cancel{false};
        std::atomic_bool slot_held{true};
        std::mutex mutex;
        std::condition_variable ready;
        bool done = false;
        std::optional<Result> result;
        std::exception_ptr error;
    };
    std::shared_ptr<State> state;
    std::thread thread;
    try {
        state = std::make_shared<State>();
        thread = std::thread([state, active, provider = std::move(provider)]() mutable {
            try { state->result.emplace(provider(state->cancel)); }
            catch (...) { state->error = std::current_exception(); }
            {
                std::lock_guard lock(state->mutex);
                state->done = true;
            }
            // A waiter that already gave up released the slot for us.
            if (state->slot_held.exchange(false)) --*active;
            state->ready.notify_one();
        });
    } catch (...) { --*active; throw; }
    const auto deadline = std::chrono::steady_clock::now() + budget;
    try {
        std::unique_lock lock(state->mutex);
        for (;;) {
            // Check cancellation before accepting even an already completed result.
            if (cancelled()) throw EnumerationCancelled();
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("Directory enumeration timed out");
            if (state->done) break;
            state->ready.wait_for(lock, std::chrono::milliseconds(20));
        }
        lock.unlock();
        thread.join();
    } catch (...) {
        state->cancel = true;
        CancelSynchronousIo(thread.native_handle());
        thread.detach();
        // A phone that stopped answering may never return: release its slot
        // so the next attempt is not refused forever. Local and network work
        // keeps its slot until the provider really finishes.
        if (shell && state->slot_held.exchange(false)) --*active;
        throw;
    }
    if (state->error) std::rethrow_exception(state->error);
    return std::move(*state->result);
}
} // namespace luling::fs
