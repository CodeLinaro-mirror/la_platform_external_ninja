// Copyright 2012 Google Inc. All Rights Reserved.
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

#include "deps_log.h"

#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "graph.h"
#include "util.h"
#include "test.h"

namespace {

const char kTestFilename[] = "DepsLogTest-tempfile";

struct DepsLogTest : public testing::Test {
  virtual void SetUp() {
    // In case a crashing test left a stale file behind.
    unlink(kTestFilename);
  }
  virtual void TearDown() {
    unlink(kTestFilename);
  }
};
Node* GetNode(State& state, const string& path) {
  EXPECT_FALSE(strpbrk(path.c_str(), "/\\"));
  return state.GetNode(state.root_scope_.GlobalPath(path), 0);
}

Node* LookupNode(State& state, const string& path) {
  EXPECT_FALSE(strpbrk(path.c_str(), "/\\"));
  return state.LookupNode(state.root_scope_.GlobalPath(path));
}

TEST_F(DepsLogTest, WriteRead) {
  State state1;
  DepsLog log1;
  string err;
  VirtualFileSystem fs;
  EXPECT_TRUE(log1.OpenForWrite(kTestFilename, fs, &err));
  ASSERT_EQ("", err);

  {
    vector<Node*> deps;
    deps.push_back(GetNode(state1, "foo.h"));
    deps.push_back(GetNode(state1, "bar.h"));
    log1.RecordDeps(GetNode(state1, "out.o"), 1, deps);

    deps.clear();
    deps.push_back(GetNode(state1, "foo.h"));
    deps.push_back(GetNode(state1, "bar2.h"));
    log1.RecordDeps(GetNode(state1, "out2.o"), 2, deps);

    DepsLog::Deps* log_deps = log1.GetDeps(GetNode(state1, "out.o"));
    ASSERT_TRUE(log_deps);
    ASSERT_EQ(1, log_deps->mtime);
    ASSERT_EQ(2, log_deps->node_count);
    ASSERT_EQ("foo.h", log_deps->nodes[0]->path());
    ASSERT_EQ("bar.h", log_deps->nodes[1]->path());
  }

  log1.Close();

  State state2;
  DepsLog log2;
  EXPECT_TRUE(log2.Load(kTestFilename, &state2, &err));
  ASSERT_EQ("", err);

  ASSERT_EQ(log1.nodes().size(), log2.nodes().size());
  for (int i = 0; i < (int)log1.nodes().size(); ++i) {
    Node* node1 = log1.nodes()[i];
    Node* node2 = log2.nodes()[i];
    ASSERT_EQ(i, node1->id());
    ASSERT_EQ(node1->id(), node2->id());
  }

  // Spot-check the entries in log2.
  DepsLog::Deps* log_deps = log2.GetDeps(GetNode(state2, "out2.o"));
  ASSERT_TRUE(log_deps);
  ASSERT_EQ(2, log_deps->mtime);
  ASSERT_EQ(2, log_deps->node_count);
  ASSERT_EQ("foo.h", log_deps->nodes[0]->path());
  ASSERT_EQ("bar2.h", log_deps->nodes[1]->path());
}

TEST_F(DepsLogTest, LotsOfDeps) {
  const int kNumDeps = 100000;  // More than 64k.

  State state1;
  DepsLog log1;
  string err;
  VirtualFileSystem fs;
  EXPECT_TRUE(log1.OpenForWrite(kTestFilename, fs, &err));
  ASSERT_EQ("", err);

  // The paths_ concurrent hash table doesn't automatically resize itself, so
  // reserve space in advance before synthesizing paths.
  state1.paths_.reserve(kNumDeps);

  {
    vector<Node*> deps;
    for (int i = 0; i < kNumDeps; ++i) {
      char buf[32];
      sprintf(buf, "file%d.h", i);
      deps.push_back(GetNode(state1, buf));
    }
    log1.RecordDeps(GetNode(state1, "out.o"), 1, deps);

    DepsLog::Deps* log_deps = log1.GetDeps(GetNode(state1, "out.o"));
    ASSERT_EQ(kNumDeps, log_deps->node_count);
  }

  log1.Close();

  State state2;
  DepsLog log2;
  EXPECT_TRUE(log2.Load(kTestFilename, &state2, &err));
  ASSERT_EQ("", err);

  DepsLog::Deps* log_deps = log2.GetDeps(GetNode(state2, "out.o"));
  ASSERT_EQ(kNumDeps, log_deps->node_count);
}

// Verify that adding the same deps twice doesn't grow the file.
TEST_F(DepsLogTest, DoubleEntry) {
  // Write some deps to the file and grab its size.
  int file_size;
  {
    State state;
    DepsLog log;
    string err;
    VirtualFileSystem fs;
    EXPECT_TRUE(log.OpenForWrite(kTestFilename, fs, &err));
    ASSERT_EQ("", err);

    vector<Node*> deps;
    deps.push_back(GetNode(state, "foo.h"));
    deps.push_back(GetNode(state, "bar.h"));
    log.RecordDeps(GetNode(state, "out.o"), 1, deps);
    log.Close();

    struct stat st;
    ASSERT_EQ(0, stat(kTestFilename, &st));
    file_size = (int)st.st_size;
    ASSERT_GT(file_size, 0);
  }

  // Now reload the file, and read the same deps.
  {
    State state;
    DepsLog log;
    string err;
    VirtualFileSystem fs;
    EXPECT_TRUE(log.Load(kTestFilename, &state, &err));

    EXPECT_TRUE(log.OpenForWrite(kTestFilename, fs, &err));
    ASSERT_EQ("", err);

    vector<Node*> deps;
    deps.push_back(GetNode(state, "foo.h"));
    deps.push_back(GetNode(state, "bar.h"));
    log.RecordDeps(GetNode(state, "out.o"), 1, deps);
    log.Close();

    struct stat st;
    ASSERT_EQ(0, stat(kTestFilename, &st));
    int file_size_2 = (int)st.st_size;
    ASSERT_EQ(file_size, file_size_2);
  }
}

// Verify that adding the new deps works and can be compacted away.
TEST_F(DepsLogTest, Recompact) {
  const char kManifest[] =
"rule cc\n"
"  command = cc\n"
"  deps = gcc\n"
"build out.o: cc\n"
"build other_out.o: cc\n";

  // Write some deps to the file and grab its size.
  int file_size;
  {
    State state;
    ASSERT_NO_FATAL_FAILURE(AssertParse(&state, kManifest));
    DepsLog log;
    string err;
    VirtualFileSystem fs;
    ASSERT_TRUE(log.OpenForWrite(kTestFilename, fs, &err));
    ASSERT_EQ("", err);

    vector<Node*> deps;
    deps.push_back(GetNode(state, "foo.h"));
    deps.push_back(GetNode(state, "bar.h"));
    log.RecordDeps(GetNode(state, "out.o"), 1, deps);

    deps.clear();
    deps.push_back(GetNode(state, "foo.h"));
    deps.push_back(GetNode(state, "baz.h"));
    log.RecordDeps(GetNode(state, "other_out.o"), 1, deps);

    log.Close();

    struct stat st;
    ASSERT_EQ(0, stat(kTestFilename, &st));
    file_size = (int)st.st_size;
    ASSERT_GT(file_size, 0);
  }

  // Now reload the file, and add slightly different deps.
  int file_size_2;
  {
    State state;
    ASSERT_NO_FATAL_FAILURE(AssertParse(&state, kManifest));
    DepsLog log;
    string err;
    VirtualFileSystem fs;
    ASSERT_TRUE(log.Load(kTestFilename, &state, &err));

    ASSERT_TRUE(log.OpenForWrite(kTestFilename, fs, &err));
    ASSERT_EQ("", err);

    vector<Node*> deps;
    deps.push_back(GetNode(state, "foo.h"));
    log.RecordDeps(GetNode(state, "out.o"), 1, deps);
    log.Close();

    struct stat st;
    ASSERT_EQ(0, stat(kTestFilename, &st));
    file_size_2 = (int)st.st_size;
    // The file should grow to record the new deps.
    ASSERT_GT(file_size_2, file_size);
  }

  // Now reload the file, verify the new deps have replaced the old, then
  // recompact.
  int file_size_3;
  {
    State state;
    ASSERT_NO_FATAL_FAILURE(AssertParse(&state, kManifest));
    DepsLog log;
    string err;
    ASSERT_TRUE(log.Load(kTestFilename, &state, &err));

    Node* out = GetNode(state, "out.o");
    DepsLog::Deps* deps = log.GetDeps(out);
    ASSERT_TRUE(deps);
    ASSERT_EQ(1, deps->mtime);
    ASSERT_EQ(1, deps->node_count);
    ASSERT_EQ("foo.h", deps->nodes[0]->path());

    Node* other_out = GetNode(state, "other_out.o");
    deps = log.GetDeps(other_out);
    ASSERT_TRUE(deps);
    ASSERT_EQ(1, deps->mtime);
    ASSERT_EQ(2, deps->node_count);
    ASSERT_EQ("foo.h", deps->nodes[0]->path());
    ASSERT_EQ("baz.h", deps->nodes[1]->path());

    VirtualFileSystem fs;
    ASSERT_TRUE(log.Recompact(kTestFilename, fs, &err));

    // The in-memory deps graph should still be valid after recompaction.
    deps = log.GetDeps(out);
    ASSERT_TRUE(deps);
    ASSERT_EQ(1, deps->mtime);
    ASSERT_EQ(1, deps->node_count);
    ASSERT_EQ("foo.h", deps->nodes[0]->path());
    ASSERT_EQ(out, log.nodes()[out->id()]);

    deps = log.GetDeps(other_out);
    ASSERT_TRUE(deps);
    ASSERT_EQ(1, deps->mtime);
    ASSERT_EQ(2, deps->node_count);
    ASSERT_EQ("foo.h", deps->nodes[0]->path());
    ASSERT_EQ("baz.h", deps->nodes[1]->path());
    ASSERT_EQ(other_out, log.nodes()[other_out->id()]);

    // The file should have shrunk a bit for the smaller deps.
    struct stat st;
    ASSERT_EQ(0, stat(kTestFilename, &st));
    file_size_3 = (int)st.st_size;
    ASSERT_LT(file_size_3, file_size_2);
  }

  // Now reload the file and recompact with an empty manifest. The previous
  // entries should be removed.
  {
    State state;
    // Intentionally not parsing kManifest here.
    DepsLog log;
    string err;
    ASSERT_TRUE(log.Load(kTestFilename, &state, &err));

    Node* out = GetNode(state, "out.o");
    DepsLog::Deps* deps = log.GetDeps(out);
    ASSERT_TRUE(deps);
    ASSERT_EQ(1, deps->mtime);
    ASSERT_EQ(1, deps->node_count);
    ASSERT_EQ("foo.h", deps->nodes[0]->path());

    Node* other_out = GetNode(state, "other_out.o");
    deps = log.GetDeps(other_out);
    ASSERT_TRUE(deps);
    ASSERT_EQ(1, deps->mtime);
    ASSERT_EQ(2, deps->node_count);
    ASSERT_EQ("foo.h", deps->nodes[0]->path());
    ASSERT_EQ("baz.h", deps->nodes[1]->path());

    // Keep out.o dependencies.
    VirtualFileSystem fs;
    fs.Create("out.o", "");

    ASSERT_TRUE(log.Recompact(kTestFilename, fs, &err));

    deps = log.GetDeps(out);
    ASSERT_TRUE(deps);
    ASSERT_EQ(1, deps->mtime);
    ASSERT_EQ(1, deps->node_count);
    ASSERT_EQ("foo.h", deps->nodes[0]->path());
    ASSERT_EQ(out, log.nodes()[out->id()]);

    // The previous entries should have been removed.
    deps = log.GetDeps(other_out);
    ASSERT_FALSE(deps);

    //ASSERT_EQ(-1, state.LookupNode("foo.h")->id());
    // The .h files pulled in via deps should no longer have ids either.
    ASSERT_EQ(-1, LookupNode(state, "baz.h")->id());

    // The file should have shrunk more.
    struct stat st;
    ASSERT_EQ(0, stat(kTestFilename, &st));
    int file_size_4 = (int)st.st_size;
    ASSERT_LT(file_size_4, file_size_3);
  }
}

// Verify that invalid file headers cause a new build.
TEST_F(DepsLogTest, InvalidHeader) {
  const char *kInvalidHeaders[] = {
    "",                              // Empty file.
    "# ninjad",                      // Truncated first line.
    "# ninjadeps\n",                 // No version int.
    "# ninjadeps\n\001\002",         // Truncated version int.
    "# ninjadeps\n\001\002\003\004"  // Invalid version int.
  };
  for (size_t i = 0; i < sizeof(kInvalidHeaders) / sizeof(kInvalidHeaders[0]);
       ++i) {
    FILE* deps_log = fopen(kTestFilename, "wb");
    ASSERT_TRUE(deps_log != NULL);
    ASSERT_EQ(
        strlen(kInvalidHeaders[i]),
        fwrite(kInvalidHeaders[i], 1, strlen(kInvalidHeaders[i]), deps_log));
    ASSERT_EQ(0 ,fclose(deps_log));

    string err;
    DepsLog log;
    State state;
    ASSERT_TRUE(log.Load(kTestFilename, &state, &err));
    EXPECT_EQ("bad deps log signature or version; starting over", err);
  }
}

// Simulate what happens when loading a truncated log file.
TEST_F(DepsLogTest, Truncated) {
  // Create a file with some entries.
  {
    State state;
    DepsLog log;
    string err;
    VirtualFileSystem fs;
    EXPECT_TRUE(log.OpenForWrite(kTestFilename, fs, &err));
    ASSERT_EQ("", err);

    vector<Node*> deps;
    deps.push_back(GetNode(state, "foo.h"));
    deps.push_back(GetNode(state, "bar.h"));
    log.RecordDeps(GetNode(state, "out.o"), 1, deps);

    deps.clear();
    deps.push_back(GetNode(state, "foo.h"));
    deps.push_back(GetNode(state, "bar2.h"));
    log.RecordDeps(GetNode(state, "out2.o"), 2, deps);

    log.Close();
  }

  // Get the file size.
  struct stat st;
  ASSERT_EQ(0, stat(kTestFilename, &st));

  // Try reloading at truncated sizes.
  // Track how many nodes/deps were found; they should decrease with
  // smaller sizes.
  int node_count = 5;
  int deps_count = 2;
  for (int size = (int)st.st_size; size > 0; --size) {
    string err;
    ASSERT_TRUE(Truncate(kTestFilename, size, &err));

    State state;
    DepsLog log;
    EXPECT_TRUE(log.Load(kTestFilename, &state, &err));
    if (!err.empty()) {
      // At some point the log will be so short as to be unparseable.
      break;
    }

    ASSERT_GE(node_count, (int)log.nodes().size());
    node_count = log.nodes().size();

    // Count how many non-NULL deps entries there are.
    int new_deps_count = 0;
    for (vector<DepsLog::Deps*>::const_iterator i = log.deps().begin();
         i != log.deps().end(); ++i) {
      if (*i)
        ++new_deps_count;
    }
    ASSERT_GE(deps_count, new_deps_count);
    deps_count = new_deps_count;
  }
}

// Run the truncation-recovery logic.
TEST_F(DepsLogTest, TruncatedRecovery) {
  // Create a file with some entries.
  {
    State state;
    DepsLog log;
    string err;
    VirtualFileSystem fs;
    EXPECT_TRUE(log.OpenForWrite(kTestFilename, fs, &err));
    ASSERT_EQ("", err);

    vector<Node*> deps;
    deps.push_back(GetNode(state, "foo.h"));
    deps.push_back(GetNode(state, "bar.h"));
    log.RecordDeps(GetNode(state, "out.o"), 1, deps);

    deps.clear();
    deps.push_back(GetNode(state, "foo.h"));
    deps.push_back(GetNode(state, "bar2.h"));
    log.RecordDeps(GetNode(state, "out2.o"), 2, deps);

    log.Close();
  }

  // Shorten the file, corrupting the last record.
  {
    struct stat st;
    ASSERT_EQ(0, stat(kTestFilename, &st));
    string err;
    ASSERT_TRUE(Truncate(kTestFilename, st.st_size - 2, &err));
  }

  // Load the file again, add an entry.
  {
    State state;
    DepsLog log;
    string err;
    EXPECT_TRUE(log.Load(kTestFilename, &state, &err));
    ASSERT_EQ("premature end of file; recovering", err);
    err.clear();

    // The truncated entry should've been discarded.
    EXPECT_EQ(NULL, log.GetDeps(GetNode(state, "out2.o")));

    VirtualFileSystem fs;
    EXPECT_TRUE(log.OpenForWrite(kTestFilename, fs, &err));
    ASSERT_EQ("", err);

    // Add a new entry.
    vector<Node*> deps;
    deps.push_back(GetNode(state, "foo.h"));
    deps.push_back(GetNode(state, "bar2.h"));
    log.RecordDeps(GetNode(state, "out2.o"), 3, deps);

    log.Close();
  }

  // Load the file a third time to verify appending after a mangled
  // entry doesn't break things.
  {
    State state;
    DepsLog log;
    string err;
    EXPECT_TRUE(log.Load(kTestFilename, &state, &err));

    // The truncated entry should exist.
    DepsLog::Deps* deps = log.GetDeps(GetNode(state, "out2.o"));
    ASSERT_TRUE(deps);
  }
}

template <typename Func>
static void DoLoadInvalidLogTest(Func&& func) {
  State state;
  DepsLog log;
  std::string err;
  ASSERT_TRUE(log.Load(kTestFilename, &state, &err));
  ASSERT_EQ("premature end of file; recovering", err);
  func(&state, &log);
}

TEST_F(DepsLogTest, LoadInvalidLog) {
  struct Item {
    Item(int num) : is_num(true), num(num) {}
    Item(const char* str) : is_num(false), str(str) {}

    bool is_num;
    uint32_t num;
    const char* str;
  };

  auto write_file = [](std::vector<Item> items) {
    FILE* fp = fopen(kTestFilename, "wb");
    for (const Item& item : items) {
      if (item.is_num) {
        ASSERT_EQ(1, fwrite(&item.num, sizeof(item.num), 1, fp));
      } else {
        ASSERT_EQ(strlen(item.str), fwrite(item.str, 1, strlen(item.str), fp));
      }
    }
    fclose(fp);
  };

  const int kCurrentVersion = 4;
  auto path_hdr = [](int path_len) -> int {
    return RoundUp(path_len, 4) + 4;
  };
  auto deps_hdr = [](int deps_cnt) -> int {
    return 0x80000000 | ((3 * sizeof(uint32_t)) + (deps_cnt * 4));
  };

  write_file({
    "# ninjadeps\n", kCurrentVersion,
    path_hdr(4), "foo0", ~0, // node #0
    path_hdr(4), "foo1", ~2, // invalid path ID
  });
  DoLoadInvalidLogTest([](State* state, DepsLog* log) {
    ASSERT_EQ(0, LookupNode(*state, "foo0")->id());
    ASSERT_EQ(nullptr, LookupNode(*state, "foo1"));
  });

  write_file({
    "# ninjadeps\n", kCurrentVersion,
    path_hdr(4), "foo0", ~0, // node #0
    deps_hdr(1), /*node*/0, /*mtime*/5, 0, /*node*/1, // invalid src ID
    path_hdr(4), "foo1", ~1, // node #1
  });
  DoLoadInvalidLogTest([](State* state, DepsLog* log) {
    ASSERT_EQ(0, LookupNode(*state, "foo0")->id());
    ASSERT_EQ(nullptr, log->GetDeps(LookupNode(*state, "foo0")));
    ASSERT_EQ(nullptr, LookupNode(*state, "foo1"));
  });

  write_file({
    "# ninjadeps\n", kCurrentVersion,
    path_hdr(4), "foo0", ~0, // node #0
    deps_hdr(1), /*node*/1, /*mtime*/5, 0, /*node*/0, // invalid out ID
    path_hdr(4), "foo1", ~1, // node #1
  });
  DoLoadInvalidLogTest([](State* state, DepsLog* log) {
    ASSERT_EQ(0, LookupNode(*state, "foo0")->id());
    ASSERT_EQ(nullptr, LookupNode(*state, "foo1"));
  });

  write_file({
    "# ninjadeps\n", kCurrentVersion,
    path_hdr(4), "foo0", ~0, // node #0
    path_hdr(4), "foo1", ~1, // node #1
    path_hdr(4), "foo2", ~2, // node #2
    deps_hdr(1), /*node*/2, /*mtime*/5, 0, /*node*/1,
    deps_hdr(1), /*node*/2, /*mtime*/6, 0, /*node*/3, // invalid src ID

    // No records after the invalid record are parsed.
    path_hdr(4), "foo3", ~3, // node #3
    deps_hdr(1), /*node*/3, /*mtime*/7, 0, /*node*/0,
    path_hdr(4), "foo4", ~4, // node #4
    deps_hdr(1), /*node*/4, /*mtime*/8, 0, /*node*/0,

    // Truncation must be handled before looking for the last deps record
    // that outputs a given node.
    deps_hdr(1), /*node*/2, /*mtime*/9, 0, /*node*/0,
    deps_hdr(1), /*node*/2, /*mtime*/9, 0, /*node*/3,
  });
  DoLoadInvalidLogTest([](State* state, DepsLog* log) {
    ASSERT_EQ(0, LookupNode(*state, "foo0")->id());
    ASSERT_EQ(1, LookupNode(*state, "foo1")->id());
    ASSERT_EQ(2, LookupNode(*state, "foo2")->id());
    ASSERT_EQ(nullptr, LookupNode(*state, "foo3"));
    ASSERT_EQ(nullptr, LookupNode(*state, "foo4"));

    ASSERT_EQ(nullptr, log->GetDeps(LookupNode(*state, "foo1")));

    DepsLog::Deps* deps = log->GetDeps(LookupNode(*state, "foo2"));
    ASSERT_EQ(5, deps->mtime);
    ASSERT_EQ(1, deps->node_count);
    ASSERT_EQ(1, deps->nodes[0]->id());
  });
}

TEST_F(DepsLogTest, MustBeDepsRecordHeader) {
  // Mark a word as a candidate.
  static constexpr uint64_t kCandidate = 0x100000000;

  // Verifies that MustBeDepsRecordHeader returns the expected value. Returns
  // true on success.
  auto do_test = [](std::vector<uint64_t> words) -> bool {
    std::vector<uint32_t> data;
    for (uint64_t word : words) {
      // Coerce from uint64_t to uint32_t to mask off the kCandidate flag.
      data.push_back(word);
    }
    DepsLogData log;
    log.words = data.data();
    log.size = data.size();
    for (size_t i = 0; i < words.size(); ++i) {
      const bool expected = (words[i] & kCandidate) == kCandidate;
      if (expected != MustBeDepsRecordHeader(log, i)) {
        printf("\n%s,%d: bad index: %zu\n", __FILE__, __LINE__, i);
        return false;
      }
    }
    return true;
  };

  // Two valid deps records with no dependencies. Each record's header is
  // recognized as the start of a deps record. The first record has an mtime_hi
  // from 2262.
  EXPECT_TRUE(do_test({
    // header               output_id   mtime_lo    mtime_hi
    0x8000000c|kCandidate,  1,          2,          0x80000100,
    0x8000000c|kCandidate,  3,          4,          5,
  }));

  // The first record's mtime_lo is within a 524us window. The second record's
  // header looks like a potential mtime_lo for 0x8007fffc.
  EXPECT_TRUE(do_test({
    // header               output_id   mtime_lo    mtime_hi
    0x8000000c|kCandidate,  1,          0x8007fffc, 2,
    0x8000000c,             3,          4,          5,
  }));

  // 0x80080000 is above the maximum record size, so it is rejected as a
  // possible header.
  EXPECT_TRUE(do_test({
    // header               output_id   mtime_lo    mtime_hi
    0x8000000c|kCandidate,  1,          0x80080000, 2,
    0x8000000c|kCandidate,  3,          4,          5,
  }));

  // Two deps records with >16K inputs each. The header could be confused with a
  // path string containing control characters, so it's not a candidate.
  EXPECT_TRUE(do_test({
    // header               output_id   mtime_lo    mtime_hi
    0x80010101,             1,          2,          3,    // input IDs elided...
    0x80010101,             4,          5,          6,    // input IDs elided...
  }));

  // The first record has a single dependency and an mtime_hi from 2262. The
  // second deps record's header looks like a potential mtime_lo for 0x80000100.
  EXPECT_TRUE(do_test({
    // header               output_id   mtime_lo    mtime_hi
    0x80000010|kCandidate,  1,          2,          0x80000100, 3,
    0x8000000c,             4,          5,          6,
  }));

  // The first deps record's mtime_lo is within a 524us window, and the second
  // record's header looks like a potential mtime_hi for 0x80000100.
  EXPECT_TRUE(do_test({
    // header               output_id   mtime_lo    mtime_hi
    0x80000010|kCandidate,  1,          0x80000100, 2,          3,
    0x8000000c,             4,          5,          6,
  }));

  // The first record has two dependencies, so its mtime_lo doesn't disqualify
  // the next record's header.
  EXPECT_TRUE(do_test({
    // header               output_id   mtime_lo    mtime_hi
    0x80000014|kCandidate,  1,          0x80000100, 2,          3,          4,
    0x8000000c|kCandidate,  5,          6,          7,
  }));

  // The first deps record's mtime_hi is from 2262, and the second record's
  // header looks like a potential mtime_hi for 0x80000100.
  EXPECT_TRUE(do_test({
    // header               output_id   mtime_lo    mtime_hi
    0x80000014|kCandidate,  1,          2,          0x80000100, 3,          4,
    0x8000000c,             5,          6,          7,
  }));
}

}  // anonymous namespace
