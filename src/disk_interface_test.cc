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

#include <assert.h>
#include <stdio.h>
#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <zstd.h>
#endif

#include "disk_interface.h"
#include "graph.h"
#include "test.h"
#include "thread_pool.h"

namespace {

struct DiskInterfaceTest : public testing::Test {
  virtual void SetUp() {
    // These tests do real disk accesses, so create a temp dir.
    temp_dir_.CreateAndEnter("Ninja-DiskInterfaceTest");
  }

  virtual void TearDown() {
    temp_dir_.Cleanup();
  }

  bool Touch(const char* path) {
    FILE *f = fopen(path, "w");
    if (!f)
      return false;
    return fclose(f) == 0;
  }

  ScopedTempDir temp_dir_;
  RealDiskInterface disk_;
};

TEST_F(DiskInterfaceTest, StatMissingFile) {
  string err;
  EXPECT_EQ(0, disk_.Stat("nosuchfile", &err));
  EXPECT_EQ("", err);

  // On Windows, the errno for a file in a nonexistent directory
  // is different.
  EXPECT_EQ(0, disk_.Stat("nosuchdir/nosuchfile", &err));
  EXPECT_EQ("", err);

  // On POSIX systems, the errno is different if a component of the
  // path prefix is not a directory.
  ASSERT_TRUE(Touch("notadir"));
  EXPECT_EQ(0, disk_.Stat("notadir/nosuchfile", &err));
  EXPECT_EQ("", err);
}

TEST_F(DiskInterfaceTest, StatBadPath) {
  string err;
#ifdef _WIN32
  string bad_path("cc:\\foo");
  EXPECT_EQ(-1, disk_.Stat(bad_path, &err));
  EXPECT_NE("", err);
#else
  string too_long_name(512, 'x');
  EXPECT_EQ(-1, disk_.Stat(too_long_name, &err));
  EXPECT_NE("", err);
#endif
}

TEST_F(DiskInterfaceTest, StatExistingFile) {
  string err;
  ASSERT_TRUE(Touch("file"));
  EXPECT_GT(disk_.Stat("file", &err), 1);
  EXPECT_EQ("", err);
}

TEST_F(DiskInterfaceTest, StatExistingDir) {
  string err;
  ASSERT_TRUE(disk_.MakeDir("subdir"));
  ASSERT_TRUE(disk_.MakeDir("subdir/subsubdir"));
  EXPECT_GT(disk_.Stat("..", &err), 1);
  EXPECT_EQ("", err);
  EXPECT_GT(disk_.Stat(".", &err), 1);
  EXPECT_EQ("", err);
  EXPECT_GT(disk_.Stat("subdir", &err), 1);
  EXPECT_EQ("", err);
  EXPECT_GT(disk_.Stat("subdir/subsubdir", &err), 1);
  EXPECT_EQ("", err);

  EXPECT_EQ(disk_.Stat("subdir", &err),
            disk_.Stat("subdir/.", &err));
  EXPECT_EQ(disk_.Stat("subdir", &err),
            disk_.Stat("subdir/subsubdir/..", &err));
  EXPECT_EQ(disk_.Stat("subdir/subsubdir", &err),
            disk_.Stat("subdir/subsubdir/.", &err));
}

#ifdef _WIN32
TEST_F(DiskInterfaceTest, StatCache) {
  string err;

  ASSERT_TRUE(Touch("file1"));
  ASSERT_TRUE(Touch("fiLE2"));
  ASSERT_TRUE(disk_.MakeDir("subdir"));
  ASSERT_TRUE(disk_.MakeDir("subdir/subsubdir"));
  ASSERT_TRUE(Touch("subdir\\subfile1"));
  ASSERT_TRUE(Touch("subdir\\SUBFILE2"));
  ASSERT_TRUE(Touch("subdir\\SUBFILE3"));

  disk_.AllowStatCache(false);
  TimeStamp parent_stat_uncached = disk_.Stat("..", &err);
  disk_.AllowStatCache(true);

  EXPECT_GT(disk_.Stat("FIle1", &err), 1);
  EXPECT_EQ("", err);
  EXPECT_GT(disk_.Stat("file1", &err), 1);
  EXPECT_EQ("", err);

  EXPECT_GT(disk_.Stat("subdir/subfile2", &err), 1);
  EXPECT_EQ("", err);
  EXPECT_GT(disk_.Stat("sUbdir\\suBFile1", &err), 1);
  EXPECT_EQ("", err);

  EXPECT_GT(disk_.Stat("..", &err), 1);
  EXPECT_EQ("", err);
  EXPECT_GT(disk_.Stat(".", &err), 1);
  EXPECT_EQ("", err);
  EXPECT_GT(disk_.Stat("subdir", &err), 1);
  EXPECT_EQ("", err);
  EXPECT_GT(disk_.Stat("subdir/subsubdir", &err), 1);
  EXPECT_EQ("", err);

#ifndef _MSC_VER // TODO: Investigate why. Also see https://github.com/ninja-build/ninja/pull/1423
  EXPECT_EQ(disk_.Stat("subdir", &err),
            disk_.Stat("subdir/.", &err));
  EXPECT_EQ("", err);
  EXPECT_EQ(disk_.Stat("subdir", &err),
            disk_.Stat("subdir/subsubdir/..", &err));
#endif
  EXPECT_EQ("", err);
  EXPECT_EQ(disk_.Stat("..", &err), parent_stat_uncached);
  EXPECT_EQ("", err);
  EXPECT_EQ(disk_.Stat("subdir/subsubdir", &err),
            disk_.Stat("subdir/subsubdir/.", &err));
  EXPECT_EQ("", err);

  // Test error cases.
  string bad_path("cc:\\foo");
  EXPECT_EQ(-1, disk_.Stat(bad_path, &err));
  EXPECT_NE("", err); err.clear();
  EXPECT_EQ(-1, disk_.Stat(bad_path, &err));
  EXPECT_NE("", err); err.clear();
  EXPECT_EQ(0, disk_.Stat("nosuchfile", &err));
  EXPECT_EQ("", err);
  EXPECT_EQ(0, disk_.Stat("nosuchdir/nosuchfile", &err));
  EXPECT_EQ("", err);
}
#endif

TEST_F(DiskInterfaceTest, ReadFile) {
  string err;
  std::string content;
  ASSERT_EQ(DiskInterface::NotFound,
            disk_.ReadFile("foobar", &content, &err));
  EXPECT_EQ("", content);
  EXPECT_NE("", err); // actual value is platform-specific
  err.clear();

  const char* kTestFile = "testfile";
  FILE* f = fopen(kTestFile, "wb");
  ASSERT_TRUE(f);
  const char* kTestContent = "test content\nok";
  fprintf(f, "%s", kTestContent);
  ASSERT_EQ(0, fclose(f));

  ASSERT_EQ(DiskInterface::Okay,
            disk_.ReadFile(kTestFile, &content, &err));
  EXPECT_EQ(kTestContent, content);
  EXPECT_EQ("", err);
}

TEST_F(DiskInterfaceTest, MakeDirs) {
  string path = "path/with/double//slash/";
  EXPECT_TRUE(disk_.MakeDirs(path.c_str()));
  FILE* f = fopen((path + "a_file").c_str(), "w");
  EXPECT_TRUE(f);
  EXPECT_EQ(0, fclose(f));
#ifdef _WIN32
  string path2 = "another\\with\\back\\\\slashes\\";
  EXPECT_TRUE(disk_.MakeDirs(path2.c_str()));
  FILE* f2 = fopen((path2 + "a_file").c_str(), "w");
  EXPECT_TRUE(f2);
  EXPECT_EQ(0, fclose(f2));
#endif
}

TEST_F(DiskInterfaceTest, RemoveFile) {
  const char* kFileName = "file-to-remove";
  ASSERT_TRUE(Touch(kFileName));
  EXPECT_EQ(0, disk_.RemoveFile(kFileName));
  EXPECT_EQ(1, disk_.RemoveFile(kFileName));
  EXPECT_EQ(1, disk_.RemoveFile("does not exist"));
}

#ifndef _WIN32
TEST_F(DiskInterfaceTest, LoadZstdFile) {
  std::string payload = "rule cc\n  command = gcc $in -o $out\n";
  for (int i = 0; i < 1000; ++i)
    payload += "build out" + std::to_string(i) + ": cc in" + std::to_string(i) + "\n";

  // Streaming Zstd without content size in header.
  ZSTD_CCtx* cctx = ZSTD_createCCtx();
  std::string compressed(ZSTD_compressBound(payload.size()) + 64, '\0');
  ZSTD_inBuffer in1 = {payload.data(), payload.size() / 2, 0};
  ZSTD_inBuffer in2 = {payload.data() + in1.size, payload.size() - in1.size, 0};
  ZSTD_outBuffer out = {&compressed[0], compressed.size(), 0};
  ASSERT_FALSE(ZSTD_isError(ZSTD_compressStream2(cctx, &out, &in1, ZSTD_e_continue)));
  ASSERT_FALSE(ZSTD_isError(ZSTD_compressStream2(cctx, &out, &in2, ZSTD_e_end)));
  ZSTD_freeCCtx(cctx);
  compressed.resize(out.pos);

  const char* kFile = "build.ninja";
  FILE* f = fopen(kFile, "wb");
  ASSERT_TRUE(f != nullptr);
  ASSERT_EQ(compressed.size(), fwrite(compressed.data(), 1, compressed.size(), f));
  ASSERT_EQ(0, fclose(f));

  std::unique_ptr<LoadedFile> loaded;
  std::string err;
  ASSERT_EQ(FileReader::Okay, disk_.LoadFile(kFile, &loaded, &err));
  ASSERT_TRUE(loaded != nullptr);
  EXPECT_EQ(payload.size(), loaded->content().size());
  EXPECT_EQ(payload, loaded->content().AsString());
  EXPECT_EQ('\0', loaded->content_with_nul()[payload.size()]);

  // Streaming Zstd without trailer exceeding initial 4x + 64KB buffer capacity.
  std::string large_payload(256 * 1024, 'a');
  ZSTD_CCtx* cctx2 = ZSTD_createCCtx();
  std::string stream_only(ZSTD_compressBound(large_payload.size()) + 64, '\0');
  ZSTD_inBuffer sin1 = {large_payload.data(), large_payload.size() / 2, 0};
  ZSTD_inBuffer sin2 = {large_payload.data() + sin1.size, large_payload.size() - sin1.size, 0};
  ZSTD_outBuffer sout = {&stream_only[0], stream_only.size(), 0};
  ASSERT_FALSE(ZSTD_isError(ZSTD_compressStream2(cctx2, &sout, &sin1, ZSTD_e_continue)));
  ASSERT_FALSE(ZSTD_isError(ZSTD_compressStream2(cctx2, &sout, &sin2, ZSTD_e_end)));
  ZSTD_freeCCtx(cctx2);
  stream_only.resize(sout.pos);

  f = fopen(kFile, "wb");
  ASSERT_TRUE(f != nullptr);
  ASSERT_EQ(stream_only.size(), fwrite(stream_only.data(), 1, stream_only.size(), f));
  ASSERT_EQ(0, fclose(f));

  loaded.reset();
  err.clear();
  ASSERT_EQ(FileReader::Okay, disk_.LoadFile(kFile, &loaded, &err));
  ASSERT_TRUE(loaded != nullptr);
  EXPECT_EQ(large_payload.size(), loaded->content().size());
  EXPECT_EQ(large_payload, loaded->content().AsString());

  // Truncated Zstd frame must fail cleanly rather than returning partial output.
  f = fopen(kFile, "wb");
  ASSERT_TRUE(f != nullptr);
  ASSERT_EQ(stream_only.size() / 2, fwrite(stream_only.data(), 1, stream_only.size() / 2, f));
  ASSERT_EQ(0, fclose(f));

  loaded.reset();
  err.clear();
  EXPECT_EQ(FileReader::OtherError, disk_.LoadFile(kFile, &loaded, &err));
}

TEST_F(DiskInterfaceTest, LoadZstdMultiFrameParallel) {
  struct ScopedThreadCount {
    explicit ScopedThreadCount(int n) { SetThreadPoolThreadCount(n); }
    ~ScopedThreadCount() { SetThreadPoolThreadCount(1); }
  } threads(4);

  auto append_le32 = [](std::string* dst, uint32_t v) {
    dst->append(reinterpret_cast<const char*>(&v), sizeof(v));
  };

  // Build 8 frames (~256 KiB total) + 1 short (< 256 B) final frame without
  // Frame_Content_Size, followed by a Zstd Seekable Format seek table
  // (matching Soong's ninjazstd writer).
  std::string expected;
  std::string seekable_stream;
  struct SeekEntry {
    uint32_t c_size;
    uint32_t d_size;
  };
  std::vector<SeekEntry> entries;

  for (int i = 0; i < 8; ++i) {
    std::string chunk;
    while (chunk.size() < 32 * 1024) {
      chunk += "build out_" + std::to_string(i) + "_" +
               std::to_string(chunk.size()) + ": cc in\n";
    }
    std::string frame(ZSTD_compressBound(chunk.size()), '\0');
    size_t c_size =
        ZSTD_compress(&frame[0], frame.size(), chunk.data(), chunk.size(), 1);
    ASSERT_FALSE(ZSTD_isError(c_size));
    frame.resize(c_size);
    seekable_stream += frame;
    expected += chunk;
    entries.push_back(
        {static_cast<uint32_t>(c_size), static_cast<uint32_t>(chunk.size())});
  }

  // Final < 256 B frame omitting Frame_Content_Size in its header.
  std::string tail = "# final short frame\n";
  ZSTD_CCtx* cctx = ZSTD_createCCtx();
  ASSERT_TRUE(cctx != nullptr);
  ASSERT_FALSE(
      ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_contentSizeFlag, 0)));
  std::string tail_frame(ZSTD_compressBound(tail.size()) + 32, '\0');
  ZSTD_inBuffer tin = {tail.data(), tail.size(), 0};
  ZSTD_outBuffer tout = {&tail_frame[0], tail_frame.size(), 0};
  ASSERT_FALSE(ZSTD_isError(ZSTD_compressStream2(cctx, &tout, &tin, ZSTD_e_end)));
  ZSTD_freeCCtx(cctx);
  tail_frame.resize(tout.pos);
  ASSERT_EQ(ZSTD_CONTENTSIZE_UNKNOWN,
            ZSTD_getFrameContentSize(tail_frame.data(), tail_frame.size()));
  seekable_stream += tail_frame;
  expected += tail;
  entries.push_back(
      {static_cast<uint32_t>(tail_frame.size()), static_cast<uint32_t>(tail.size())});

  auto append_seek_table = [&](std::string* dst,
                               const std::vector<SeekEntry>& tbl) {
    uint32_t payload = static_cast<uint32_t>(8 * tbl.size() + 9);
    append_le32(dst, 0x184D2A5EU);
    append_le32(dst, payload);
    for (const auto& e : tbl) {
      append_le32(dst, e.c_size);
      append_le32(dst, e.d_size);
    }
    append_le32(dst, static_cast<uint32_t>(tbl.size()));
    dst->push_back('\0');  // Seek_Table_Descriptor: no checksums.
    append_le32(dst, 0x8F92EAB1U);
  };
  append_seek_table(&seekable_stream, entries);

  const char* kFile = "multiframe.ninja";
  auto write_bin = [&](const std::string& data) {
    FILE* f = fopen(kFile, "wb");
    ASSERT_TRUE(f != nullptr);
    ASSERT_EQ(data.size(), fwrite(data.data(), 1, data.size(), f));
    ASSERT_EQ(0, fclose(f));
  };

  write_bin(seekable_stream);
  std::unique_ptr<LoadedFile> loaded;
  std::string err;
  ASSERT_EQ(FileReader::Okay, disk_.LoadFile(kFile, &loaded, &err));
  ASSERT_TRUE(loaded != nullptr);
  EXPECT_EQ(expected.size(), loaded->content().size());
  EXPECT_EQ(expected, loaded->content().AsString());
  EXPECT_EQ('\0', loaded->content_with_nul()[expected.size()]);

  // Multi-frame Zstd without a seek table (Frame_Content_Size in headers +
  // interleaved skippable metadata frame).
  std::string no_seek_stream;
  std::string no_seek_expected;
  for (int i = 0; i < 6; ++i) {
    std::string chunk(20 * 1024, static_cast<char>('A' + i));
    std::string frame(ZSTD_compressBound(chunk.size()), '\0');
    size_t c_size =
        ZSTD_compress(&frame[0], frame.size(), chunk.data(), chunk.size(), 1);
    ASSERT_FALSE(ZSTD_isError(c_size));
    no_seek_stream.append(frame.data(), c_size);
    no_seek_expected += chunk;
    if (i == 2) {
      append_le32(&no_seek_stream, 0x184D2A53U);
      append_le32(&no_seek_stream, 4U);
      append_le32(&no_seek_stream, 0xDEADBEEFU);
    }
  }
  write_bin(no_seek_stream);
  loaded.reset();
  err.clear();
  ASSERT_EQ(FileReader::Okay, disk_.LoadFile(kFile, &loaded, &err));
  ASSERT_TRUE(loaded != nullptr);
  EXPECT_EQ(no_seek_expected, loaded->content().AsString());

  // Empty 0-byte Zstd frame + seek table.
  std::string empty_frame(ZSTD_compressBound(0), '\0');
  size_t empty_csize = ZSTD_compress(&empty_frame[0], empty_frame.size(), "", 0, 1);
  ASSERT_FALSE(ZSTD_isError(empty_csize));
  empty_frame.resize(empty_csize);
  append_seek_table(&empty_frame, {{static_cast<uint32_t>(empty_csize), 0U}});
  write_bin(empty_frame);
  loaded.reset();
  err.clear();
  ASSERT_EQ(FileReader::Okay, disk_.LoadFile(kFile, &loaded, &err));
  ASSERT_TRUE(loaded != nullptr);
  EXPECT_EQ(0u, loaded->content().size());
  EXPECT_EQ('\0', loaded->content_with_nul()[0]);

  // Mismatched decompressed size in seek table must fail cleanly.
  std::vector<SeekEntry> bad_entries = entries;
  bad_entries[3].d_size += 17;
  std::string bad_seek_stream =
      seekable_stream.substr(0, seekable_stream.size() - (8 + 8 * entries.size() + 9));
  append_seek_table(&bad_seek_stream, bad_entries);
  write_bin(bad_seek_stream);
  loaded.reset();
  err.clear();
  EXPECT_EQ(FileReader::OtherError, disk_.LoadFile(kFile, &loaded, &err));
}
#endif

