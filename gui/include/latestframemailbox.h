#pragma once

#include <memory>
#include <mutex>
#include <utility>

// One pending value, one scheduled consumer. Replacing a pending decoded
// picture is safe: the decoder itself still receives every compressed packet.
// The consumer must call take() once for every successful publish() wakeup.
template<class T>
class LatestFrameMailbox
{
public:
    template<class Merge>
    bool publish(std::shared_ptr<T> value, Merge merge)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_)
            merge(*value, *pending_);
        pending_.swap(value);
        if (scheduled_)
            return false;
        scheduled_ = true;
        return true;
    }

    std::shared_ptr<T> take()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        scheduled_ = false;
        return std::move(pending_);
    }

private:
    std::mutex mutex_;
    std::shared_ptr<T> pending_;
    bool scheduled_ = false;
};
