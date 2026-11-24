#include "latestframemailbox.h"
#include <cassert>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <iostream>

struct Frame { int id; int skipped; };
static std::shared_ptr<Frame> frame(int id) {
    return std::make_shared<Frame>(Frame{id, 0});
}
static void merge(Frame &next, const Frame &old) { next.skipped += old.skipped + 1; }

int main() {
    LatestFrameMailbox<Frame> mailbox;
    assert(!mailbox.take());
    auto first = frame(1);
    std::weak_ptr<Frame> old = first;
    assert(mailbox.publish(first, merge));
    first.reset();
    assert(!mailbox.publish(frame(2), merge));
    assert(old.expired());
    assert(!mailbox.publish(frame(3), merge));
    auto latest = mailbox.take();
    assert(latest && latest->id == 3 && latest->skipped == 2);
    assert(!mailbox.take());
    assert(mailbox.publish(frame(4), merge));
    assert(mailbox.take()->id == 4);

    // Real producer and consumer, modelling the one queued GUI event.
    LatestFrameMailbox<Frame> concurrent;
    std::mutex events_mutex;
    std::condition_variable ready;
    bool event = false, done = false;
    const int count = 100000;
    std::thread producer([&] {
        for (int i=1; i<=count; ++i) {
            if (concurrent.publish(frame(i), merge)) {
                std::lock_guard<std::mutex> lock(events_mutex);
                assert(!event);
                event = true;
                ready.notify_one();
            }
        }
        std::lock_guard<std::mutex> lock(events_mutex);
        done = true;
        ready.notify_one();
    });
    int last = 0, consumed = 0, skipped = 0;
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(events_mutex);
            ready.wait(lock, [&] { return event || done; });
            if (!event && done) break;
            event = false;
        }
        auto value = concurrent.take();
        assert(value && value->id > last);
        assert(value->id - last == value->skipped + 1);
        last = value->id;
        skipped += value->skipped;
        ++consumed;
    }
    producer.join();
    assert(last == count);
    assert(consumed + skipped == count);
    assert(!concurrent.take());
    std::cout << "PASS: ownership, replacement, rearming, 100000 concurrent publications; "
              << consumed << " delivered, " << skipped << " superseded\n";
}