struct StatTest : public StateTestWithBuiltinRules,
                  public DiskInterface {
  StatTest() : scan_(&state_, NULL, NULL, this, NULL, false) {}

  // DiskInterface implementation.
  virtual TimeStamp Stat(const string& path, string* err) const;
  virtual TimeStamp LStat(const string& path, bool* is_dir, bool* is_symlink, string* err) const;
  virtual bool IsStatThreadSafe() const;
  virtual bool WriteFile(const string& path, const string& contents) {
    assert(false);
    return true;
  }
  virtual bool MakeDir(const string& path) {
    assert(false);
    return false;
  }
  virtual Status ReadFile(const string& path, string* contents, string* err) {
    assert(false);
    return NotFound;
  }
  virtual Status LoadFile(const std::string& path,
                          std::unique_ptr<LoadedFile>* result,
                          std::string* err) {
    assert(false);
    return NotFound;
  }
  virtual bool Getcwd(std::string* out_path, std::string* err) {
    assert(false);
    return false;
  }
  virtual bool Chdir(const std::string dir, std::string* err) {
    assert(false);
    return false;
  }
  virtual int RemoveFile(const string& path) {
    assert(false);
    return 0;
  }

  DependencyScan scan_;
  map<string, TimeStamp> mtimes_;
  mutable vector<string> stats_;
};

