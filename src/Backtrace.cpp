#include "memleak/Backtrace.hpp"

#include <cstdlib>
#include <cstring>

// Backtrace support is provided by the <execinfo.h> family on glibc and on
// Apple platforms. We probe for it and fall back to a no-op implementation
// elsewhere so the library always builds and links.
#if defined(__has_include)
#  if __has_include(<execinfo.h>) && (defined(__GLIBC__) || defined(__APPLE__))
#    define MEMLEAK_HAVE_EXECINFO 1
#  endif
#endif

#if defined(MEMLEAK_HAVE_EXECINFO)
#  include <execinfo.h>
#endif

// Demangling is available through the Itanium C++ ABI on both libstdc++ and
// libc++. It is independent of the backtrace facility, so probe separately.
#if defined(__has_include)
#  if __has_include(<cxxabi.h>)
#    define MEMLEAK_HAVE_CXXABI 1
#  endif
#endif

#if defined(MEMLEAK_HAVE_CXXABI)
#  include <cxxabi.h>
#endif

namespace memleak {
namespace detail {

bool backtraceSupported() noexcept {
#if defined(MEMLEAK_HAVE_EXECINFO)
    return true;
#else
    return false;
#endif
}

std::size_t captureBacktrace(void** out, std::size_t max, std::size_t skip) noexcept {
#if defined(MEMLEAK_HAVE_EXECINFO)
    // Grab a little extra so we can discard the requested number of internal
    // frames without losing user-visible ones.
    constexpr std::size_t kScratch = 8;
    void* raw[kScratch + 64];
    std::size_t want = max + skip;
    if (want > kScratch + 64) {
        want = kScratch + 64;
    }
    int got = ::backtrace(raw, static_cast<int>(want));
    if (got <= 0) {
        return 0;
    }
    std::size_t n = static_cast<std::size_t>(got);
    if (skip >= n) {
        return 0;
    }
    std::size_t avail = n - skip;
    std::size_t count = avail < max ? avail : max;
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = raw[skip + i];
    }
    return count;
#else
    (void)out;
    (void)max;
    (void)skip;
    return 0;
#endif
}

#if defined(MEMLEAK_HAVE_CXXABI)
namespace {

// Extract the mangled symbol from a backtrace_symbols() line and demangle it.
// The line formats differ between glibc ("module(symbol+0xNN) [0xADDR]") and
// Apple ("N module 0xADDR symbol + NN"); handle both, returning empty on miss.
std::string demangleFromLine(const char* line) {
    std::string s(line);
    std::string mangled;

    std::size_t open = s.find('(');
    if (open != std::string::npos) {
        // glibc style.
        std::size_t plus = s.find('+', open);
        std::size_t end = (plus != std::string::npos) ? plus : s.find(')', open);
        if (end != std::string::npos && end > open + 1) {
            mangled = s.substr(open + 1, end - open - 1);
        }
    }

    if (mangled.empty()) {
        // Apple style: the symbol is the second-to-last whitespace token group
        // preceding "+ offset". Scan from the right for "+".
        std::size_t plus = s.rfind(" + ");
        if (plus != std::string::npos) {
            std::size_t symEnd = plus;
            std::size_t symStart = s.rfind(' ', symEnd - 1);
            if (symStart != std::string::npos && symEnd > symStart + 1) {
                mangled = s.substr(symStart + 1, symEnd - symStart - 1);
            }
        }
    }

    if (mangled.empty()) {
        return std::string();
    }

    int status = 0;
    char* demangled = abi::__cxa_demangle(mangled.c_str(), nullptr, nullptr, &status);
    if (status == 0 && demangled != nullptr) {
        std::string out(demangled);
        std::free(demangled);
        return out;
    }
    if (demangled != nullptr) {
        std::free(demangled);
    }
    // Not a mangled C++ name (e.g. a C symbol); return as-is.
    return mangled;
}

}  // namespace
#endif  // MEMLEAK_HAVE_CXXABI

std::string symbolizeFrame(void* frame) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llx",
                  static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(frame)));
    std::string addr(buf);

#if defined(MEMLEAK_HAVE_EXECINFO)
    void* arr[1] = {frame};
    char** lines = ::backtrace_symbols(arr, 1);
    if (lines != nullptr) {
        std::string line(lines[0] != nullptr ? lines[0] : "");
        std::free(lines);
#  if defined(MEMLEAK_HAVE_CXXABI)
        std::string name = demangleFromLine(line.c_str());
        if (!name.empty()) {
            return name + " [" + addr + "]";
        }
#  endif
        if (!line.empty()) {
            return line;
        }
    }
#endif

    return addr;
}

}  // namespace detail
}  // namespace memleak
