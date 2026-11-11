// Small least-recently-used cache (not thread-safe: use it from one thread, the UI thread here).
#pragma once

#include <cstddef>
#include <list>
#include <unordered_map>
#include <utility>

namespace mappe {

template <typename K, typename V>
class LruCache {
public:
    explicit LruCache(std::size_t capacity) : _capacity(capacity) {}

    std::size_t Size() const { return _map.size(); }

    // Does not change the recency order.
    bool Contains(const K& key) const { return _map.find(key) != _map.end(); }

    // Marks the entry as most recently used.
    bool TryGet(const K& key, V& out) {
        auto it = _map.find(key);
        if (it == _map.end()) return false;
        _order.splice(_order.begin(), _order, it->second);
        out = it->second->second;
        return true;
    }

    void Put(const K& key, const V& value) {
        auto it = _map.find(key);
        if (it != _map.end()) {
            it->second->second = value;
            _order.splice(_order.begin(), _order, it->second);
            return;
        }
        _order.push_front(std::make_pair(key, value));
        _map[key] = _order.begin();
        while (_map.size() > _capacity) {
            auto last = std::prev(_order.end());
            _map.erase(last->first);
            _order.pop_back();
        }
    }

    bool Remove(const K& key) {
        auto it = _map.find(key);
        if (it == _map.end()) return false;
        _order.erase(it->second);
        _map.erase(it);
        return true;
    }

    void Clear() {
        _map.clear();
        _order.clear();
    }

private:
    std::size_t _capacity;
    std::list<std::pair<K, V>> _order;
    std::unordered_map<K, typename std::list<std::pair<K, V>>::iterator> _map;
};

}  // namespace mappe
