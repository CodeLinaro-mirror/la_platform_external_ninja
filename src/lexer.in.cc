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

#include "lexer.h"

#include <stdio.h>

#include "eval_env.h"
#include "graph.h"
#include "util.h"

size_t AdvanceToNextManifestChunk(StringPiece content, size_t idx) {
  assert(idx <= content.size());

  // Iterate over each LF in the manifest, starting at the given index.
  while (true) {
    const void* next_line = memchr(content.data() + idx, '\n',
                                   content.size() - idx);
    if (next_line == nullptr) {
      break;
    }
    idx = static_cast<const char*>(next_line) - content.data();
    ++idx; // step over the LF

    // The line must not be preceded by a line continuator. This logic can
    // filter out more split candidates than strictly necessary:
    //  - The preceding line could have a comment that ends with a "$": "# $\n"
    //  - The preceding line could end with an escaped-dollar: "X=$$\n"
    if ((idx >= 2 && content.substr(idx - 2, 2) == "$\n") ||
        (idx >= 3 && content.substr(idx - 3, 3) == "$\r\n")) {
      continue;
    }

    // Skip an indented line or a comment line, either of which could be part of
    // an earlier declaration. Ninja allows unindented comments (as well as
    // indented comments) inside a binding block, e.g.:
    //
    //   build foo: cc
    //   # comment-line
    //     pool = link_pool
    //
    // Ninja doesn't allow blank lines in a binding block. This code could
    // probably allow a chunk to start with a blank line, but it seems better if
    // it doesn't.
    if (idx >= content.size() ||
        content[idx] == ' ' || content[idx] == '#' ||
        content[idx] == '\r' || content[idx] == '\n') {
      continue;
    }

    return idx;
  }

  return content.size();
}

bool DecorateErrorWithLocation(const std::string& filename,
                               const char* file_start,
                               size_t file_offset,
                               const std::string& message,
                               std::string* err) {
  // Make a copy in case message and err alias.
  std::string message_tmp = message;

  // Compute line/column.
  int line = 1;
  const char* line_start = file_start;
  const char* file_pos = file_start + file_offset;

  for (const char* p = line_start; p < file_pos; ++p) {
    if (*p == '\n') {
      ++line;
      line_start = p + 1;
    }
  }
  int col = (int)(file_pos - line_start);

  char buf[1024];
  snprintf(buf, sizeof(buf), "%s:%d: ", filename.c_str(), line);
  *err = buf;
  *err += message_tmp + "\n";

  // Add some context to the message.
  const int kTruncateColumn = 72;
  if (col > 0 && col < kTruncateColumn) {
    int len;
    bool truncated = true;
    for (len = 0; len < kTruncateColumn; ++len) {
      if (line_start[len] == 0 || line_start[len] == '\n') {
        truncated = false;
        break;
      }
    }
    *err += string(line_start, len);
    if (truncated)
      *err += "...";
    *err += "\n";
    *err += string(col, ' ');
    *err += "^ near here";
  }

  return false;
}

bool Lexer::Error(const std::string& message, std::string* err) {
  return DecorateErrorWithLocation(filename_, input_.data(),
                                   GetLastTokenOffset(), message, err);
}

bool Lexer::UnexpectedNulError(const char* pos, std::string* err) {
  assert(*pos == '\0');
  const char* msg = (pos == EndOfFile()) ? "unexpected EOF"
                                         : "unexpected NUL byte";
  return Error(msg, err);
}

const char* Lexer::TokenName(Token t) {
  switch (t) {
  case ERROR:    return "lexing error";
  case BUILD:    return "'build'";
  case CHDIR:    return "'chdir'";
  case COLON:    return "':'";
  case DEFAULT:  return "'default'";
  case EQUALS:   return "'='";
  case IDENT:    return "identifier";
  case INCLUDE:  return "'include'";
  case INDENT:   return "indent";
  case NEWLINE:  return "newline";
  case PIPE2:    return "'||'";
  case PIPE:     return "'|'";
  case PIPEAT:   return "'|@'";
  case POOL:     return "'pool'";
  case RULE:     return "'rule'";
  case SUBNINJA: return "'subninja'";
  case TNUL:     return "nul byte";
  case TEOF:     return "eof";
  }
  return NULL;  // not reached
}

const char* Lexer::TokenErrorHint(Token expected) {
  switch (expected) {
  case COLON:
    return " ($ also escapes ':')";
  default:
    return "";
  }
}

string Lexer::DescribeLastError() {
  if (last_token_) {
    switch (last_token_[0]) {
    case '\t':
      return "tabs are not allowed, use spaces";
    }
  }
  return "lexing error";
}

void Lexer::UnreadToken() {
  ofs_ = last_token_;
}

