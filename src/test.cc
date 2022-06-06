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

#ifdef _WIN32
#include <direct.h>  // Has to be before util.h is included.
#endif

#include "test.h"

#include <algorithm>

#include <errno.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "build_log.h"
#include "graph.h"
#include "manifest_parser.h"
#include "util.h"

namespace {

#ifdef _WIN32
#ifndef _mktemp_s
/// mingw has no mktemp.  Implement one with the same type as the one
/// found in the Windows API.
int _mktemp_s(char* templ) {
  char* ofs = strchr(templ, 'X');
  sprintf(ofs, "%d", rand() % 1000000);
  return 0;
}
#endif

/// Windows has no mkdtemp.  Implement it in terms of _mktemp_s.
char* mkdtemp(char* name_template) {
  int err = _mktemp_s(name_template);
  if (err < 0) {
    perror("_mktemp_s");
    return NULL;
  }

  err = _mkdir(name_template);
  if (err < 0) {
    perror("mkdir");
    return NULL;
  }

  return name_template;
}
#endif  // _WIN32

string GetSystemTempDir() {
#ifdef _WIN32
  char buf[1024];
  if (!GetTempPath(sizeof(buf), buf))
    return "";
  return buf;
#else
  const char* tempdir = getenv("TMPDIR");
  if (tempdir)
    return tempdir;
  return "/tmp";
#endif
}

}  // anonymous namespace

StateTestWithBuiltinRules::StateTestWithBuiltinRules() {
  AddCatRule(&state_);
}

void StateTestWithBuiltinRules::AddCatRule(State* state) {
  AssertParse(state,
"rule cat\n"
"  command = cat $in > $out\n");
}

Node* StateTestWithBuiltinRules::GetNode(const string& path) {
  EXPECT_FALSE(strpbrk(path.c_str(), "/\\"));
  return state_.GetNode(state_.root_scope_.GlobalPath(path), 0);
}

void AssertParse(State* state, const char* input,
                 ManifestParserOptions opts) {
  ManifestParser parser(state, NULL, opts);
  string err;
  EXPECT_TRUE(parser.ParseTest(input, &err));
  ASSERT_EQ("", err);
  VerifyGraph(*state);
}

void AssertHash(const char* expected, uint64_t actual) {
  ASSERT_EQ(BuildLog::LogEntry::HashCommand(expected), actual);
}

void VerifyGraph(const State& state) {
  std::set<Node*> edge_nodes;
  std::map<Node*, Edge*> expected_node_input;
  std::map<Node*, std::set<Edge*>> expected_node_outputs;

  // An edge ID is always equal to its index in the global edge table.
  for (size_t idx = 0; idx < state.edges_.size(); ++idx)
    EXPECT_EQ(idx, state.edges_[idx]->id_);

  // Check that each edge is valid.
  for (Edge* e : state.edges_) {
    // All edges need at least one output.
    EXPECT_FALSE(e->outputs_.empty());
    // Check that the edge's node counts are correct.
    EXPECT_GE(e->explicit_outs_, 0);
    EXPECT_GE(e->implicit_outs_, 0);
    EXPECT_GE(e->explicit_deps_, 0);
    EXPECT_GE(e->implicit_deps_, 0);
    EXPECT_GE(e->order_only_deps_, 0);
    EXPECT_EQ(static_cast<size_t>(e->explicit_outs_ + e->implicit_outs_),
              e->outputs_.size());
    EXPECT_EQ(static_cast<size_t>(e->explicit_deps_ + e->implicit_deps_ +
                                  e->order_only_deps_),
              e->inputs_.size());
    // Record everything seen in edges so we can check it against the nodes.
    for (Node* n : e->inputs_) {
      edge_nodes.insert(n);
      expected_node_outputs[n].insert(e);
    }
    for (Node* n : e->outputs_) {
      edge_nodes.insert(n);
      EXPECT_EQ(nullptr, expected_node_input[n]);
      expected_node_input[n] = e;
    }
  }

  // Verify that any node used by an edge is in the node table.
  //
  // N.B. It's OK to have nodes that aren't referenced by any edge. The code
  // below will verify that such nodes have no in/out edges. This situation can
  // happen in a couple of ways:
  //  - Loading the depslog creates a node for every path in the log, but the
  //    implicit edge<->node links are only created as ninja scans targets for
  //    dirty nodes.
  //  - Removing an invalid edge for dupbuild=warn doesn't remove the input
  //    nodes.
  for (Node* n : edge_nodes)
    EXPECT_EQ(n, state.LookupNode(n->globalPath()));

  // Check each node against the set of edges.
  for (const auto& p : state.paths_) {
    Node* n = p.second;
    auto out_edges_vec = n->GetOutEdges();
    std::set<Edge*> out_edges(out_edges_vec.begin(), out_edges_vec.end());
    EXPECT_EQ(n, state.LookupNode(n->globalPath()));
    EXPECT_EQ(expected_node_outputs[n], out_edges);
    EXPECT_EQ(expected_node_input[n], n->in_edge());
  }
}

