// Copyright 2011 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef NINJA_UTIL_H_
#define NINJA_UTIL_H_

#ifdef _WIN32
#include "win32port.h"
#else
#include <stdint.h>
#endif

#include <limits.h>
#include <stdarg.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>
using namespace std;

#ifdef _MSC_VER
#define NORETURN __declspec(noreturn)
#else
#define NORETURN __attribute__((noreturn))
#endif

/// Log a fatal message and exit. This function should not be called from a
/// worker thread, because doing so would tend to produce interleaved output,
/// and it might destruct static globals that other threads are using.
NORETURN void Fatal(const char* msg, ...);

// Have a generic fall-through for different versions of C/C++.
#if defined(__cplusplus) && __cplusplus >= 201703L
#define NINJA_FALLTHROUGH [[fallthrough]]
#elif defined(__cplusplus) && __cplusplus >= 201103L && defined(__clang__)
#define NINJA_FALLTHROUGH [[clang::fallthrough]]
#elif defined(__cplusplus) && __cplusplus >= 201103L && defined(__GNUC__) && \
    __GNUC__ >= 7
#define NINJA_FALLTHROUGH [[gnu::fallthrough]]
#elif defined(__GNUC__) && __GNUC__ >= 7 // gcc 7
#define NINJA_FALLTHROUGH __attribute__ ((fallthrough))
#else // C++11 on gcc 6, and all other cases
#define NINJA_FALLTHROUGH
#endif

/// Log a warning message.
void Warning(const char* msg, ...);
void Warning(const char* msg, va_list ap);

/// Log an error message.
void Error(const char* msg, ...);
void Error(const char* msg, va_list ap);

/// Log an informational message.
void Info(const char* msg, ...);
void Info(const char* msg, va_list ap);

/// Canonicalize a path like "foo/../bar.h" into just "bar.h".
/// |slash_bits| has bits set starting from lowest for a backslash that was
/// normalized to a forward slash. (only used on Windows)
bool CanonicalizePath(string* path, uint64_t* slash_bits, string* err);
bool CanonicalizePath(char* path, size_t* len, uint64_t* slash_bits,
                      string* err);

/// Appends |input| to |*result|, escaping according to the whims of either
/// Bash, or Win32's CommandLineToArgvW().
/// Appends the string directly to |result| without modification if we can
/// determine that it contains no problematic characters.
void GetShellEscapedString(const string& input, string* result);
void GetWin32EscapedString(const string& input, string* result);

/// Read a file to a string (in text mode: with CRLF conversion
/// on Windows).
/// Returns -errno and fills in \a err on error.
int ReadFile(const string& path, string* contents, string* err);

#ifndef _WIN32
class Mapping {
public:
  Mapping(char* base, size_t file_size, size_t mapping_size)
      : base_(base), file_size_(file_size), mapping_size_(mapping_size) {}
  ~Mapping() { unmap(); }
  void unmap();

  Mapping(const Mapping&) = delete;
  Mapping& operator=(const Mapping&) = delete;

  char* base() const { return base_; }

  /// This size does not include the extra NUL at the end.
  size_t file_size() const { return file_size_; }

  /// The size of the entire mapping, including the extra zero page at the end.
  size_t mapping_size() const { return mapping_size_; }

private:
  char* base_ = nullptr;
  size_t file_size_ = 0;
  size_t mapping_size_ = 0;
};

/// Map the file into memory as read-only. The file is followed by a zero page,
/// and the first byte after the file's content is guaranteed to be NUL. The
/// lexer relies on this property to efficiently detect the end of input.
int MapFile(const std::string& filename,
            std::unique_ptr<Mapping>* result,
            std::string* err);
#endif  // ! _WIN32

/// Mark a file descriptor to not be inherited on exec()s.
void SetCloseOnExec(int fd);

/// Given a misspelled string and a list of correct spellings, returns
/// the closest match or NULL if there is no close enough match.
const char* SpellcheckStringV(const string& text,
                              const vector<const char*>& words);

/// Like SpellcheckStringV, but takes a NULL-terminated list.
const char* SpellcheckString(const char* text, ...);

bool islatinalpha(int c);

/// Removes all Ansi escape codes (http://www.termsys.demon.co.uk/vtansi.htm).
string StripAnsiEscapeCodes(const string& in);

