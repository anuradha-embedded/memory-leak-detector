#include "memleak/Tracker.hpp"

#include "memleak/Backtrace.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <new>
#include <ostream>
#include <type_traits>

namespace memleak {

namespace {

// Reentrancy guard. While set, all bookkeeping allocations performed by the
// tracker (or by code it calls into, such as backtrace_symbols) are ignored by
// the operator overloads. Thread-local so concurrent threads each have their
// own guard. This is the primary defense against infinite recursion.
thread_local int g_inTracker = 0;

struct GuardScope {
    GuardScope() noexcept { ++g_inTracker; }
    ~GuardScope() noexcept { --g_inTracker; }
};

// Untracked allocation helpers. The registry must not route its own storage
// through operator new (that would recurse), so it goes straight to malloc/
// free. A tiny typed wrapper keeps call sites readable.
template <typename T>
T* untrackedAlloc(std::size_t count) {
    void* p = std::malloc(count * sizeof(T));
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return static_cast<T*>(p);
}

void untrackedFree(void* p) noexcept { std::free(p); }

}  // namespace

bool inTrackerScope() noexcept { return g_inTracker != 0; }

// Open-addressing hash table keyed by pointer, with all storage obtained from
// malloc so it is invisible to the tracker. Tombstone-free: we use linear
// probing with backward-shift deletion to keep clusters compact.
struct Tracker::Registry {
    struct Slot {
        AllocationRecord record;
        bool             occupied = false;
    };

    Slot*       slots = nullptr;
    std::size_t capacity = 0;
    std::size_t count = 0;
    std::size_t liveBytes = 0;

    Registry() { rehash(256); }

    ~Registry() {
        if (slots != nullptr) {
            untrackedFree(slots);
            slots = nullptr;
        }
    }

    static std::size_t hashPtr(void* p) noexcept {
        auto v = reinterpret_cast<std::uintptr_t>(p);
        // Fibonacci-style mixing; pointers are typically 16-byte aligned so the
        // low bits carry little entropy.
        v ^= v >> 33;
        v *= 0xff51afd7ed558ccdULL;
        v ^= v >> 33;
        return static_cast<std::size_t>(v);
    }

    void rehash(std::size_t newCap) {
        Slot* old = slots;
        std::size_t oldCap = capacity;

        Slot* fresh = untrackedAlloc<Slot>(newCap);
        for (std::size_t i = 0; i < newCap; ++i) {
            fresh[i] = Slot{};
        }
        slots = fresh;
        capacity = newCap;
        count = 0;
        liveBytes = 0;

        if (old != nullptr) {
            for (std::size_t i = 0; i < oldCap; ++i) {
                if (old[i].occupied) {
                    insert(old[i].record);
                }
            }
            untrackedFree(old);
        }
    }

    void insert(const AllocationRecord& rec) {
        if ((count + 1) * 4 >= capacity * 3) {  // keep load factor below 0.75
            rehash(capacity * 2);
        }
        std::size_t mask = capacity - 1;
        std::size_t idx = hashPtr(rec.address) & mask;
        while (slots[idx].occupied) {
            if (slots[idx].record.address == rec.address) {
                // Duplicate address (e.g. a missed free): overwrite, adjusting
                // the byte tally to reflect the latest size.
                liveBytes -= slots[idx].record.size;
                slots[idx].record = rec;
                liveBytes += rec.size;
                return;
            }
            idx = (idx + 1) & mask;
        }
        slots[idx].record = rec;
        slots[idx].occupied = true;
        ++count;
        liveBytes += rec.size;
    }