Lexer::Token Lexer::ReadToken() {
  const char* p = ofs_;
  const char* q;
  const char* r;
  const char* start;
  Lexer::Token token;
  for (;;) {
    start = p;
    /*!re2c
    re2c:define:YYCTYPE = "unsigned char";
    re2c:define:YYCURSOR = p;
    re2c:define:YYMARKER = q;
    re2c:define:YYCTXMARKER = r;
    re2c:yyfill:enable = 0;

    nul = "\000";
    simple_varname = [a-zA-Z0-9_-]+;
    varname = [a-zA-Z0-9_.-]+;
    eol = "\n" | "\r\n";
    comment = "#"[^\000\r\n]*;

    [ ]*comment / eol { continue; }
    [ ]*eol    { token = NEWLINE;  break; }
    [ ]+       { token = INDENT;   break; }
    "build"    { token = BUILD;    break; }
    "pool"     { token = POOL;     break; }
    "rule"     { token = RULE;     break; }
    "chdir"    { token = CHDIR;    break; }
    "default"  { token = DEFAULT;  break; }
    "="        { token = EQUALS;   break; }
    ":"        { token = COLON;    break; }
    "|@"       { token = PIPEAT;   break; }
    "||"       { token = PIPE2;    break; }
    "|"        { token = PIPE;     break; }
    "include"  { token = INCLUDE;  break; }
    "subninja" { token = SUBNINJA; break; }
    varname    { token = IDENT;    break; }
    nul        { token = (start == EndOfFile()) ? TEOF : TNUL; break; }
    [^]        { token = ERROR;    break; }
    */
  }

  last_token_ = start;
  ofs_ = p;
  if (token != NEWLINE && token != TEOF)
    EatWhitespace();
  return token;
}

bool Lexer::PeekIndent() {
  const char* p = ofs_;
  const char* q;
  const char* start;
  for (;;) {
    start = p;
    /*!re2c
    [ ]*comment eol { continue; }
    [ ]*eol         { last_token_ = ofs_ = start; return false; }
    [ ]+            { last_token_ = start; ofs_ = p; return true; }
    [^]             { last_token_ = ofs_ = start; return false; }
    */
  }
}

bool Lexer::PeekToken(Token token) {
  Token t = ReadToken();
  if (t == token)
    return true;
  UnreadToken();
  return false;
}

void Lexer::EatWhitespace() {
  const char* p = ofs_;
  const char* q;
  for (;;) {
    ofs_ = p;
    /*!re2c
    [ ]+    { continue; }
    "$" eol { continue; }
    nul     { break; }
    [^]     { break; }
    */
  }
}

bool Lexer::ReadIdent(StringPiece* out) {
  const char* p = ofs_;
  const char* start;
  for (;;) {
    start = p;
    /*!re2c
    varname {
      *out = StringPiece(start, p - start);
      break;
    }
    [^] {
      last_token_ = start;
      return false;
    }
    */
  }
  last_token_ = start;
  ofs_ = p;
  EatWhitespace();
  return true;
}

bool Lexer::ReadBindingValue(StringPiece* out, string* err) {
  const char* p = ofs_;
  const char* q;
  const char* start;
  for (;;) {
    start = p;
    /*!re2c
    ( [^$\r\n\000]
        | "$" [$: ]
        | "$" eol
        | "${" varname "}"
        | "$" simple_varname )+ {
      continue;
    }
    eol {
      break;
    }
    "$". {
      last_token_ = start;
      return Error("bad $-escape (literal $ must be written as $$)", err);
    }
    nul {
      last_token_ = start;
      return UnexpectedNulError(start, err);
    }
    [^] {
      last_token_ = start;
      return Error(DescribeLastError(), err);
    }
    */
  }
  *out = StringPiece(ofs_, p - ofs_);
  last_token_ = start;
  ofs_ = p;
  // Non-path strings end in newlines, so there's no whitespace to eat.
  return true;
}

StringPiece Lexer::PeekCanonicalPath() {
  auto finish = [this](const char* start, const char* end) {
    ofs_ = end;
    EatWhitespace();
    return StringPiece(start, end - start);
  };

  const char* p = ofs_;
  const char* q;
  const char* r;
  last_token_ = ofs_;

  do {
    /*!re2c
    canon_no_dot = [^$ :|/.\r\n\000];
    canon_any = canon_no_dot | ".";
    canon_piece = ( canon_no_dot | "." canon_no_dot | ".." canon_any ) canon_any*;
    canon_pieces = canon_piece ( "/" canon_piece )*;

    // The Chromium gn manifests have many paths that start with "./" but are
    // otherwise canonical. Allow them and strip off the leading "./".
    ( "/" | "../"* ) canon_pieces / ([ :|] | eol)   { return finish(ofs_, p);     }
    "./" "../"*      canon_pieces / ([ :|] | eol)   { return finish(ofs_ + 2, p); }
    [^]                                             { break;                      }
    */
  } while (false);

  return {};
}

