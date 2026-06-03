// Demonstration program for the memleak library.
//
// It performs a mix of well-behaved and deliberately leaky allocations, queries
// the tracker programmatically, and then lets the at-exit handler print the
// final report. Build and run it to see the detector in action:
//
//     ./memleak_demo
//
// The exit code is the number of live (leaked) allocations, capped at 125, so
// the leak state is observable from a shell script as well.

#include "memleak/Tracker.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

// A small object so the per-type leak sizes in the report differ visibly.
struct Widget {
    std::uint64_t id = 0;
    double        weight = 0.0;
    char          tag[16] = {};
};

// A fixed-size global table of raw pointers that the program populates but
// never tears down -- the classic "registered once, freed never" leak. It uses
// static storage (no heap buffer of its own), so the only allocations that leak
// are the objects the entries point at, and the compiler cannot elide them
// because they escape into a global with external visibility to the optimizer.
struct SessionTable {
    void*       entries[8] = {};
    std::size_t count = 0;

    void add(void* p) {
        if (count < (sizeof(entries) / sizeof(entries[0]))) {
            entries[count++] = p;
        }
    }
};

SessionTable& sessionTable() {
    static SessionTable table;
    return table;
}

// Allocates and correctly frees: contributes nothing to the leak total.
void wellBehaved() {
    auto* w = new Widget();
    w->id = 1;
    delete w;

    auto* arr = new int[64];
    for (int i = 0; i < 64; ++i) {
        arr[i] = i;
    }
    delete[] arr;

    // std::string / std::vector exercise the allocator through the library
    // containers, all of which clean up after themselves.
    std::vector<std::string> names;
    names.emplace_back("alpha");
    names.emplace_back("beta");
    names.emplace_back("gamma");
}

// Allocates without freeing: each call leaks one Widget and one int buffer by
// stashing them in the session table and then "forgetting" to release them.
void leakOnPurpose() {
    Widget* w = new Widget();
    w->id = 0xABCD;
    w->weight = 3.5;
    sessionTable().add(w);  // registered, never freed

    int* buffer = new int[32];
    buffer[0] = 7;
    sessionTable().add(buffer);  // registered, never freed
}

}  // namespace

int main() {
    memleak::Tracker& tracker = memleak::Tracker::instance();

    std::cout << "memleak demo: backtrace support is "
              << (memleak::Tracker::backtraceSupported() ? "enabled" : "disabled")
              << "\n";

    wellBehaved();
    std::cout << "after well-behaved phase: "
              << tracker.liveAllocationCount() << " live allocation(s), "
              << tracker.liveBytes() << " live byte(s)\n";

    // Leak a couple of allocations in two separate call sites so the report
    // shows distinct stacks.
    leakOnPurpose();
    leakOnPurpose();

    std::size_t live = tracker.liveAllocationCount();
    std::cout << "after leaking phase:      "
              << live << " live allocation(s), "
              << tracker.liveBytes() << " live byte(s)\n";

    std::cout << "\nExplicit report follows (the at-exit handler will print "
                 "another copy):\n";
    tracker.report(std::cout);

    return live > 125 ? 125 : static_cast<int>(live);
}
