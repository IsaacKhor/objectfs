#pragma once

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <shared_mutex>

/**
 * This is a thread safe, bounded, blocking, multi-producer multi-consumer,
 * single-ended, FIFO queue. Push operations block until there's space, and pop
 * blocks until there are entries in the queue to pop.
 *
 * It uses an underlying std::queue for the actual queue, and then just adds a
 * single global lock to both pop and push. Readers are notified when there are
 * entries via condition vars, same for writers.
 *
 * Based on CPython's queue implementation found at
 * https://github.com/python/cpython/blob/main/Lib/queue.py
 *
 * This queue is neither movable nor copyable. Use smart pointers instead.
 */
template <typename T> class BlockingMPMC
{
  public:
    BlockingMPMC(size_t size) : _buffer(), _max_capacity(size) {}
    ~BlockingMPMC();

    // TODO Change to take an rvalue to default move instead of copy
    void push(T t)
    {
        {
            std::unique_lock<std::mutex> lck(_mutex);
            _can_push.wait(lck,
                           [&]() { return _buffer.size() < _max_capacity; });
            _buffer.push(std::move(t));
        }
        _can_pop.notify_one();
    }

    T pop()
    {
        T x;
        {
            std::unique_lock<std::mutex> lck(_mutex);
            _can_pop.wait(lck, [&]() { return !_buffer.empty(); });
            x = std::move(_buffer.front());
            _buffer.pop();
        }
        _can_push.notify_one();
        return x;
    }

  private:
    BlockingMPMC(BlockingMPMC &src) = delete;
    BlockingMPMC(BlockingMPMC &&src) = delete;
    BlockingMPMC &operator=(BlockingMPMC &src) = delete;
    BlockingMPMC &operator=(BlockingMPMC &&src) = delete;

    std::queue<T> _buffer;
    size_t _max_capacity;
    std::mutex _mutex;
    std::condition_variable _can_pop;
    std::condition_variable _can_push;
};

/**
 * Concurrent std::map. Wraps std::map with mutexes
 */
template <typename K, typename V> class ConcurrentMap
{
  private:
    std::map<K, V> map;
    std::shared_mutex mutex;

    explicit ConcurrentMap(ConcurrentMap &other) = delete;
    ConcurrentMap &operator=(ConcurrentMap &other) = delete;

  public:
    explicit ConcurrentMap() = default;
    explicit ConcurrentMap(ConcurrentMap &&other) = default;
    ConcurrentMap &operator=(ConcurrentMap &&other) = default;

    V &at(const K &key)
    {
        std::shared_lock lock(mutex);
        return map.at(key);
    }

    // Can't do this because std::optional<T&> did not make it in :(
    // std::optional<V &> get(const K &key)
    // {
    //     std::shared_lock lock(mutex);
    //     if (map.contains(key))
    //         return map.at(key);
    //     return std::nullopt;
    // }

    std::optional<V> get_copy(const K &key)
    {
        std::shared_lock lock(mutex);
        if (map.contains(key))
            return std::make_optional<V>(map.at(key));
        return std::nullopt;
    }

    bool contains(const K &key)
    {
        std::shared_lock lock(mutex);
        return map.contains(key);
    }

    void insert(const K &key, const V &&value)
    {
        std::unique_lock lock(mutex);
        map.insert_or_assign(key, std::move(value));
    }

    void erase(const K &key)
    {
        std::unique_lock lock(mutex);
        map.erase(key);
    }

    std::vector<std::pair<K, V>> entries()
    {
        std::shared_lock lock(mutex);
        std::vector<std::pair<K, V>> items;
        for (auto &item : map)
            items.push_back(item);
        return items;
    }

    bool empty()
    {
        std::shared_lock lock(mutex);
        return map.empty();
    }
};

/**
 * LRU cache. Implemented with a map and a linked list. Each cached element
 * is put on the list, and the map contains pointers to the list elements.
 * On access, the element is moved to the front of the list, and on eviction,
 * the element at the back of the list is removed.
 */
template <typename K, typename V> class LRUCache
{
};