bool Lexer::ReadPath(LexedPath* out, std::string* err) {
  const char* p = ofs_;
  const char* q;
  const char* start;
  for (;;) {
    start = p;
    /*!re2c
    ( [^$ :|\r\n\000]
        | "$" [$: ]
        | "$" eol [ ]*
        | "${" varname "}"
        | "$" simple_varname )+ {
      continue;
    }
    [ :|] | eol {
      p = start;
      break;
    }
    "$". {
      last_token_ = start;
      return Error("bad $-escape (literal $ must be written as $$)", err);
    }
    nul {
      last_token_ = start;
      return UnexpectedNulError(start, err);
    }
    [^] {
      last_token_ = start;
      return Error(DescribeLastError(), err);
    }
    */
  }
  *out = {};
  out->str_ = StringPiece(ofs_, p - ofs_);
  last_token_ = start;
  ofs_ = p;
  EatWhitespace();
  return true;
}

/// Append the let binding's evaluated value to the output string. The input
/// StringPiece must include a valid binding terminator.
template <typename EvalVar>
static inline void EvaluateBinding(std::string* out_append, StringPiece value,
                                   EvalVar&& eval_var) {
  auto expand = [&eval_var](const char* start, const char* end) {
    StringPiece var(start, end - start);
    eval_var(var);
  };

  const char* p = value.data();
  const char* q;

  for (;;) {
    const char* start = p;
    /*!re2c
    [^$\r\n\000]+       { out_append->append(start, p - start);   continue; }
    "$" [$: ]           { out_append->push_back(start[1]);        continue; }
    "$" eol [ ]*        {                                         continue; }
    "${" varname "}"    { expand(start + 2, p - 1);               continue; }
    "$" simple_varname  { expand(start + 1, p);                   continue; }
    eol                 {                                         break;    }
    [^]                 { assert(false && "bad input in EvaluateBinding"); abort(); }
    */
  }
  assert((p == value.data() + value.size()) &&
         "bad end pos in EvaluateBinding");
}

void EvaluateBindingInScope(std::string* out_append, StringPiece value,
                            ScopePosition pos) {
  EvaluateBinding(out_append, value,
      [out_append, &pos](const HashedStrView& var) {
    Scope::EvaluateVariableAtPos(out_append, var, pos);
  });
}

bool EvaluateBindingOnRule(std::string* out_append, StringPiece value,
                           EdgeEval* edge_eval, std::string* err) {
  bool result = true;
  EvaluateBinding(out_append, value,
      [out_append, edge_eval, &result, err](const HashedStrView& var) {
    result = result && edge_eval->EvaluateVariable(out_append, var, err);
  });
  return result;
}

std::string EvaluateBindingForTesting(StringPiece value) {
  std::string result;
  EvaluateBinding(&result, value, [&result](StringPiece var) {
    result += "[$" + var.AsString() + "]";
  });
  return result;
}

/// Append an evaluated path to the output string.
///
/// This function does not canonicalize the output. Ninja canonicalizes paths for
/// build nodes, but not all paths (e.g. It doesn't canonicalize paths to
/// included ninja files.)
template <typename EvalVar>
static inline void EvaluatePath(std::string* out_append, const LexedPath& path,
                                EvalVar&& eval_var) {
  auto expand = [&eval_var](const char* start, const char* end) {
    StringPiece var(start, end - start);
    eval_var(var);
  };

  const char* p = path.str_.data();
  const char* q;

  for (;;) {
    const char* start = p;
    /*!re2c
    [^$ :|\r\n\000]+        { out_append->append(start, p - start);   continue; }
    "$" [$: ]               { out_append->push_back(start[1]);        continue; }
    "$" eol [ ]*            {                                         continue; }
    "${" varname "}"        { expand(start + 2, p - 1);               continue; }
    "$" simple_varname      { expand(start + 1, p);                   continue; }
    [ :|] | eol             { p = start;                              break;    }
    [^]                     { assert(false && "bad input in EvaluatePath"); abort(); }
    */
  }
  assert((p == path.str_.data() + path.str_.size()) &&
         "bad end pos in EvaluatePath");
}

void EvaluatePathInScope(std::string* out_append, const LexedPath& path,
                         ScopePosition pos) {
  EvaluatePath(out_append, path, [out_append, &pos](const HashedStrView& var) {
    Scope::EvaluateVariableAtPos(out_append, var, pos);
  });
}

void EvaluatePathOnEdge(std::string* out_append, const LexedPath& path,
                        const Edge& edge) {
  EvaluatePath(out_append, path, [out_append, &edge](const HashedStrView& var) {
    // First look for a binding on the edge itself, then fall back to the
    // edge's enclosing scope.
    if (edge.EvaluateVariableSelfOnly(out_append, var))
      return;
    Scope::EvaluateVariableAtPos(out_append, var, edge.pos_.scope_pos());
  });
}

std::string EvaluatePathForTesting(const LexedPath& path) {
  std::string result;
  EvaluatePath(&result, path, [&result](StringPiece var) {
    result += "[$" + var.AsString() + "]";
  });
  return result;
}
