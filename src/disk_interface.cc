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

#include "disk_interface.h"

#include <algorithm>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <sstream>
#include <windows.h>
#include <direct.h>  // _mkdir, chdir, getcwd
#else
#include <atomic>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>  // chdir, getcwd
#include <vector>
#include <zstd.h>
#endif

#include "metrics.h"
#include "thread_pool.h"
#include "util.h"

namespace {

string DirName(const string& path) {
#ifdef _WIN32
  static const char kPathSeparators[] = "\\/";
#else
  static const char kPathSeparators[] = "/";
#endif
  static const char* const kEnd = kPathSeparators + sizeof(kPathSeparators) - 1;

  string::size_type slash_pos = path.find_last_of(kPathSeparators);
  if (slash_pos == string::npos)
    return string();  // Nothing to do.
  while (slash_pos > 0 &&
         std::find(kPathSeparators, kEnd, path[slash_pos - 1]) != kEnd)
    --slash_pos;
  return path.substr(0, slash_pos);
}

int MakeDir(const string& path) {
#ifdef _WIN32
  return _mkdir(path.c_str());
#else
  return mkdir(path.c_str(), 0777);
#endif
}

#ifdef _WIN32
TimeStamp TimeStampFromFileTime(const FILETIME& filetime) {
  // FILETIME is in 100-nanosecond increments since the Windows epoch.
  // We don't much care about epoch correctness but we do want the
  // resulting value to fit in a 64-bit integer.
  uint64_t mtime = ((uint64_t)filetime.dwHighDateTime << 32) |
    ((uint64_t)filetime.dwLowDateTime);
  // 1600 epoch -> 2000 epoch (subtract 400 years).
  return (TimeStamp)mtime - 12622770400LL * (1000000000LL / 100);
}

TimeStamp StatSingleFile(const string& path, string* err) {
  WIN32_FILE_ATTRIBUTE_DATA attrs;
  if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &attrs)) {
    DWORD win_err = GetLastError();
    if (win_err == ERROR_FILE_NOT_FOUND || win_err == ERROR_PATH_NOT_FOUND)
      return 0;
    *err = "GetFileAttributesEx(" + path + "): " + GetLastErrorString();
    return -1;
  }
  return TimeStampFromFileTime(attrs.ftLastWriteTime);
}

bool IsWindows7OrLater() {
  OSVERSIONINFOEX version_info =
      { sizeof(OSVERSIONINFOEX), 6, 1, 0, 0, {0}, 0, 0, 0, 0, 0};
  DWORDLONG comparison = 0;
  VER_SET_CONDITION(comparison, VER_MAJORVERSION, VER_GREATER_EQUAL);
  VER_SET_CONDITION(comparison, VER_MINORVERSION, VER_GREATER_EQUAL);
  return VerifyVersionInfo(
      &version_info, VER_MAJORVERSION | VER_MINORVERSION, comparison);
}

bool StatAllFilesInDir(const string& dir, map<string, TimeStamp>* stamps,
                       string* err) {
  // FindExInfoBasic is 30% faster than FindExInfoStandard.
  static bool can_use_basic_info = IsWindows7OrLater();
  // This is not in earlier SDKs.
  const FINDEX_INFO_LEVELS kFindExInfoBasic =
      static_cast<FINDEX_INFO_LEVELS>(1);
  FINDEX_INFO_LEVELS level =
      can_use_basic_info ? kFindExInfoBasic : FindExInfoStandard;
  WIN32_FIND_DATAA ffd;
  HANDLE find_handle = FindFirstFileExA((dir + "\\*").c_str(), level, &ffd,
                                        FindExSearchNameMatch, NULL, 0);

  if (find_handle == INVALID_HANDLE_VALUE) {
    DWORD win_err = GetLastError();
    if (win_err == ERROR_FILE_NOT_FOUND || win_err == ERROR_PATH_NOT_FOUND)
      return true;
    *err = "FindFirstFileExA(" + dir + "): " + GetLastErrorString();
    return false;
  }
  do {
    string lowername = ffd.cFileName;
    if (lowername == "..") {
      // Seems to just copy the timestamp for ".." from ".", which is wrong.
      // This is the case at least on NTFS under Windows 7.
      continue;
    }
    transform(lowername.begin(), lowername.end(), lowername.begin(), ::tolower);
    stamps->insert(make_pair(lowername,
                             TimeStampFromFileTime(ffd.ftLastWriteTime)));
  } while (FindNextFileA(find_handle, &ffd));
  FindClose(find_handle);
  return true;
}
#endif  // _WIN32

}  // namespace

