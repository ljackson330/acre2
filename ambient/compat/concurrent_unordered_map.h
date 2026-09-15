// MSVC Concurrency Runtime shim for mingw-w64 cross-compilation.
//
// ACRE2Core uses concurrency::concurrent_unordered_map, which ships only with
// Visual C++. This provides the subset ACRE2 actually uses:
//   operator[], find, begin, end, insert, unsafe_erase, clear
//
// Backed by std::map rather than std::unordered_map deliberately. MSVC's
// concurrent_unordered_map permits insertion concurrent with traversal;
// std::unordered_map invalidates all iterators when an insert triggers a
// rehash, which would be a real use-after-free in KeyHandlerEngine's iteration
// loop. std::map is node-based and never invalidates iterators on insert, so
// it preserves the guarantee the calling code was written against.
#pragma once

#include <map>
#include <mutex>
#include <utility>

namespace Concurrency {

template <typename Key, typename Value>
class concurrent_unordered_map {
public:
    using map_type = std::map<Key, Value>;
    using iterator = typename map_type::iterator;
    using const_iterator = typename map_type::const_iterator;
    using value_type = typename map_type::value_type;

    Value& operator[](const Key& k) {
        std::lock_guard<std::recursive_mutex> g(m_mutex);
        return m_map[k];
    }

    iterator find(const Key& k) {
        std::lock_guard<std::recursive_mutex> g(m_mutex);
        return m_map.find(k);
    }

    std::pair<iterator, bool> insert(const value_type& v) {
        std::lock_guard<std::recursive_mutex> g(m_mutex);
        return m_map.insert(v);
    }

    // MSVC spells erase "unsafe_erase" to flag that it is not concurrency-safe.
    iterator unsafe_erase(iterator it) {
        std::lock_guard<std::recursive_mutex> g(m_mutex);
        return m_map.erase(it);
    }

    size_t unsafe_erase(const Key& k) {
        std::lock_guard<std::recursive_mutex> g(m_mutex);
        return m_map.erase(k);
    }

    void clear() {
        std::lock_guard<std::recursive_mutex> g(m_mutex);
        m_map.clear();
    }

    size_t size() const {
        std::lock_guard<std::recursive_mutex> g(m_mutex);
        return m_map.size();
    }

    bool empty() const {
        std::lock_guard<std::recursive_mutex> g(m_mutex);
        return m_map.empty();
    }

    // Unsynchronized by design -- matches MSVC, where traversal is concurrent
    // with insertion but the caller serializes against erase.
    iterator begin() { return m_map.begin(); }
    iterator end()   { return m_map.end(); }
    const_iterator begin() const { return m_map.begin(); }
    const_iterator end()   const { return m_map.end(); }

private:
    map_type m_map;
    mutable std::recursive_mutex m_mutex;
};

}  // namespace Concurrency

// MSVC exposes both spellings; ACRE2 uses each in different files.
namespace concurrency = Concurrency;