void VirtualFileSystem::Create(const string& path,
                               const string& contents) {
  string fullpath = cwd_ + path;
  files_[fullpath].mtime = now_;
  files_[fullpath].contents = contents;
  files_created_.insert(fullpath);
}

void VirtualFileSystem::CreateSymlink(const string& path,
                                      const string& dest) {
  string fullpath = cwd_ + path;
  files_[fullpath].mtime = now_;
  files_[fullpath].contents = dest;
  files_[fullpath].is_symlink = true;
  files_created_.insert(fullpath);
}

TimeStamp VirtualFileSystem::Stat(const string& path, string* err) const {
  string fullpath = cwd_ + path;
  DirMap::const_iterator d = dirs_.find(fullpath);
  if (d != dirs_.end()) {
    *err = d->second.stat_error;
    return d->second.mtime;
  }
  FileMap::const_iterator i = files_.find(fullpath);
  if (i != files_.end()) {
    if (i->second.is_symlink) {
      return Stat(i->second.contents, err);
    }
    *err = i->second.stat_error;
    return i->second.mtime;
  }
  return 0;
}

TimeStamp VirtualFileSystem::LStat(const string& path, bool* is_dir, bool* is_symlink, string* err) const {
  string fullpath = cwd_ + path;
  DirMap::const_iterator d = dirs_.find(fullpath);
  if (d != dirs_.end()) {
    if (is_dir != nullptr)
      *is_dir = true;
    *err = d->second.stat_error;
    return d->second.mtime;
  }
  FileMap::const_iterator i = files_.find(fullpath);
  if (i != files_.end()) {
    if (is_dir != nullptr)
      *is_dir = false;
    if (is_symlink != nullptr)
      *is_symlink = i->second.is_symlink;
    *err = i->second.stat_error;
    return i->second.mtime;
  }
  return 0;
}

bool VirtualFileSystem::IsStatThreadSafe() const {
  return true;
}

bool VirtualFileSystem::WriteFile(const string& path, const string& contents) {
  string fullpath = cwd_ + path;
  if (files_.find(fullpath) == files_.end()) {
    if (dirs_.find(fullpath) != dirs_.end())
      return false;

    string::size_type slash_pos = fullpath.find_last_of("/");
    if (slash_pos != string::npos) {
      DirMap::iterator d = dirs_.find(fullpath.substr(0, slash_pos));
      if (d != dirs_.end()) {
        d->second.mtime = now_;
      } else {
        return false;
      }
    }
  }

  Create(path, contents);
  return true;
}

bool VirtualFileSystem::MakeDir(const string& path) {
  string fullpath = cwd_ + path;
  if (dirs_.find(fullpath) != dirs_.end())
    return true;
  if (files_.find(fullpath) != files_.end())
    return false;

  string::size_type slash_pos = fullpath.find_last_of("/");
  if (slash_pos != string::npos) {
    DirMap::iterator d = dirs_.find(fullpath.substr(0, slash_pos));
    if (d != dirs_.end()) {
      d->second.mtime = now_;
    } else {
      return false;
    }
  }

  dirs_[fullpath].mtime = now_;
  directories_made_.push_back(fullpath);
  return true;  // success
}

FileReader::Status VirtualFileSystem::ReadFile(const string& path,
                                               string* contents,
                                               string* err) {
  // Delegate to the more-general LoadFile.
  std::unique_ptr<LoadedFile> file;
  Status result = LoadFile(path, &file, err);
  if (result == Okay) {
    *contents = file->content().AsString();
  }
  return result;
}

