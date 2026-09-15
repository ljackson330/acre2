// MSVC Concurrency Runtime shim for mingw-w64 cross-compilation.
//
// Provides the subset of concurrency::concurrent_queue that ACRE2Core's
// NamedPipeServer uses: push, try_pop, clear.
#pragma once

#include <deque>
#include <mutex>

namespace Concurrency {

template <typename T>
class concurrent_queue {
public:
    void push(const T& item) {
        std::lock_guard<std::mutex> g(m_mutex);
        m_queue.push_back(item);
    }

    bool try_pop(T& out) {
        std::lock_guard<std::mutex> g(m_mutex);
        if (m_queue.empty()) {
            return false;
        }
        out = m_queue.front();
        m_queue.pop_front();
        return true;
    }

    void clear() {
        std::lock_guard<std::mutex> g(m_mutex);
        m_queue.clear();
    }

    bool empty() const {
        std::lock_guard<std::mutex> g(m_mutex);
        return m_queue.empty();
    }

    size_t unsafe_size() const {
        std::lock_guard<std::mutex> g(m_mutex);
        return m_queue.size();
    }

private:
    std::deque<T> m_queue;
    mutable std::mutex m_mutex;
};

}  // namespace Concurrency

namespace concurrency = Concurrency;
