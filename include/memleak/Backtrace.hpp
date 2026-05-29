#ifndef MEMLEAK_BACKTRACE_HPP
#define MEMLEAK_BACKTRACE_HPP

#include <cstddef>
#include <string>

namespace memleak {
namespace detail {

// Capture up to `max` return addresses of the current call stack into `out`,
// skipping the first `skip` frames (which belong to the tracker itself).
// Returns the number of frames actually stored. On platforms without a usable
// backtrace facility this returns 0 and the caller degrades gracefully.
std::size_t captureBacktrace(void** out, std::size_t max, std::size_t skip) noexcept;

// True when captureBacktrace() can produce real frames on this build.
bool backtraceSupported() noexcept;

// Turn a single captured frame into a human readable string. When the platform
// supports symbolization the result contains the symbol name (demangled where
// possible); otherwise it falls back to the raw address. Never throws.
std::string symbolizeFrame(void* frame);

}  // namespace detail
}  // namespace memleak

#endif  // MEMLEAK_BACKTRACE_HPP
