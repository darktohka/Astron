#pragma once
#include <atomic>
#include <memory>
#include <mutex>

// A LifetimeGuard is a reference-counted liveness token that makes deferred
// (potentially cross-thread) callbacks safe against their target being
// destroyed before the callback runs.
//
// The target object owns a std::shared_ptr<LifetimeGuard>. A callback that may
// outlive the target captures a std::shared_ptr<LifetimeGuard> copy. When the
// callback runs it must lock `mutex`, check `alive`, and only then touch the
// target. The target calls invalidate() (which also takes `mutex`) immediately
// before it is destroyed.
//
// Thanks Rust.
class LifetimeGuard
{
  public:
    std::recursive_mutex mutex;
    std::atomic<bool> alive{true};

    inline void invalidate()
    {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        alive.store(false);
    }
};

using LifetimeGuardPtr = std::shared_ptr<LifetimeGuard>;
