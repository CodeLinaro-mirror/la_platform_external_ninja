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

#ifndef NINJA_DISK_INTERFACE_H_
#define NINJA_DISK_INTERFACE_H_

#include <map>
#include <memory>
#include <string>
using namespace std;

#include "string_piece.h"
#include "timestamp.h"

/// A file whose content has been loaded into memory.
struct LoadedFile {
  LoadedFile() {}

  LoadedFile(const LoadedFile&) = delete;
  LoadedFile& operator=(const LoadedFile&) = delete;

  virtual ~LoadedFile() {}

  const std::string& filename() const { return filename_; }

  /// Return the size of the file's content, excluding the extra NUL at the end.
  size_t size() const { return content_with_nul_.size() - 1; }

  /// Return the file content, excluding the extra NUL at the end.
  StringPiece content() const {
    return content_with_nul_.substr(0, content_with_nul_.size() - 1);
  }

  /// The last character of this StringPiece will be NUL. (i.e. The size of this
  /// view will be 1 greater than the true file size.)
  StringPiece content_with_nul() const { return content_with_nul_; }

protected:
  std::string filename_;

  /// The start of the string view must be aligned sufficiently for any basic
  /// type it may contain (e.g. a minimum alignment of 16 bytes is typical). The
  /// alignment minimum is necessary for parsing the 4-byte aligned binary
  /// .ninja_deps log.
  StringPiece content_with_nul_;
};

/// An implementation of LoadedFile where the file's content is copied into a
/// char[] array. This class adds a NUL to the given string.
struct HeapLoadedFile : LoadedFile {
  HeapLoadedFile(const std::string& filename, StringPiece content) {
    filename_ = filename;
    // An array allocated with new char[N] is guaranteed to be sufficiently
    // aligned for any fundamental type.
    storage_.reset(new char[content.size() + 1]);
    memcpy(storage_.get(), content.data(), content.size());
    storage_[content.size()] = '\0';
    content_with_nul_ = StringPiece(storage_.get(), content.size() + 1);
  }

private:
  /// Use new char[] to provide the alignment guarantee this class needs.
  std::unique_ptr<char[]> storage_;
};

/// Interface for reading files from disk.  See DiskInterface for details.
/// This base offers the minimum interface needed just to read files.
struct FileReader {
  virtual ~FileReader() {}

  /// Result of ReadFile.
  enum Status {
    Okay,
    NotFound,
    OtherError
  };

  /// Read and store in given string.  On success, return Okay.
  /// On error, return another Status and fill |err|.
  virtual Status ReadFile(const string& path, string* contents,
                          string* err) = 0;

  /// Open the file for reading and return an abstract LoadedFile. On success,
  /// return Okay. On error, return another Status and fill |err|.
  virtual Status LoadFile(const std::string& path,
                          std::unique_ptr<LoadedFile>* result,
                          std::string* err) = 0;
};

/// Interface for accessing the disk.
///
/// Abstract so it can be mocked out for tests.  The real implementation
/// is RealDiskInterface.
struct DiskInterface: public FileReader {
  /// stat() a file, returning the mtime, or 0 if missing and -1 on
  /// other errors. Thread-safe iff IsStatThreadSafe returns true.
  virtual TimeStamp Stat(const string& path, string* err) const = 0;

  /// lstat() a path, returning the mtime, or 0 if missing and -1 on
  /// other errors. Does not traverse symlinks, and returns whether the
  /// path represents a directory or a symlink. Thread-safe iff
  /// IsStatThreadSafe returns true.
  virtual TimeStamp LStat(const string& path, bool* is_dir, bool* is_symlink, string* err) const = 0;

  /// True if Stat() can be called from multiple threads concurrently.
  virtual bool IsStatThreadSafe() const = 0;

  /// Create a directory, returning false on failure.
  virtual bool MakeDir(const string& path) = 0;

  /// Create a file, with the specified name and contents
  /// Returns true on success, false on failure
  virtual bool WriteFile(const string& path, const string& contents) = 0;

  /// Remove the file named @a path. It behaves like 'rm -f path' so no errors
  /// are reported if it does not exists.
  /// @returns 0 if the file has been removed,
  ///          1 if the file does not exist, and
  ///          -1 if an error occurs.
  virtual int RemoveFile(const string& path) = 0;

  /// Create all the parent directories for path; like mkdir -p
  /// `basename path`.
  bool MakeDirs(const string& path);
};

/// Implementation of DiskInterface that actually hits the disk.
struct RealDiskInterface : public DiskInterface {
  RealDiskInterface()
#ifdef _WIN32
                      : use_cache_(false)
#endif
                      {}
  virtual ~RealDiskInterface() {}
  virtual TimeStamp Stat(const string& path, string* err) const;
  virtual TimeStamp LStat(const string& path, bool* is_dir, bool* is_symlink, string* err) const;
  virtual bool IsStatThreadSafe() const;
  virtual bool MakeDir(const string& path);
  virtual bool WriteFile(const string& path, const string& contents);
  virtual Status ReadFile(const string& path, string* contents, string* err);
  virtual Status LoadFile(const std::string& path,
                          std::unique_ptr<LoadedFile>* result,
                          std::string* err);
  virtual int RemoveFile(const string& path);

  /// Whether stat information can be cached.  Only has an effect on Windows.
  void AllowStatCache(bool allow);

 private:
#ifdef _WIN32
  /// Whether stat information can be cached.
  bool use_cache_;

  typedef map<string, TimeStamp> DirCache;
  // TODO: Neither a map nor a hashmap seems ideal here.  If the statcache
  // works out, come up with a better data structure.
  typedef map<string, DirCache> Cache;
  mutable Cache cache_;
#endif
};

#endif  // NINJA_DISK_INTERFACE_H_
