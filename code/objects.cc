#include "objects.h"
#include <fstream>
#include <vector>

template <typename T>
BlockingMPMC<T>::BlockingMPMC(size_t size) : _buffer(), _max_capacity(size)
{
}

template <typename T> BlockingMPMC<T>::~BlockingMPMC() {}

template <typename T> void BlockingMPMC<T>::push(T t)
{
    {
        std::unique_lock<std::mutex> lck(_mutex);
        _can_push.wait(lck, [&]() { return _buffer.size() < _max_capacity; });
        _buffer.push(std::move(t));
    }
    _can_pop.notify_one();
}

template <typename T> T BlockingMPMC<T>::pop()
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

ConcurrentByteLog::ConcurrentByteLog(size_t capacity)
    : capacity(capacity), size_(0)
{
    buffer = new uint8_t[capacity];
    standby_buffer = new uint8_t[capacity];
}

ConcurrentByteLog::~ConcurrentByteLog()
{
    delete[] buffer;
    delete[] standby_buffer;
}

size_t ConcurrentByteLog::append(const void *data, size_t data_size)
{
    if (data_size == 0)
        return 0;

    std::shared_lock lock(mutex);
    if (size_ + data_size > capacity)
        throw "Data exceeds log capacity";

    memcpy(buffer + size_, data, data_size);
    return size_.fetch_add(data_size);
}

size_t ConcurrentByteLog::overwrite(size_t offset, const void *data,
                                    size_t data_size)
{
    if (data_size == 0)
        return 0;

    std::unique_lock lock(mutex);
    if (offset + data_size > size_.load())
        throw "Trying to overwrite non-existent data";

    memcpy(buffer + offset, data, data_size);
    return offset;
}

bool ConcurrentByteLog::get(void *dest, size_t offset, size_t data_size)
{
    std::shared_lock lock(mutex);
    if (offset + data_size > size_.load())
        throw "Data doesn't exist in log: offset + data_size > size";
    memcpy(dest, buffer + offset, data_size);
    return true;
}

size_t ConcurrentByteLog::size()
{
    std::shared_lock lock(mutex);
    return size_.load();
}

uint8_t *ConcurrentByteLog::reset()
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