FileReader::Status VirtualFileSystem::LoadFile(const std::string& path,
                                               std::unique_ptr<LoadedFile>* result,
                                               std::string* err) {
  string fullpath = cwd_ + path;
  files_read_.push_back(fullpath);
  FileMap::iterator i = files_.find(fullpath);
  if (i != files_.end()) {
    if (i->second.is_symlink) {
      return LoadFile(i->second.contents, result, err);
    }
    std::string& contents = i->second.contents;
    *result = std::unique_ptr<HeapLoadedFile>(new HeapLoadedFile(path,
                                                                 contents));
    return Okay;
  }
  *err = strerror(ENOENT);
  return NotFound;
}

bool VirtualFileSystem::Getcwd(std::string* out_path, std::string* err) {
  // Empty cwd_ means the current dir is '/'
  if (cwd_.empty()) {
    out_path->assign("/");
  } else {
    out_path->assign(cwd_);
    // Strip off '/' if present.
    if (cwd_.size() > 0 && cwd_.at(cwd_.size() - 1) == '/') {
      out_path->erase(out_path->end() - 1);
    }
  }
  err->clear();
  return true;
}

bool VirtualFileSystem::Chdir(const std::string dir, std::string* err) {
  // VirtualFileSystem::Chdir does not support ".." or "." relative paths.
  // However, simple relative paths are ok. And absolute paths are ok.

  string dest;
  if (dir.empty()) {
    err->assign("VirtualFileSystem::Chdir does not accept the empty string");
    return false;
  } else if (dir == "/") {
    cwd_.clear();
    err->clear();
    return true;
  } else if (dir.at(0) == '/') {
    // Treat path as absolute.
    dest.insert(dest.begin(), dir.begin() + 1, dir.end());
  } else {
    // Treat path as relative.
    dest = cwd_ + dir;
  }
  if (dest.find(".") != string::npos) {
    err->assign("VirtualFileSystem::Chdir does not accept . or ..");
    return false;
  }
  if (dest.size() > 1 && dest.at(dest.size() - 1) == '/') {
    dest.erase(dest.size() - 1);
  }

  // Look up dest in directories_made_.
  vector<string>::iterator i = directories_made_.begin();
  for (; i != directories_made_.end(); i++) {
    if (*i == dest) {
      cwd_ = dest;
      cwd_ += '/';
      err->clear();
      return true;
    }
  }
  *err = strerror(ENOENT);
  return false;
}

int VirtualFileSystem::RemoveFile(const string& path) {
  string fullpath = cwd_ + path;
  if (dirs_.find(fullpath) != dirs_.end())
    return -1;
  FileMap::iterator i = files_.find(fullpath);
  if (i != files_.end()) {
    string::size_type slash_pos = fullpath.find_last_of("/");
    if (slash_pos != string::npos) {
      DirMap::iterator d = dirs_.find(fullpath.substr(0, slash_pos));
      if (d != dirs_.end()) {
        d->second.mtime = now_;
      }
    }

    files_.erase(i);
    files_removed_.insert(path);
    return 0;
  } else {
    return 1;
  }
}

void ScopedTempDir::CreateAndEnter(const string& name) {
  // First change into the system temp dir and save it for cleanup.
  start_dir_ = GetSystemTempDir();
  if (start_dir_.empty())
    Fatal("couldn't get system temp dir");
  if (chdir(start_dir_.c_str()) < 0)
    Fatal("chdir: %s", strerror(errno));

  // Create a temporary subdirectory of that.
  char name_template[1024];
  strcpy(name_template, name.c_str());
  strcat(name_template, "-XXXXXX");
  char* tempname = mkdtemp(name_template);
  if (!tempname)
    Fatal("mkdtemp: %s", strerror(errno));
  temp_dir_name_ = tempname;

  // chdir into the new temporary directory.
  if (chdir(temp_dir_name_.c_str()) < 0)
    Fatal("chdir: %s", strerror(errno));
}

void ScopedTempDir::Cleanup() {
  if (temp_dir_name_.empty())
    return;  // Something went wrong earlier.

  // Move out of the directory we're about to clobber.
  if (chdir(start_dir_.c_str()) < 0)
    Fatal("chdir: %s", strerror(errno));

#ifdef _WIN32
  string command = "rmdir /s /q " + temp_dir_name_;
#else
  string command = "rm -rf " + temp_dir_name_;
#endif
  if (system(command.c_str()) < 0)
    Fatal("system: %s", strerror(errno));

  temp_dir_name_.clear();
}
