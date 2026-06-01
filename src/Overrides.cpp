#include "memleak/Tracker.hpp"

#include <cstdlib>
#include <new>

// Global allocation function overrides.
//
// Every form mandated by the C++17 standard library is provided so that the
// tracker observes all heap traffic regardless of which overload the compiler
// or standard library selects:
//
//   * throwing and nothrow operator new / new[]
//   * plain operator delete / delete[]
//   * nothrow operator delete / delete[]
//   * sized operator delete / delete[]  (C++14/17)
//
// The placement forms (operator new(size, void*)) are intentionally NOT
// overridden: they do not allocate, so there is nothing to track, and
// replacing them is ill-formed.
//
// Each allocating function uses std::malloc as the backing store and each
// deallocating function uses std::free, keeping the pair symmetric. Recording
// is delegated to the Tracker, which internally guards against recursion.

namespace {

// Shared allocation path. Throws std::bad_alloc on failure for the throwing
// overloads; the nothrow overloads catch and translate to nullptr.
void* doAllocate(std::size_t size) {
    // operator new must return a unique, non-null pointer even for size 0.
    std::size_t request = size != 0 ? size : 1;
    void* p = std::malloc(request);
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    memleak::Tracker::instance().recordAllocation(p, size);
    return p;
}

void doDeallocate(void* p) noexcept {
    if (p == nullptr) {
        return;
    }
    memleak::Tracker::instance().recordDeallocation(p);
    std::free(p);
}

}  // namespace

// ---- operator new ---------------------------------------------------------

void* operator new(std::size_t size) {
    return doAllocate(size);
}

void* operator new[](std::size_t size) {
    return doAllocate(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return doAllocate(size);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return doAllocate(size);
    } catch (...) {
        return nullptr;
    }
}

// ---- operator delete ------------------------------------------------------

void operator delete(void* p) noexcept {
    doDeallocate(p);
}

void operator delete[](void* p) noexcept {
    doDeallocate(p);
}

void operator delete(void* p, const std::nothrow_t&) noexcept {
    doDeallocate(p);
}

void operator delete[](void* p, const std::nothrow_t&) noexcept {
    doDeallocate(p);
}

// Sized deallocation (C++14, mandated for replacement in C++17). The size is
// advisory for our purposes since the registry already knows the recorded
// size; we ignore it and free as usual.
void operator delete(void* p, std::size_t) noexcept {
    doDeallocate(p);
}

void operator delete[](void* p, std::size_t) noexcept {
    doDeallocate(p);
}
