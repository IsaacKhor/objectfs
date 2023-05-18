#pragma once

#include <atomic>
#include <optional>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <queue>

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
    BlockingMPMC(size_t size);
    ~BlockingMPMC();

    // TODO Change to take an rvalue to default move instead of copy
    void push(T t);
    T pop();

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
template <typename K, typename V> class SharedConcurrentMap
{
  private:
    std::map<K, V> map;
    std::shared_mutex mutex;

  public:
    explicit SharedConcurrentMap(size_t bucket_count);

    V &at(const K &key)
    {
        std::shared_lock lock(mutex);
        return map.at(key);
    }

    std::optional<V &> get(const K &key)
    {
        std::shared_lock lock(mutex);
        if (map.contains(key))
            return map.at(key);
        return std::nullopt;
    }

    bool contains(const K &key)
    {
        std::shared_lock lock(mutex);
        return map.contains(key);
    }

    void insert(const K &key, const V &value)
    {
        std::unique_lock lock(mutex);
        map.insert(key, value);
    }

    void erase(const K &key)
    {
        std::unique_lock lock(mutex);
        map.erase(key);
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

class ConcurrentByteLog
{
  private:
    std::shared_mutex mutex;
    std::atomic_int64_t size_ = 0;
    size_t capacity;
    uint8_t *buffer;
    uint8_t *standby_buffer;

    ConcurrentByteLog(const ConcurrentByteLog &other) = delete;
    ConcurrentByteLog(const ConcurrentByteLog &&other) = delete;
    ConcurrentByteLog &operator=(const ConcurrentByteLog &other) = delete;
    ConcurrentByteLog &operator=(const ConcurrentByteLog &&other) = delete;

  public:
    explicit ConcurrentByteLog(size_t capacity);
    ~ConcurrentByteLog();

    size_t append(const void *data, size_t data_size);
    size_t overwrite(size_t offset, const void *data, size_t data_size);
    bool get(void *dest, size_t offset, size_t data_size);
    size_t size();
    uint8_t *reset();
};

class LogManager
{

    std::shared_mutex log_mtx;
    ConcurrentByteLog active_meta_log;
    ConcurrentByteLog active_data_log;
};