    bool erase(void* ptr) {
        if (capacity == 0) {
            return false;
        }
        std::size_t mask = capacity - 1;
        std::size_t idx = hashPtr(ptr) & mask;
        while (slots[idx].occupied) {
            if (slots[idx].record.address == ptr) {
                liveBytes -= slots[idx].record.size;
                slots[idx].occupied = false;
                --count;
                // Backward-shift deletion to preserve probe-sequence validity.
                std::size_t prev = idx;
                std::size_t cur = (idx + 1) & mask;
                while (slots[cur].occupied) {
                    std::size_t home = hashPtr(slots[cur].record.address) & mask;
                    // If `cur` can legally move back into `prev`, shift it.
                    bool movable;
                    if (prev <= cur) {
                        movable = !(prev < home && home <= cur);
                    } else {
                        movable = !(prev < home || home <= cur);
                    }
                    if (movable) {
                        slots[prev] = slots[cur];
                        slots[cur].occupied = false;
                        prev = cur;
                    }
                    cur = (cur + 1) & mask;
                }
                return true;
            }
            idx = (idx + 1) & mask;
        }
        return false;
    }
};

Tracker& Tracker::instance() noexcept {
    // The tracker must outlive every other object in the process, including
    // function-local and namespace-scope statics whose destructors free memory
    // during shutdown, and the atexit report handler. A normal static would be
    // destroyed partway through that sequence, leaving its mutex unusable (and
    // any late delete would touch freed bookkeeping). We therefore construct it
    // once into a statically-allocated, never-destroyed buffer. Construction is
    // guarded by a call_once so first use is thread-safe.
    static std::aligned_storage<sizeof(Tracker), alignof(Tracker)>::type storage;
    static std::once_flag once;
    static Tracker* self = nullptr;
    std::call_once(once, [] {
        self = new (&storage) Tracker();
    });
    return *self;
}

Tracker::Tracker()
    : registry_(nullptr), enabled_(false), reportAtExit_(true), serial_(0) {
    GuardScope guard;
    registry_ = untrackedAlloc<Registry>(1);
    new (registry_) Registry();
    enabled_ = true;

    // Emit the leak report when the program winds down. Registered once, here,
    // so it is in place as soon as the singleton exists. The handler is safe
    // because the singleton is never destroyed (see instance()).
    std::atexit([] {
        Tracker& self = Tracker::instance();
        if (!self.reportAtExit()) {
            return;
        }
        if (self.liveAllocationCount() == 0) {
            return;
        }
        self.report(std::cerr);
    });
}

Tracker::~Tracker() {
    // Never invoked in practice: the singleton lives for the whole process.
    // Defined for completeness and to keep the type's contract honest.
    GuardScope guard;
    if (registry_ != nullptr) {
        registry_->~Registry();
        untrackedFree(registry_);
        registry_ = nullptr;
    }
}

void Tracker::enable() noexcept { enabled_ = true; }
void Tracker::disable() noexcept { enabled_ = false; }
bool Tracker::enabled() const noexcept { return enabled_; }

void Tracker::setReportAtExit(bool on) noexcept { reportAtExit_ = on; }
bool Tracker::reportAtExit() const noexcept { return reportAtExit_; }

void Tracker::recordAllocation(void* ptr, std::size_t size) noexcept {
    if (ptr == nullptr || !enabled_ || g_inTracker != 0) {
        return;
    }
    GuardScope guard;  // protect the symbol/capture path below

    AllocationRecord rec;
    rec.address = ptr;
    rec.size = size;
    // Skip captureBacktrace itself plus this frame and the operator new frame.
    rec.frameCount = detail::captureBacktrace(rec.frames, kMaxFrames, 3);

    std::lock_guard<std::mutex> lock(mutex_);
    rec.serial = ++serial_;
    registry_->insert(rec);
}

void Tracker::recordDeallocation(void* ptr) noexcept {
    if (ptr == nullptr || g_inTracker != 0) {
        return;
    }
    GuardScope guard;
    std::lock_guard<std::mutex> lock(mutex_);
    registry_->erase(ptr);
}

std::size_t Tracker::liveAllocationCount() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return registry_->count;
}

std::size_t Tracker::liveBytes() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return registry_->liveBytes;
}

LeakSummary Tracker::summary() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    LeakSummary s;
    s.liveAllocations = registry_->count;
    s.liveBytes = registry_->liveBytes;
    s.totalAllocations = serial_;
    // Frees are not separately counted; derive freed = total - live for display
    // purposes in the report rather than storing redundant state.
    s.totalFrees = serial_ >= registry_->count
                       ? serial_ - registry_->count
                       : 0;
    return s;
}

std::vector<AllocationRecord> Tracker::liveAllocations() const {
    GuardScope guard;  // the vector below allocates; keep it untracked
    std::vector<AllocationRecord> out;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        out.reserve(registry_->count);
        for (std::size_t i = 0; i < registry_->capacity; ++i) {
            if (registry_->slots[i].occupied) {
                out.push_back(registry_->slots[i].record);
            }
        }
    }
    std::sort(out.begin(), out.end(),
              [](const AllocationRecord& a, const AllocationRecord& b) {
                  return a.serial < b.serial;
              });
    return out;
}

std::size_t Tracker::report(std::ostream& os) const {
    GuardScope guard;  // all the streaming/formatting below allocates
    std::vector<AllocationRecord> live = liveAllocations();
    LeakSummary s = summary();

    os << "================ memleak: leak report ================\n";
    if (live.empty()) {
        os << "No leaks detected. "
           << s.totalAllocations << " allocation(s) tracked, all freed.\n";
        os << "======================================================\n";
        return 0;
    }

    os << "Detected " << live.size() << " live allocation(s), "
       << s.liveBytes << " byte(s) leaked.\n";
    os << "(lifetime: " << s.totalAllocations << " tracked, "
       << s.totalFrees << " freed)\n";
    if (!detail::backtraceSupported()) {
        os << "[call-site capture unavailable on this platform]\n";
    }
    os << "------------------------------------------------------\n";

    std::size_t n = 0;
    for (const AllocationRecord& rec : live) {
        ++n;
        os << "#" << n << "  " << rec.size << " byte(s)"
           << "  @ " << rec.address
           << "  (alloc #" << rec.serial << ")\n";
        if (rec.frameCount == 0) {
            os << "      <no call site captured>\n";
        } else {
            for (std::size_t f = 0; f < rec.frameCount; ++f) {
                os << "      at " << detail::symbolizeFrame(rec.frames[f]) << "\n";
            }
        }
    }
    os << "======================================================\n";
    return live.size();
}

std::size_t Tracker::report() const { return report(std::cerr); }

bool Tracker::backtraceSupported() noexcept {
    return detail::backtraceSupported();
}

void Tracker::reset() noexcept {
    GuardScope guard;
    std::lock_guard<std::mutex> lock(mutex_);
    registry_->~Registry();
    new (registry_) Registry();
    serial_ = 0;
}

ScopedTracking::ScopedTracking() noexcept {
    previous_ = Tracker::instance().enabled();
    Tracker::instance().enable();
}

ScopedTracking::~ScopedTracking() noexcept {
    if (!previous_) {
        Tracker::instance().disable();
    }
}

}  // namespace memleak
