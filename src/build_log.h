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

#ifndef NINJA_BUILD_LOG_H_
#define NINJA_BUILD_LOG_H_

#include <string>
#include <stdio.h>
using namespace std;

#include "hashed_str_view.h"
#include "concurrent_hash_map.h"
#include "timestamp.h"
#include "util.h"  // uint64_t
#include "eval_env.h"  // GlobalPathStr

struct State;
struct Edge;

/// Can answer questions about the manifest for the BuildLog.
struct BuildLogUser {
  /// Return if a given output is no longer part of the build manifest.
  /// This is only called during recompaction and doesn't have to be fast.
  virtual bool IsPathDead(GlobalPathStr s) const = 0;
};

/// Store a log of every command ran for every build.
/// It has a few uses:
///
/// 1) (hashes of) command lines for existing output files, so we know
///    when we need to rebuild due to the command changing
/// 2) timing information, perhaps for generating reports
/// 3) restat information
struct BuildLog {
  BuildLog();
  ~BuildLog();

  bool OpenForWrite(const string& path, const BuildLogUser& user, string* err);
  bool RecordCommand(Edge* edge, int start_time, int end_time,
                     TimeStamp mtime = 0);
  void Close();

  /// Load the on-disk log.
  bool Load(const string& path, string* err);

  struct LogEntry {
    HashedStr output;
    uint64_t command_hash;
    int start_time;
    int end_time;
    TimeStamp mtime;

    // Used during build log parsing.
    std::atomic<size_t> newest_parsed_line;

    static uint64_t HashCommand(StringPiece command);

    // Used by tests.
    bool operator==(const LogEntry& o) {
      return output == o.output && command_hash == o.command_hash &&
          start_time == o.start_time && end_time == o.end_time &&
          mtime == o.mtime;
    }

    explicit LogEntry(const HashedStrView& output);
    LogEntry(const HashedStrView& output, uint64_t command_hash,
             int start_time, int end_time, TimeStamp restat_mtime);
  };

  /// Lookup a previously-run command by its output path.
  LogEntry* LookupByOutput(GlobalPathStr path);

  /// Serialize an entry into a log file.
  bool WriteEntry(FILE* f, const LogEntry& entry);

  /// Rewrite the known log entries, throwing away old data.
  bool Recompact(const string& path, const BuildLogUser& user, string* err);

  /// The HashedStrView refers to memory in the LogEntry itself. This map uses
  /// a value of LogEntry* rather than LogEntry, because moving a LogEntry would
  /// invalidate the HashedStrView.
  typedef ConcurrentHashMap<HashedStrView, LogEntry*> Entries;
  const Entries& entries() const { return entries_; }

 private:
  Entries entries_;
  FILE* log_file_;
  bool needs_recompaction_;
};

#endif // NINJA_BUILD_LOG_H_
