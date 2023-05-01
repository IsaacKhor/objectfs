#pragma once

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <shared_mutex>

/**
 * Concurrent std::map. Wraps std::map with mutexes
 */
template <typename K, typename V> class ConcurrentMap
{
  private:
    std::map<K, V> map;
    std::shared_mutex mutex;

  public:
    explicit ConcurrentMap(size_t bucket_count);

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
    explicit ConcurrentByteLog(size_t capacity) : capacity(capacity)
    {
        buffer = new uint8_t[capacity];
        standby_buffer = new uint8_t[capacity];
    }

    ~ConcurrentByteLog() { delete[] buffer; }

    size_t append(const void *data, size_t data_size)
    {
        if (data_size == 0)
            return 0;

        std::shared_lock lock(mutex);
        if (size_ + data_size > capacity)
            throw "Data exceeds log capacity";

        memcpy(buffer + size_, data, data_size);
        return size_.fetch_add(data_size);
    }

    size_t overwrite(size_t offset, const void *data, size_t data_size)
    {
        if (data_size == 0)
            return 0;

        std::unique_lock lock(mutex);
        if (offset + data_size > size_.load())
            throw "Trying to overwrite non-existent data";

        memcpy(buffer + offset, data, data_size);
        return offset;
    }

    bool get(void *dest, size_t offset, size_t data_size)
    {
        std::shared_lock lock(mutex);
        if (offset + data_size > size_.load())
            throw "Data doesn't exist in log: offset + data_size > size";
        memcpy(dest, buffer + offset, data_size);
        return true;
    }

    size_t size()
    {
        std::shared_lock lock(mutex);
        return size_.load();
    }

    uint8_t *reset()
    {
        auto dest_size = size_.load();
        uint8_t *dest = new uint8_t[dest_size];
        {
            std::unique_lock lock(mutex);
            std::swap(buffer, standby_buffer);
            size_.store(0);
        }
        memcpy(dest, standby_buffer, dest_size);
        return dest;
    }
};

/**
 * Why this exists: there was an atomic int that was incremented sometimes, and
 * decremented other times. I don't know what it does, but instead of replacing
 * it with a shared mutex lets temporarily use RAII to make it behave
 * correctly on all exit points
 */
class CounterGuard
{
  private:
    std::atomic<int> *counter;
    std::condition_variable *cv;

    CounterGuard(const CounterGuard &other) = delete;
    CounterGuard(const CounterGuard &&other) = delete;
    CounterGuard &operator=(const CounterGuard &other) = delete;
    CounterGuard &operator=(const CounterGuard &&other) = delete;

  public:
    CounterGuard(std::atomic<int> *counter, std::condition_variable *cv)
        : counter(counter), cv(cv)
    {
        counter->fetch_add(1, std::memory_order_seq_cst);
    }

    ~CounterGuard()
    {
        int old = counter->fetch_sub(1, std::memory_order_seq_cst);
        if (old == 1)
            cv->notify_all();
    }
};