TimeStamp StatTest::Stat(const string& path, string* err) const {
  return LStat(path, nullptr, nullptr, err);
}

TimeStamp StatTest::LStat(const string& path, bool* is_dir, bool* is_symlink, string* err) const {
  stats_.push_back(path);
  map<string, TimeStamp>::const_iterator i = mtimes_.find(path);
  if (i == mtimes_.end())
    return 0;  // File not found.
  if (is_dir != nullptr)
    *is_dir = false;
  if (is_symlink != nullptr)
    *is_symlink = false;
  return i->second;
}

bool StatTest::IsStatThreadSafe() const {
  return false;
}

TEST_F(StatTest, Simple) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(&state_,
"build out: cat in\n"));

  Node* out = GetNode("out");
  string err;
  EXPECT_TRUE(out->Stat(this, &err));
  EXPECT_EQ("", err);
  ASSERT_EQ(1u, stats_.size());
  scan_.RecomputeDirty(out, NULL, NULL);
  ASSERT_EQ(2u, stats_.size());
  ASSERT_EQ("out", stats_[0]);
  ASSERT_EQ("in",  stats_[1]);
}

TEST_F(StatTest, TwoStep) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(&state_,
"build out: cat mid\n"
"build mid: cat in\n"));

  Node* out = GetNode("out");
  string err;
  EXPECT_TRUE(out->Stat(this, &err));
  EXPECT_EQ("", err);
  ASSERT_EQ(1u, stats_.size());
  scan_.RecomputeDirty(out, NULL, NULL);
  ASSERT_EQ(3u, stats_.size());
  ASSERT_EQ("out", stats_[0]);
  ASSERT_TRUE(GetNode("out")->dirty());
  ASSERT_EQ("mid",  stats_[1]);
  ASSERT_TRUE(GetNode("mid")->dirty());
  ASSERT_EQ("in",  stats_[2]);
}

