// examples/track_workload.cpp
//
// A self-contained example that links against libmemleak and exercises a small
// "workload" with both balanced and leaking allocations. It shows the two ways
// a program typically interacts with the detector:
//
//   1. Query the tracker programmatically during execution.
//   2. Let the automatic at-exit handler print the final leak report.
//
// Build it after the library, for example with the plain Makefile:
//
//     make
//     c++ -std=c++17 -O2 -Iinclude examples/track_workload.cpp \
//         build-make/libmemleak.a -pthread -o track_workload
//     ./track_workload
//
// or with CMake, by adding it as another add_executable() target.

#include "memleak/Tracker.hpp"

#include <iostream>
#include <vector>

namespace {

// A cache that hands out node pointers and is supposed to free them on clear().
// The bug below: clear() forgets the last node, modelling a real-world leak.
struct NodeCache {
    std::vector<int*> nodes;

    int* acquire(int value) {
        int* n = new int(value);
        nodes.push_back(n);
        return n;
    }

    void clear() {
        if (nodes.empty()) {
            return;
        }
        // Off-by-one: stops one short, leaking the final node every time.
        for (std::size_t i = 0; i + 1 < nodes.size(); ++i) {
            delete nodes[i];
        }
        nodes.clear();
    }
};

}  // namespace

int main() {
    memleak::Tracker& tracker = memleak::Tracker::instance();

    NodeCache cache;
    for (int i = 0; i < 5; ++i) {
        cache.acquire(i * 10);
    }
    std::cout << "after acquiring 5 nodes: "
              << tracker.liveAllocationCount() << " live (cache buffer + nodes)\n";

    cache.clear();  // buggy clear leaks one node
    std::cout << "after buggy clear:       "
              << tracker.liveAllocationCount() << " live\n";

    std::cout << "\nThe at-exit handler will report the leaked node:\n";
    return 0;
}
