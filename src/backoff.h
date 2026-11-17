// SPDX-License-Identifier: MIT
// Retry schedule and packet watchdog, as pure logic over millisecond tick values.
#pragma once
#include <cstdint>

namespace firstword {

class Backoff {
public:
    // A failed attempt at `now` schedules the next try after 1 s, 2 s, 5 s, then every 10 s.
    void Fail(std::uint64_t now) {
        static constexpr std::uint64_t steps[] = { 1000, 2000, 5000, 10000 };
        const unsigned index = attempts_ < 3 ? attempts_ : 3;
        due_ = now + steps[index];
        if (attempts_ < 1000000) ++attempts_;
    }
    void Reset() { attempts_ = 0; due_ = 0; }
    bool Due(std::uint64_t now) const { return now >= due_; }
    std::uint64_t MillisUntilDue(std::uint64_t now) const { return now >= due_ ? 0 : due_ - now; }
    unsigned Attempts() const { return attempts_; }

private:
    unsigned attempts_ = 0;
    std::uint64_t due_ = 0;
};

// Device-enumerator recovery: creating the enumerator is not proof of health; only a completed
// enumeration resets the schedule, so repeated "created, then enumeration failed" keeps escalating.
class EnumeratorRecovery {
public:
    void Created() {}
    void EnumerationSucceeded() { backoff_.Reset(); }
    void Failed(std::uint64_t now) { backoff_.Fail(now); }
    bool Due(std::uint64_t now) const { return backoff_.Due(now); }

private:
    Backoff backoff_;
};

// A stream is stale when no packet arrived for longer than `limit` milliseconds.
inline bool IsStale(std::uint64_t lastPacket, std::uint64_t now, std::uint64_t limit) {
    return now > lastPacket && now - lastPacket > limit;
}

} // namespace firstword