// DiskInterface ---------------------------------------------------------------

bool DiskInterface::MakeDirs(const string& path) {
  string dir = DirName(path);
  if (dir.empty())
    return true;  // Reached root; assume it's there.
  string err;
  TimeStamp mtime = Stat(dir, &err);
  if (mtime < 0) {
    Error("%s", err.c_str());
    return false;
  }
  if (mtime > 0)
    return true;  // Exists already; we're done.

  // Directory doesn't exist.  Try creating its parent first.
  bool success = MakeDirs(dir);
  if (!success)
    return false;
  return MakeDir(dir);
}

// RealDiskInterface -----------------------------------------------------------

#ifndef _WIN32
static TimeStamp StatTimestamp(const struct stat& st) {
  // Some users (Flatpak) set mtime to 0, this should be harmless
  // and avoids conflicting with our return value of 0 meaning
  // that it doesn't exist.
  if (st.st_mtime == 0)
    return 1;
#if defined(_AIX)
  return (int64_t)st.st_mtime * 1000000000LL + st.st_mtime_n;
#elif defined(__APPLE__)
  return ((int64_t)st.st_mtimespec.tv_sec * 1000000000LL +
          st.st_mtimespec.tv_nsec);
#elif defined(st_mtime) // A macro, so we're likely on modern POSIX.
  return (int64_t)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#else
  return (int64_t)st.st_mtime * 1000000000LL + st.st_mtimensec;
#endif
}
#endif

/// This function is thread-safe on Unix but not on Windows. See
/// RealDiskInterface::IsStatThreadSafe.
TimeStamp RealDiskInterface::Stat(const string& path, string* err) const {
  METRIC_RECORD("node stat");
#ifdef _WIN32
  // MSDN: "Naming Files, Paths, and Namespaces"
  // http://msdn.microsoft.com/en-us/library/windows/desktop/aa365247(v=vs.85).aspx
  if (!path.empty() && path[0] != '\\' && path.size() > MAX_PATH) {
    ostringstream err_stream;
    err_stream << "Stat(" << path << "): Filename longer than " << MAX_PATH
               << " characters";
    *err = err_stream.str();
    return -1;
  }
  if (!use_cache_)
    return StatSingleFile(path, err);

  string dir = DirName(path);
  string base(path.substr(dir.size() ? dir.size() + 1 : 0));
  if (base == "..") {
    // StatAllFilesInDir does not report any information for base = "..".
    base = ".";
    dir = path;
  }

  transform(dir.begin(), dir.end(), dir.begin(), ::tolower);
  transform(base.begin(), base.end(), base.begin(), ::tolower);

  Cache::iterator ci = cache_.find(dir);
  if (ci == cache_.end()) {
    ci = cache_.insert(make_pair(dir, DirCache())).first;
    if (!StatAllFilesInDir(dir.empty() ? "." : dir, &ci->second, err)) {
      cache_.erase(ci);
      return -1;
    }
  }
  DirCache::iterator di = ci->second.find(base);
  return di != ci->second.end() ? di->second : 0;
#else
  struct stat st;
  if (stat(path.c_str(), &st) < 0) {
    if (errno == ENOENT || errno == ENOTDIR)
      return 0;
    *err = "stat(" + path + "): " + strerror(errno);
    return -1;
  }
  return StatTimestamp(st);
#endif
}

TimeStamp RealDiskInterface::LStat(
  const string& path, bool* is_dir, bool* is_symlink, string* err) const {
  METRIC_RECORD("node lstat");
#ifdef _WIN32
#error unimplemented
#else
  struct stat st;
  if (lstat(path.c_str(), &st) < 0) {
    if (errno == ENOENT || errno == ENOTDIR)
      return 0;
    *err = "lstat(" + path + "): " + strerror(errno);
    return -1;
  }
  if (is_dir != nullptr) {
    *is_dir = S_ISDIR(st.st_mode);
  }

  if (is_symlink != nullptr) {
    *is_symlink = S_ISLNK(st.st_mode);
  }
  return StatTimestamp(st);
#endif
}

bool RealDiskInterface::IsStatThreadSafe() const {
#ifdef _WIN32
  return false;
#else
  return true;
#endif
}

