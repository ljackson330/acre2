// MSVC Concurrency Runtime shim for mingw-w64 cross-compilation.
//
// Provides the subset of concurrency::concurrent_unordered_set that
// ACRE2Core's SoundMixer uses: insert, find, begin, end, unsafe_erase.
//
// Backed by std::set for the same reason as the map shim -- node-based storage
// keeps iterators valid across insertion, which SoundMixer's mixdown loop
// relies on while channels are being added from other threads.
#pragma once

#include <mutex>
#include <set>
#include <utility>

namespace Concurrency {

template <typename T>
class concurrent_unordered_set {
public:
    using set_type = std::set<T>;
    using iterator = typename set_type::iterator;
    using const_iterator = typename set_type::const_iterator;

    std::pair<iterator, bool> insert(const T& v) {
        std::lock_guard<std::recursive_mutex> g(m_mutex);
        return m_set.insert(v);
    }

    iterator find(const T& v) {
        std::lock_guard<std::recursive_mutex> g(m_mutex);
        return m_set.find(v);
    }

    iterator unsafe_erase(iterator it) {
        std::lock_guard<std::recursive_mutex> g(m_mutex);
        return m_set.erase(it);
    }

    size_t unsafe_erase(const T& v) {
        std::lock_guard<std::recursive_mutex> g(m_mutex);
        return m_set.erase(v);
    }

    void clear() {
        std::lock_guard<std::recursive_mutex> g(m_mutex);
        m_set.clear();
    }

    size_t size() const {
        std::lock_guard<std::recursive_mutex> g(m_mutex);
        return m_set.size();
    }

    bool empty() const {
        std::lock_guard<std::recursive_mutex> g(m_mutex);
        return m_set.empty();
    }

    iterator begin() { return m_set.begin(); }
    iterator end()   { return m_set.end(); }
    const_iterator begin() const { return m_set.begin(); }
    const_iterator end()   const { return m_set.end(); }

private:
    set_type m_set;
    mutable std::recursive_mutex m_mutex;
};

}  // namespace Concurrency

namespace concurrency = Concurrency;
