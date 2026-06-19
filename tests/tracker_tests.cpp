// Self-contained correctness tests for the memleak tracker.
//
// These use <cassert> only and query the tracker's API directly rather than
// parsing any printed output. Each test isolates its measurement window with
// reset() so prior activity (and the test harness's own allocations) does not
// perturb the counts.
//
// Run via ctest, or directly: it returns 0 on success and aborts on failure.

#include "memleak/Tracker.hpp"

// A test suite whose checks are compiled out in Release would be worthless, so
// force assertions to stay live regardless of the build's NDEBUG setting.
#ifdef NDEBUG
#  undef NDEBUG
#endif
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <new>
#include <vector>

namespace {

memleak::Tracker& T() { return memleak::Tracker::instance(); }

// Force an allocation to be observable so the optimizer cannot elide the
// new/delete pair. The C++14/17 standard explicitly permits a compiler to
// remove a new/delete pair whose result is otherwise unused (N3664), which
// would stop the operator overloads from ever running. Merely stashing the
// pointer is not enough -- the value can be materialized without a real
// allocation. Touching the allocated storage through a volatile lvalue marks
// the object as live and is portable across clang and g++ at any -O level.
template <typename T>
T* keep(T* p) {
    if (p != nullptr) {
        volatile unsigned char* probe = reinterpret_cast<volatile unsigned char*>(p);
        *probe = static_cast<unsigned char>(*probe + 1);
    }
    return p;
}

// The tracker must be enabled and its at-exit report suppressed; otherwise the
// allocations we intentionally leak below would print on shutdown and the test
// would appear noisy (and on some CIs, fail a "clean stderr" expectation).
void prepare() {
    T().setReportAtExit(false);
    T().enable();
    T().reset();
}

// Verifies that a balanced sequence of allocations and frees leaves the tracker
// reporting zero live allocations and zero live bytes.
void test_balanced_leaves_nothing_live() {
    prepare();

    // Raw scalar new/delete.
    int* a = keep(new int(42));
    assert(T().liveAllocationCount() == 1);
    delete a;

    // Array new/delete.
    double* arr = keep(new double[10]);
    assert(T().liveAllocationCount() == 1);
    delete[] arr;

    // Nothrow new + plain delete (matching deallocation).
    char* c = keep(new (std::nothrow) char[256]);
    assert(c != nullptr);
    assert(T().liveAllocationCount() == 1);
    delete[] c;

    assert(T().liveAllocationCount() == 0);
    assert(T().liveBytes() == 0);

    memleak::LeakSummary s = T().summary();
    assert(s.liveAllocations == 0);
    assert(s.liveBytes == 0);
    assert(s.totalAllocations == 3);  // three allocations seen in this window

    std::cout << "[ok] balanced allocate/free leaves nothing live\n";
}

// Leaks a precisely known number of bytes across a known number of allocations
// and asserts the tracker reports both exactly.
void test_known_leak_is_counted_exactly() {
    prepare();

    // Allocation 1: a 64-element int array. operator new[] is asked for at
    // least 64 * sizeof(int) bytes; the recorded size is exactly that request.
    const std::size_t intCount = 64;
    const std::size_t intBytes = intCount * sizeof(int);
    int* leak1 = keep(new int[intCount]);
    leak1[0] = 1;  // touch to defeat any dead-store elimination

    // Allocation 2: a single scalar of a fixed-size POD.
    struct Pod {
        std::uint64_t a;
        std::uint64_t b;
    };
    const std::size_t podBytes = sizeof(Pod);
    Pod* leak2 = keep(new Pod{1, 2});

    // Allocation 3: a raw byte buffer of a chosen size.
    const std::size_t rawBytes = 1000;
    char* leak3 = keep(new char[rawBytes]);
    leak3[0] = 'x';

    const std::size_t expectedCount = 3;
    const std::size_t expectedBytes = intBytes + podBytes + rawBytes;

    assert(T().liveAllocationCount() == expectedCount);
    assert(T().liveBytes() == expectedBytes);

    memleak::LeakSummary s = T().summary();
    assert(s.liveAllocations == expectedCount);
    assert(s.liveBytes == expectedBytes);

    // The snapshot must contain exactly the leaked records with their sizes.
    std::vector<memleak::AllocationRecord> recs = T().liveAllocations();
    assert(recs.size() == expectedCount);
    std::size_t summed = 0;
    for (const auto& r : recs) {
        summed += r.size;
        assert(r.address != nullptr);
        assert(r.serial != 0);
    }
    assert(summed == expectedBytes);

    // Records come back ordered by serial.
    for (std::size_t i = 1; i < recs.size(); ++i) {
        assert(recs[i - 1].serial < recs[i].serial);
    }

    std::cout << "[ok] known leak counted exactly: " << expectedCount
              << " allocation(s), " << expectedBytes << " byte(s)\n";

    // Clean up so we do not actually leak from the test process and so the next
    // test starts from a known baseline.
    delete[] leak1;
    delete leak2;
    delete[] leak3;
    assert(T().liveAllocationCount() == 0);
}

// Confirms that a freed allocation drops out of the live set and that freeing a
// pointer the tracker never saw is a harmless no-op.
void test_free_removes_and_unknown_free_is_noop() {
    prepare();

    int* p = keep(new int[5]);
    assert(T().liveAllocationCount() == 1);
    delete[] p;
    assert(T().liveAllocationCount() == 0);

    // Allocate while disabled: the tracker should not see it, so the matching
    // free is an unknown-pointer no-op that must not corrupt counts.
    T().disable();
    int* hidden = keep(new int(7));
    T().enable();
    assert(T().liveAllocationCount() == 0);
    delete hidden;  // unknown to the tracker -> no-op record-wise
    assert(T().liveAllocationCount() == 0);
    assert(T().liveBytes() == 0);

    std::cout << "[ok] free removes records; unknown free is a no-op\n";
}

// Exercises a larger churn to force the registry to rehash, then drains it to
// verify the table stays consistent through growth and backward-shift deletes.
//
// The bookkeeping vector itself allocates through the tracked path, so we
// pre-size it (one buffer allocation) and measure live counts as deltas from a
// captured baseline rather than absolute values.
void test_registry_growth_and_drain() {
    prepare();

    const std::size_t n = 5000;
    std::vector<int*> ptrs;
    ptrs.reserve(n);  // single tracked buffer allocation for the vector

    const std::size_t baseline = T().liveAllocationCount();

    for (std::size_t i = 0; i < n; ++i) {
        ptrs.push_back(new int(static_cast<int>(i)));
    }
    assert(T().liveAllocationCount() == baseline + n);

    // Free in a non-sequential pattern to stress the deletion shifting.
    for (std::size_t i = 0; i < n; i += 2) {
        delete ptrs[i];
        ptrs[i] = nullptr;
    }
    assert(T().liveAllocationCount() == baseline + n / 2);

    for (std::size_t i = 1; i < n; i += 2) {
        delete ptrs[i];
        ptrs[i] = nullptr;
    }
    assert(T().liveAllocationCount() == baseline);

    std::cout << "[ok] registry survives growth and full drain of " << n
              << " allocations\n";
}

}  // namespace

int main() {
    test_balanced_leaves_nothing_live();
    test_known_leak_is_counted_exactly();
    test_free_removes_and_unknown_free_is_noop();
    test_registry_growth_and_drain();

    // Leave a clean slate so nothing prints at exit.
    T().reset();
    std::cout << "all tests passed\n";
    return 0;
}