bool RealDiskInterface::WriteFile(const string& path, const string& contents) {
  FILE* fp = fopen(path.c_str(), "w");
  if (fp == NULL) {
    Error("WriteFile(%s): Unable to create file. %s",
          path.c_str(), strerror(errno));
    return false;
  }

  if (fwrite(contents.data(), 1, contents.length(), fp) < contents.length())  {
    Error("WriteFile(%s): Unable to write to the file. %s",
          path.c_str(), strerror(errno));
    fclose(fp);
    return false;
  }

  if (fclose(fp) == EOF) {
    Error("WriteFile(%s): Unable to close the file. %s",
          path.c_str(), strerror(errno));
    return false;
  }

  return true;
}

bool RealDiskInterface::MakeDir(const string& path) {
  if (::MakeDir(path) < 0) {
    if (errno == EEXIST) {
      return true;
    }
    Error("mkdir(%s): %s", path.c_str(), strerror(errno));
    return false;
  }
  return true;
}

FileReader::Status RealDiskInterface::ReadFile(const string& path,
                                               string* contents,
                                               string* err) {
  switch (::ReadFile(path, contents, err)) {
  case 0:       return Okay;
  case -ENOENT: return NotFound;
  default:      return OtherError;
  }
}

#ifndef _WIN32
namespace {

constexpr uint32_t kZstdMagic = 0xFD2FB528U;
constexpr uint32_t kZstdSkippableMask = 0xFFFFFFF0U;
constexpr uint32_t kZstdSkippableMagic = 0x184D2A50U;
constexpr size_t kZstdMagicSize = 4;
constexpr size_t kZstdSkippableHeaderSize = 8;  // 4B magic + 4B Frame_Size

// Zstd Seekable Format specification:
// https://github.com/facebook/zstd/blob/dev/contrib/seekable_format/zstd_seekable_compression_format.md
constexpr uint32_t kZstdSeekTableSkipMagic = 0x184D2A5EU;
constexpr uint32_t kZstdSeekableFooterMagic = 0x8F92EAB1U;
constexpr size_t kZstdSeekFooterSize = 9;  // 4B num_frames + 1B descriptor + 4B magic
constexpr size_t kZstdSeekTableMinSize =
    kZstdSkippableHeaderSize + kZstdSeekFooterSize;
constexpr uint8_t kZstdSeekDescChecksumFlag = 0x80U;
constexpr uint8_t kZstdSeekDescReservedMask = 0x7CU;
constexpr uint64_t kZstdSeekEntrySize = 8;  // 4B c_size + 4B d_size
constexpr uint64_t kZstdSeekEntrySizeWithChecksum = 12;

inline uint32_t ReadLE32(const char* p) {
  const auto* b = reinterpret_cast<const uint8_t*>(p);
  return static_cast<uint32_t>(b[0]) |
         (static_cast<uint32_t>(b[1]) << 8) |
         (static_cast<uint32_t>(b[2]) << 16) |
         (static_cast<uint32_t>(b[3]) << 24);
}

inline bool IsZstdStream(const char* data, size_t size) {
  if (size < kZstdMagicSize) return false;
  uint32_t magic = ReadLE32(data);
  return magic == kZstdMagic ||
         (size >= kZstdSkippableHeaderSize &&
          (magic & kZstdSkippableMask) == kZstdSkippableMagic);
}

struct ZstdFrameSpan {
  size_t c_off;
  size_t c_size;
  size_t d_off;
  size_t d_size;
};

// Parse a Zstd Seekable Format seek table at the end of [comp, comp + comp_size)
// in O(num_frames) without scanning compressed blocks.
bool TryParseZstdSeekTable(const char* comp, size_t comp_size,
                           uint64_t max_prealloc,
                           std::vector<ZstdFrameSpan>* frames,
                           size_t* total_raw) {
  if (comp_size < kZstdSeekTableMinSize) return false;
  const char* footer = comp + comp_size - kZstdSeekFooterSize;
  if (ReadLE32(footer + 5) != kZstdSeekableFooterMagic) return false;
  uint8_t descriptor = static_cast<uint8_t>(footer[4]);
  if ((descriptor & kZstdSeekDescReservedMask) != 0) return false;
  uint32_t num_frames = ReadLE32(footer);
  uint64_t entry_size = (descriptor & kZstdSeekDescChecksumFlag)
                            ? kZstdSeekEntrySizeWithChecksum
                            : kZstdSeekEntrySize;
  uint64_t payload =
      static_cast<uint64_t>(num_frames) * entry_size + kZstdSeekFooterSize;
  if (payload > UINT32_MAX || kZstdSkippableHeaderSize + payload > comp_size) {
    return false;
  }

  size_t st_off =
      comp_size - static_cast<size_t>(kZstdSkippableHeaderSize + payload);
  if (ReadLE32(comp + st_off) != kZstdSeekTableSkipMagic ||
      ReadLE32(comp + st_off + kZstdMagicSize) !=
          static_cast<uint32_t>(payload)) {
    return false;
  }

  frames->clear();
  frames->reserve(num_frames);
  size_t c_off = 0;
  uint64_t d_off = 0;
  const char* table = comp + st_off + kZstdSkippableHeaderSize;
  for (uint32_t i = 0; i < num_frames; ++i) {
    const char* entry =
        table + static_cast<size_t>(i) * static_cast<size_t>(entry_size);
    uint32_t c_size = ReadLE32(entry);
    uint32_t d_size = ReadLE32(entry + 4);
    if (c_size < kZstdMagicSize || c_size > st_off - c_off) return false;
    if (d_size > max_prealloc - d_off) return false;
    uint32_t f_magic = ReadLE32(comp + c_off);
    if (f_magic == kZstdMagic) {
      frames->push_back({c_off, c_size, static_cast<size_t>(d_off), d_size});
    } else if ((f_magic & kZstdSkippableMask) == kZstdSkippableMagic &&
               c_size >= kZstdSkippableHeaderSize && d_size == 0) {
      // Skippable frame recorded in seek table.
    } else {
      return false;
    }
    c_off += c_size;
    d_off += d_size;
  }
  if (c_off != st_off) return false;
  *total_raw = static_cast<size_t>(d_off);
  return true;
}

// Scan RFC 8878 frame headers when no seek table is present. Sets *all_known to
// true iff every data frame carries Frame_Content_Size within max_prealloc.
bool TryScanZstdFrames(const char* comp, size_t comp_size,
                       uint64_t max_prealloc,
                       std::vector<ZstdFrameSpan>* frames, size_t* total_raw,
                       bool* all_known, std::string* err) {
  frames->clear();
  *all_known = false;
  size_t c_off = 0;
  uint64_t d_off = 0;
  while (c_off < comp_size) {
    size_t rem = comp_size - c_off;
    if (rem < kZstdMagicSize) {
      *err = "zstd decompression failed: truncated frame header";
      return false;
    }
    uint32_t f_magic = ReadLE32(comp + c_off);
    if (f_magic == kZstdMagic) {
      unsigned long long f_dsize = ZSTD_getFrameContentSize(comp + c_off, rem);
      if (f_dsize == ZSTD_CONTENTSIZE_ERROR) {
        *err = "zstd decompression failed: invalid frame header";
        return false;
      }
      if (f_dsize == ZSTD_CONTENTSIZE_UNKNOWN ||
          f_dsize > max_prealloc - d_off) {
        return true;
      }
      size_t f_csize = ZSTD_findFrameCompressedSize(comp + c_off, rem);
      if (ZSTD_isError(f_csize) || f_csize == 0) {
        *err = std::string("zstd decompression failed: ") +
               ZSTD_getErrorName(f_csize);
        return false;
      }
      frames->push_back({c_off, f_csize, static_cast<size_t>(d_off),
                         static_cast<size_t>(f_dsize)});
      c_off += f_csize;
      d_off += f_dsize;
    } else if ((f_magic & kZstdSkippableMask) == kZstdSkippableMagic) {
      size_t f_csize = ZSTD_findFrameCompressedSize(comp + c_off, rem);
      if (ZSTD_isError(f_csize) || f_csize == 0) {
        *err = std::string("zstd decompression failed: ") +
               ZSTD_getErrorName(f_csize);
        return false;
      }
      c_off += f_csize;
    } else {
      *err = "zstd decompression failed: unknown frame descriptor";
      return false;
    }
  }
  *total_raw = static_cast<size_t>(d_off);
  *all_known = true;
  return true;
}

bool DecompressZstdFrames(const char* comp,
                          const std::vector<ZstdFrameSpan>& frames,
                          size_t total_raw, std::unique_ptr<Mapping>* out,
                          std::string* err) {
  const size_t page_size = sysconf(_SC_PAGESIZE);
  const size_t map_size = RoundUp(total_raw + 1, page_size);
  char* base = static_cast<char*>(
      mmap(nullptr, map_size, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (base == MAP_FAILED) {
    *err = std::string("zstd mmap failed: ") + strerror(errno);
    return false;
  }

  // Cap per-file decompression at 32 threads (matching Soong's ninjazstd
  // encoder cap): 32 cores saturate memory bandwidth without paying thread
  // creation and page-fault contention across 100+ vCPUs on large hosts.
  // Skip thread creation below 64 KiB where single-threaded decode (<40 us)
  // is faster than spawning worker threads.
  static constexpr size_t kMaxZstdThreads = 32;
  static constexpr size_t kMinParallelBytes = 64 << 10;

  const size_t max_threads = std::min(
      static_cast<size_t>(std::max(1, GetThreadPoolThreadCount())),
      kMaxZstdThreads);
  const size_t helper_count =
      (frames.size() > 1 && total_raw >= kMinParallelBytes && max_threads > 1)
          ? std::min(frames.size(), max_threads) - 1
          : 0;

  std::atomic<size_t> next_frame{0};
  std::atomic<bool> failed{false};
  std::string first_err;

  auto record_err = [&](std::string msg) {
    if (!failed.exchange(true)) {
      first_err = std::move(msg);
    }
  };

  auto worker = [&]() {
    ZSTD_DCtx* dctx = nullptr;
    while (!failed.load()) {
      size_t idx = next_frame++;
      if (idx >= frames.size()) break;
      if (!dctx) {
        dctx = ZSTD_createDCtx();
        if (!dctx) {
          record_err("zstd context allocation failed");
          break;
        }
      }
      const ZstdFrameSpan& f = frames[idx];
      size_t r = ZSTD_decompressDCtx(dctx, base + f.d_off, f.d_size,
                                     comp + f.c_off, f.c_size);
      if (ZSTD_isError(r)) {
        record_err(std::string("zstd decompression failed: ") +
                   ZSTD_getErrorName(r));
        break;
      }
      if (r != f.d_size) {
        record_err("zstd decompression failed: frame size mismatch");
        break;
      }
    }
    if (dctx) ZSTD_freeDCtx(dctx);
  };

  std::vector<std::thread> helpers;
  helpers.reserve(helper_count);
  for (size_t i = 0; i < helper_count; ++i) {
    helpers.emplace_back(worker);
  }
  worker();
  for (auto& t : helpers) {
    t.join();
  }

  if (failed.load()) {
    munmap(base, map_size);
    *err = std::move(first_err);
    return false;
  }
  base[total_raw] = '\0';
  out->reset(new Mapping(base, total_raw, map_size));
  return true;
}

bool DecompressZstdStreaming(const char* comp, size_t comp_size,
                             uint64_t max_prealloc,
                             std::unique_ptr<Mapping>* out,
                             std::string* err) {
  size_t cap = static_cast<size_t>(std::min<uint64_t>(
      static_cast<uint64_t>(comp_size) * 4 + 65536, max_prealloc));
  const size_t page_size = sysconf(_SC_PAGESIZE);
  size_t map_size = RoundUp(cap + 1, page_size);
  char* base = static_cast<char*>(
      mmap(nullptr, map_size, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (base == MAP_FAILED) {
    *err = std::string("zstd mmap failed: ") + strerror(errno);
    return false;
  }
  ZSTD_DCtx* dctx = ZSTD_createDCtx();
  if (!dctx) {
    munmap(base, map_size);
    *err = "zstd context allocation failed";
    return false;
  }
  ZSTD_inBuffer in = {comp, comp_size, 0};
  ZSTD_outBuffer out_buf = {base, map_size - 1, 0};
  size_t ret = 0;
  while (in.pos < in.size || ret > 0) {
    if (out_buf.pos == out_buf.size) {
      if (map_size > SIZE_MAX / 2) {
        ZSTD_freeDCtx(dctx);
        munmap(base, map_size);
        *err = "zstd decompressed size exceeds address space";
        return false;
      }
      size_t new_map_size = map_size * 2;
      char* new_base = static_cast<char*>(
          mmap(nullptr, new_map_size, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
      if (new_base == MAP_FAILED) {
        ZSTD_freeDCtx(dctx);
        munmap(base, map_size);
        *err = std::string("zstd mmap failed: ") + strerror(errno);
        return false;
      }
      memcpy(new_base, base, out_buf.pos);
      munmap(base, map_size);
      base = new_base;
      map_size = new_map_size;
      out_buf.dst = base;
      out_buf.size = map_size - 1;
    } else if (in.pos == in.size && ret > 0) {
      ZSTD_freeDCtx(dctx);
      munmap(base, map_size);
      *err = "zstd decompression failed: truncated stream";
      return false;
    }
    ret = ZSTD_decompressStream(dctx, &out_buf, &in);
    if (ZSTD_isError(ret)) {
      ZSTD_freeDCtx(dctx);
      munmap(base, map_size);
      *err = std::string("zstd decompression failed: ") + ZSTD_getErrorName(ret);
      return false;
    }
  }
  ZSTD_freeDCtx(dctx);
  base[out_buf.pos] = '\0';
  out->reset(new Mapping(base, out_buf.pos, map_size));
  return true;
}

bool DecompressZstdMapping(const char* comp, size_t comp_size,
                           std::unique_ptr<Mapping>* out, std::string* err) {
  const uint64_t max_prealloc =
      std::min<uint64_t>(64ULL << 30, SIZE_MAX / 2);
  std::vector<ZstdFrameSpan> frames;
  size_t total_raw = 0;
  if (TryParseZstdSeekTable(comp, comp_size, max_prealloc, &frames,
                            &total_raw)) {
    return DecompressZstdFrames(comp, frames, total_raw, out, err);
  }
  bool all_known = false;
  if (!TryScanZstdFrames(comp, comp_size, max_prealloc, &frames, &total_raw,
                         &all_known, err)) {
    return false;
  }
  if (all_known) {
    return DecompressZstdFrames(comp, frames, total_raw, out, err);
  }
  return DecompressZstdStreaming(comp, comp_size, max_prealloc, out, err);
}

}  // namespace
#endif

FileReader::Status RealDiskInterface::LoadFile(const std::string& path,
                                               std::unique_ptr<LoadedFile>* result,
                                               std::string* err) {
#ifdef _WIN32
#error "LoadFile is not implemented yet on Windows"
#else
  struct MappedFile : LoadedFile {
    MappedFile(const std::string& filename, std::unique_ptr<Mapping> mapping)
        : mapping_(std::move(mapping)) {
      filename_ = filename;
      content_with_nul_ = { mapping_->base(), mapping_->file_size() + 1 };
    }
    std::unique_ptr<Mapping> mapping_;
  };

  std::unique_ptr<Mapping> mapping;
  switch (::MapFile(path, &mapping, err)) {
  case 0:
    if (IsZstdStream(mapping->base(), mapping->file_size()) &&
        !DecompressZstdMapping(mapping->base(), mapping->file_size(), &mapping,
                               err)) {
      return OtherError;
    }
    *result = std::unique_ptr<MappedFile>(new MappedFile(path,
                                                         std::move(mapping)));
    return Okay;
  case -ENOENT:
    return NotFound;
  default:
    return OtherError;
  }
#endif
}

int RealDiskInterface::RemoveFile(const string& path) {
  if (remove(path.c_str()) < 0) {
    switch (errno) {
      case ENOENT:
        return 1;
      default:
        Error("remove(%s): %s", path.c_str(), strerror(errno));
        return -1;
    }
  } else {
    return 0;
  }
}

void RealDiskInterface::AllowStatCache(bool allow) {
#ifdef _WIN32
  use_cache_ = allow;
  if (!use_cache_)
    cache_.clear();
#endif
}

bool RealDiskInterface::Getcwd(std::string* out_path, std::string* err) {
  vector<char> cwd;
  do {
    cwd.resize(cwd.size() + 1024);
    errno = 0;
  } while (!::getcwd(&cwd[0], cwd.size()) && errno == ERANGE);
  if (errno == 0) {
    out_path->assign(&cwd[0]);
    err->clear();
    return true;
  }
  *err = "getcwd failed: ";
  *err += strerror(errno);
  return false;
}

bool RealDiskInterface::Chdir(const std::string dir, std::string* err) {
  if (chdir(dir.c_str()) == 0)
    return true;
  *err = "chdir(" + dir + ") failed: " + strerror(errno);
  return false;
}
