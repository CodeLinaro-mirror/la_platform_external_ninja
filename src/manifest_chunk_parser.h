// Copyright 2018 Google Inc. All Rights Reserved.
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

#ifndef NINJA_MANIFEST_CHUNK_PARSER_H_
#define NINJA_MANIFEST_CHUNK_PARSER_H_

#include <stdlib.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "disk_interface.h"
#include "eval_env.h"
#include "graph.h"
#include "state.h"
#include "string_piece.h"

struct Lexer;

namespace manifest_chunk {

struct Error {
  std::string msg_;
};

struct RequiredVersion {
  StringPiece version_;
};

struct Include {
  LexedPath path_;
  LexedPath chdir_;
  std::string chdir_plus_slash_;
  bool new_scope_ = false;
  size_t diag_pos_ = 0;

  std::unordered_map<std::string, std::string> envvar;
};

struct DefaultTarget {
  RelativePosition pos_;
  LexedPath parsed_path_;
  size_t diag_pos_ = 0;
};

/// A group of manifest declarations generated while parsing a chunk.
struct Clump {
  Clump(const LoadedFile& file) : file_(file) {}

  const LoadedFile& file_;
  BasePosition pos_;

  std::vector<Binding*> bindings_;
  std::vector<Rule*> rules_;
  std::vector<Pool*> pools_;
  std::vector<Edge*> edges_;
  std::vector<DefaultTarget*> default_targets_;
  std::vector<Scope*> owner_scope_;

  /// A count of non-implicit outputs across all edges.
  size_t edge_output_count_ = 0;

  DeclIndex decl_count() const { return next_index_; }

  /// Allocate an index within the clump. Once the parallelized chunk parsing is
  /// finished, each clump's base position will be computed, giving every clump
  /// item both a DFS "depth-first-search" position and a position within the
  /// tree of scopes.
  RelativePosition AllocNextPos() { return { &pos_, next_index_++ }; }

private:
  DeclIndex next_index_ = 0;
};

/// This class could be replaced with std::variant from C++17.
struct ParserItem {
  enum Kind {
    kError, kRequiredVersion, kInclude, kClump
  };

  Kind kind;

  union {
    Error* error;
    RequiredVersion* required_version;
    Include* include;
    Clump* clump;
  } u;

  ParserItem(Error* val)            : kind(kError)            { u.error             = val; }
  ParserItem(RequiredVersion* val)  : kind(kRequiredVersion)  { u.required_version  = val; }
  ParserItem(Include* val)          : kind(kInclude)          { u.include           = val; }
  ParserItem(Clump* val)            : kind(kClump)            { u.clump             = val; }
};

/// Parse a chunk of a manifest. If the parser encounters a syntactic error, the
/// final item will be an error object.
void ParseChunk(const LoadedFile& file, StringPiece chunk_content,
                std::vector<ParserItem>* out, bool experimentalEnvvar);

/// Split the input into chunks of declarations.
std::vector<StringPiece> SplitManifestIntoChunks(StringPiece input);

} // namespace manifest_chunk

#endif  // NINJA_MANIFEST_CHUNK_PARSER_H_