TEST_F(StatTest, Tree) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(&state_,
"build out: cat mid1 mid2\n"
"build mid1: cat in11 in12\n"
"build mid2: cat in21 in22\n"));

  Node* out = GetNode("out");
  string err;
  EXPECT_TRUE(out->Stat(this, &err));
  EXPECT_EQ("", err);
  ASSERT_EQ(1u, stats_.size());
  scan_.RecomputeDirty(out, NULL, NULL);
  ASSERT_EQ(1u + 6u, stats_.size());
  ASSERT_EQ("mid1", stats_[1]);
  ASSERT_TRUE(GetNode("mid1")->dirty());
  ASSERT_EQ("in11", stats_[2]);
}

TEST_F(StatTest, Middle) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(&state_,
"build out: cat mid\n"
"build mid: cat in\n"));

  mtimes_["in"] = 1;
  mtimes_["mid"] = 0;  // missing
  mtimes_["out"] = 1;

  Node* out = GetNode("out");
  string err;
  EXPECT_TRUE(out->Stat(this, &err));
  EXPECT_EQ("", err);
  ASSERT_EQ(1u, stats_.size());
  scan_.RecomputeDirty(out, NULL, NULL);
  ASSERT_FALSE(GetNode("in")->dirty());
  ASSERT_TRUE(GetNode("mid")->dirty());
  ASSERT_TRUE(GetNode("out")->dirty());
}

}  // namespace
