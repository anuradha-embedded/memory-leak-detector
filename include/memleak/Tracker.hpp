#ifndef MEMLEAK_TRACKER_HPP
#define MEMLEAK_TRACKER_HPP

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <mutex>
#include <string>
#include <vector>

namespace memleak {

// Maximum number of stack frames captured per allocation. Kept small so the
// per-record footprint stays bounded; the topmost frames are the interesting
// ones for pinpointing a leak's origin.
inline constexpr std::size_t kMaxFrames = 16;

// A single live allocation as seen by the tracker. The frame pointers are raw
// return addresses captured at allocation time; symbolization is deferred to
// reporting so that the allocation hot path stays cheap.
struct AllocationRecord {
    void*       address = nullptr;
    std::size_t size = 0;
    std::uint64_t serial = 0;            // monotonically increasing allocation id
    std::size_t frameCount = 0;
    void*       frames[kMaxFrames] = {};
};

// Aggregated view of everything still outstanding when report() runs.
struct LeakSummary {
    std::size_t   liveAllocations = 0;
    std::size_t   liveBytes = 0;
    std::uint64_t totalAllocations = 0;  // lifetime count, freed or not
    std::uint64_t totalFrees = 0;
};

// Thread-safe registry of outstanding heap allocations.
//
// The Tracker is a process-wide singleton. Once enabled (which the overridden
// global operators do lazily on first use) every operator new / new[] records
// the returned pointer, its size and a capture of the call site; every
// operator delete / delete[] removes the matching record. At program exit an
// installed handler prints any records that were never freed.
//
// The implementation is careful to allocate its own bookkeeping through an
// untracked path and to use a thread-local reentrancy guard, so the tracker
// never recurses into itself.
class Tracker {
public:
    static Tracker& instance() noexcept;

    // Turn recording on or off. Disabling does not drop existing records; it
    // only stops new events from being registered. Allocations made while
    // disabled are simply invisible to the tracker (and their later free is a
    // harmless no-op miss).
    void enable() noexcept;
    void disable() noexcept;
    bool enabled() const noexcept;

    // Hook points used by the global operator overloads.
    void recordAllocation(void* ptr, std::size_t size) noexcept;
    void recordDeallocation(void* ptr) noexcept;

    // Programmatic queries. Safe to call at any time.
    std::size_t liveAllocationCount() const noexcept;
    std::size_t liveBytes() const noexcept;
    LeakSummary summary() const noexcept;

    // Snapshot of the currently outstanding allocations, ordered by serial.
    std::vector<AllocationRecord> liveAllocations() const;

    // Print a human readable leak report to the given stream. Returns the
    // number of live allocations that were reported.
    std::size_t report(std::ostream& os) const;

    // Convenience: report() to std::cerr.
    std::size_t report() const;

    // Drop every record without touching the underlying memory. Intended for
    // tests that want a clean slate; not part of normal operation.
    void reset() noexcept;

    // Controls whether the at-exit handler emits a report. On by default.
    void setReportAtExit(bool on) noexcept;
    bool reportAtExit() const noexcept;

    // True when call-site capture is functional on this build/platform.
    static bool backtraceSupported() noexcept;

    Tracker(const Tracker&) = delete;
    Tracker& operator=(const Tracker&) = delete;

private:
    Tracker();
    ~Tracker();

    // The bucketed hash table holding live records. Defined in the .cpp so the
    // header stays free of implementation detail and untracked-allocator types.
    struct Registry;

    mutable std::mutex mutex_;
    Registry*          registry_;
    bool               enabled_;
    bool               reportAtExit_;
    std::uint64_t      serial_;
};

// RAII helper that enables the tracker for the lifetime of the object and
// restores the previous state afterwards. Handy for scoping detection to a
// region of interest in larger programs and tests.
class ScopedTracking {
public:
    ScopedTracking() noexcept;
    ~ScopedTracking() noexcept;

    ScopedTracking(const ScopedTracking&) = delete;
    ScopedTracking& operator=(const ScopedTracking&) = delete;

private:
    bool previous_;
};

}  // namespace memleak

#endif  // MEMLEAK_TRACKER_HPP
