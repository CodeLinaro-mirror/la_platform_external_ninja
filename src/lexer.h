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

#ifndef NINJA_LEXER_H_
#define NINJA_LEXER_H_

#include <assert.h>

#include <string>

#include "string_piece.h"
#include "util.h"

// Windows may #define ERROR.
#ifdef ERROR
#undef ERROR
#endif

struct Edge;
struct EdgeEval;
struct EvalString;
struct LexedPath;
struct ScopePosition;
struct Scope;

/// Search the manifest for the next acceptable point to split the manifest.
/// Assuming the manifest is syntactically valid to the returned position, the
/// chunk parser will finish parsing a declaration just before the returned
/// position.
size_t AdvanceToNextManifestChunk(StringPiece content, size_t idx);

bool DecorateErrorWithLocation(const std::string& filename,
                               const char* file_start,
                               size_t file_offset,
                               const std::string& message,
                               std::string* err);

struct Lexer {
  /// Ordinary lexer constructor.
  Lexer(const std::string& filename, StringPiece file_content, const char* pos)
      : filename_(filename), input_(file_content), ofs_(pos) {
    assert(pos >= &file_content[0] &&
           pos <= file_content.data() + file_content.size());
  }

  /// Helper ctor useful for tests.
  explicit Lexer(const char* input) : Lexer("input", input, input) {}

  enum Token {
    ERROR,
    BUILD,
    CHDIR,
    COLON,
    DEFAULT,
    EQUALS,
    IDENT,
    INCLUDE,
    INDENT,
    NEWLINE,
    PIPE,
    PIPE2,
    PIPEAT,
    POOL,
    RULE,
    SUBNINJA,
    TNUL,
    TEOF,
  };

  /// Return a human-readable form of a token, used in error messages.
  static const char* TokenName(Token t);

  /// Return a human-readable token hint, used in error messages.
  static const char* TokenErrorHint(Token expected);

  /// If the last token read was an ERROR token, provide more info
  /// or the empty string.
  std::string DescribeLastError();

  // XXX: Decide whether to use char* or size_t to represent lexer position.
  // (We're using size_t for diagnostic reporting -- GetLastTokenOffset, but
  // we're using char* for checking for the end-of-chunk.)
  const char* GetPos() { return ofs_; }
  void ResetPos(const char* pos) { ofs_ = pos; last_token_ = nullptr; }

  size_t GetLastTokenOffset() {
    return last_token_ ? last_token_ - input_.data() : 0;
  }

  /// Read a Token from the Token enum.
  Token ReadToken();

  /// Rewind to the last read Token.
  void UnreadToken();

  /// If the next token is a binding's indentation, this function reads it and
  /// returns true. This function skips lines containing only a comment, which
  /// are allowed in a block of indented bindings. (A completely blank line, on
  /// the other hand, ends the block.)
  bool PeekIndent();

  /// If the next token is \a token, read it and return true.
  bool PeekToken(Token token);

  /// Read a simple identifier (a rule or variable name).
  /// Returns false if a name can't be read.
  bool ReadIdent(StringPiece* out);

  /// Parse the value in a "var = value" let binding and return the unprocessed
  /// span of the value (escapes and variable expansions are left as-is). The
  /// binding must end with a newline, which is consumed.
  ///
  /// The returned StringPiece includes the EOL terminator, "\r\n" or "\n".
  /// Copying the StringPiece copies the terminator.
  bool ReadBindingValue(StringPiece* out, std::string* err);

  /// Try to parse a path from the manifest that's already canonical and doesn't
  /// need evaluation. Returns the string if successful and return a 0-size
  /// string otherwise. This function is a performance optimization.
  StringPiece PeekCanonicalPath();

  /// Read a ninja path (e.g. build/include declarations) and output a LexedPath
  /// object with the position of the path in the loaded file's buffer. If there
  /// is no path before the lexer sees a delimiter (e.g. space, newline, colon),
  /// it returns true and outputs an empty string.
  ///
  /// On error, the function returns false and writes an error to *err.
  ///
  /// The delimiter is not included in the path's StringPiece, and (except for
  /// space characters), the delimiter is left in the lexer's input stream.
  ///
  /// Because path evaluation requires the delimiter's presence, the path should
  /// be evaluated before its underlying memory is freed, and the StringPiece
  /// should not be duplicated. (e.g. Don't copy it into an std::string first.)
  bool ReadPath(LexedPath* out, std::string* err);

  /// Construct an error message with context.
  bool Error(const std::string& message, std::string* err);

private:
  /// Return the end of the file input (which points at a NUL byte).
  const char* EndOfFile() { return input_.data() + input_.size(); }

  /// Report an error on a NUL character, which could indicate EOF, but could
  /// also be a stray NUL character in the file's content.
  bool UnexpectedNulError(const char* pos, std::string* err);

  /// Skip past whitespace (called after each read token/ident/etc.).
  void EatWhitespace();

  std::string filename_;
  StringPiece input_;
  const char* ofs_ = nullptr;
  const char* last_token_ = nullptr;
};

void EvaluateBindingInScope(std::string* out_append,
                            StringPiece value,
                            ScopePosition pos);

bool EvaluateBindingOnRule(std::string* out_append,
                           StringPiece value,
                           EdgeEval* edge_eval, Scope* target,
                           std::string* err);

/// Used for lexer tests.
std::string EvaluateBindingForTesting(StringPiece value);

void EvaluatePathInScope(std::string* out_append, const LexedPath& path,
                         ScopePosition pos);

void EvaluatePathOnEdge(std::string* out_append, const LexedPath& path,
                        const Edge& edge);

/// Used for lexer tests.
std::string EvaluatePathForTesting(const LexedPath& path);

#endif // NINJA_LEXER_H_
