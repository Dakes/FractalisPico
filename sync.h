#ifndef SYNC_H
#define SYNC_H

// Small mutex wrapper, so the calculation code also builds on a desktop machine (for testing)
#if PICO_ON_DEVICE
#include "pico/mutex.h"

class Lock {
public:
    Lock() { mutex_init(&mutex); }
    void lock() { mutex_enter_blocking(&mutex); }
    void unlock() { mutex_exit(&mutex); }
private:
    mutex_t mutex;
};
#else
#include <mutex>

class Lock {
public:
    void lock() { mutex.lock(); }
    void unlock() { mutex.unlock(); }
private:
    std::mutex mutex;
};
#endif

class LockGuard {
public:
    explicit LockGuard(Lock& lock) : lock(lock) { lock.lock(); }
    ~LockGuard() { lock.unlock(); }
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;
private:
    Lock& lock;
};

#endif // SYNC_H