/// @return the number of processors on the machine.  Useful for an initial
/// guess for how many jobs to run in parallel.  @return 0 on error.
int GetProcessorCount();

/// @return the load average of the machine. A negative value is returned
/// on error.
double GetLoadAverage();

/// Elide the given string @a str with '...' in the middle if the length
/// exceeds @a width.
string ElideMiddle(const string& str, size_t width);

/// Truncates a file to the given size.
bool Truncate(const string& path, size_t size, string* err);

#ifdef _MSC_VER
#define snprintf _snprintf
#define fileno _fileno
#define unlink _unlink
#define chdir _chdir
#define strtoull _strtoui64
#define getcwd _getcwd
#define PATH_MAX _MAX_PATH
#endif

#ifdef _WIN32
/// Convert the value returned by GetLastError() into a string.
string GetLastErrorString();

/// Calls Fatal() with a function name and GetLastErrorString.
NORETURN void Win32Fatal(const char* function, const char* hint = NULL);

#define NINJA_WIN32_CD_DELIM ":NINJA_WIN32_CD:"
#endif

template <typename T, typename U>
decltype(T() + U()) RoundUp(T x, U y) {
  return (x + (y - 1)) / y * y;
}

template <typename T>
std::vector<std::pair<T, T>> SplitByChunkSize(T total, T chunk_size) {
  std::vector<std::pair<T, T>> result;
  result.reserve(total / chunk_size + 1);
  T start = 0;
  while (start < total) {
    T finish = std::min<T>(start + chunk_size, total);
    result.emplace_back(start, finish);
    start = finish;
  }
  return result;
}

template <typename T, typename C>
std::vector<std::pair<T, T>> SplitByCount(T total, C count) {
  T chunk_size = std::max<T>(1, RoundUp(total, count) / count);
  return SplitByChunkSize(total, chunk_size);
}

template <typename T>
struct IntegralRange {
  IntegralRange(T start, T stop) : start_(start), stop_(stop) {}
  size_t size() const { return (stop_ >= start_) ? (stop_ - start_) : 0; }
  T operator[](size_t idx) const  { return start_ + idx; }
private:
  T start_;
  T stop_;
};

/// Atomically set |*dest| to the lesser of its current value and the given
/// |candidate| value, using the given less-than comparator.
template <typename T, typename LessThan=std::less<T>>
T AtomicUpdateMinimum(std::atomic<T>* dest, T candidate,
                      LessThan less_than=LessThan()) {
  T current = dest->load();
  while (true) {
    if (!less_than(candidate, current)) {
      return current;
    }
    if (dest->compare_exchange_weak(current, candidate)) {
      return candidate;
    }
  }
}

/// Atomically set |*dest| to the greater of its current value and the given
/// |candidate| value, using the given less-than comparator.
template <typename T, typename LessThan=std::less<T>>
T AtomicUpdateMaximum(std::atomic<T>* dest, T candidate,
                      LessThan less_than=LessThan()) {
  // The C++ standard library (e.g. std::max) orders values using operator<
  // rather than operator>, so flip the argument order with a lambda rather than
  // take a greater-than functor argument.
  return AtomicUpdateMinimum(dest, candidate, [&](const T& x, const T& y) {
    return less_than(y, x);
  });
}

/// If any string in the given vector is non-empty, copy it to |*err| and return
/// false. Otherwise return true.
bool PropagateError(std::string* err,
                    const std::vector<std::string>& subtask_err);

/// Assign each thread a "slot" within a fixed number of slots. The number of
/// slots, and each thread's slot index, is fixed until the program exits.
size_t GetThreadSlotCount();
size_t GetThreadSlotIndex();

/// False sharing between CPU cores can have a measurable effect on ninja
/// performance. Avoid it by overaligning some frequently-accessed data
/// structures, which ensures that different copies of a struct are on different
/// cache lines. A typical L1 cache line size is 64 bytes. Perhaps this code
/// could eventually use std::hardware_destructive_interference_size from C++17,
/// but that constant is currently missing from libc++.
///
/// This overalignment is only done with C++17 to avoid confusion. In previous
/// C++ versions, operator new didn't support overalignment. (See WG21 paper
/// P0035R4.)
#if __cplusplus >= 201703L
#define NINJA_ALIGNAS_CACHE_LINE alignas(64)
#else
#define NINJA_ALIGNAS_CACHE_LINE
#endif

#endif  // NINJA_UTIL_H_
