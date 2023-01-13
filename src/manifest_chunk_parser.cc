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

#include "manifest_chunk_parser.h"

#include <assert.h>

#include <thread>
#include <unordered_map>

#include "lexer.h"
#include "metrics.h"
#include "thread_pool.h"
#include "util.h"

namespace manifest_chunk {

class ChunkParser {
  const LoadedFile& file_;
  const bool experimentalEnvvar_;
  Lexer lexer_;
  const char* chunk_end_ = nullptr;
  std::vector<ParserItem>* out_ = nullptr;
  Clump* current_clump_ = nullptr;

  Clump* MakeClump() {
    if (current_clump_)
      return current_clump_;
    current_clump_ = new Clump { file_ };
    out_->push_back(current_clump_);
    return current_clump_;
  }

  void OutItem(ParserItem item) {
    current_clump_ = nullptr;
    out_->push_back(std::move(item));
  }

  bool OutError(const std::string& err) {
    OutItem(new Error { err });
    return false;
  }

  bool LexerError(const std::string& message);
  bool ExpectToken(Lexer::Token expected);
  bool ParseLet(StringPiece* key, StringPiece* value);
  bool ParseFileInclude(bool new_scope);
  bool ParsePool();
  bool ParseDefault();
  bool ParseBinding();
  bool ParseRule();
  bool ParseEdge();

public:
  ChunkParser(const LoadedFile& file,
              StringPiece chunk_content,
              std::vector<ParserItem>* out,
              bool experimentalEnvvar)
      : file_(file),
        experimentalEnvvar_(experimentalEnvvar),
        lexer_(file.filename(), file.content(), chunk_content.data()),
        chunk_end_(chunk_content.data() + chunk_content.size()),
        out_(out) {}

  bool ParseChunk();
};

bool ChunkParser::LexerError(const std::string& message) {
  std::string err;
  lexer_.Error(message, &err);
  return OutError(err);
}

bool ChunkParser::ExpectToken(Lexer::Token expected) {
  Lexer::Token token = lexer_.ReadToken();
  if (token != expected) {
    std::string message = string("expected ") + Lexer::TokenName(expected);
    message += string(", got ") + Lexer::TokenName(token);
    message += Lexer::TokenErrorHint(expected);
    return LexerError(message);
  }
  return true;
}

bool ChunkParser::ParseLet(StringPiece* key, StringPiece* value) {
  if (!lexer_.ReadIdent(key))
    return LexerError("expected variable name");
  if (!ExpectToken(Lexer::EQUALS))
    return false;
  std::string err;
  if (!lexer_.ReadBindingValue(value, &err))
    return OutError(err);
  return true;
}

bool ChunkParser::ParseFileInclude(bool new_scope) {
  Include* include = new Include();
  include->new_scope_ = new_scope;
  std::string err;
  if (!lexer_.ReadPath(&include->path_, &err))
    return OutError(err);
  include->diag_pos_ = lexer_.GetLastTokenOffset();
  if (!ExpectToken(Lexer::NEWLINE))
    return false;
  // If new_scope == false, it would be possible to just ignore the next token.
  // If a Lexer::INDENT somehow was the next token, it would fail with
  // 'ninja: build.ninja:NNN: unexpected indent'. This might be a slightly more
  // helpful message.
  if (lexer_.PeekToken(Lexer::INDENT)) {
    if (!new_scope)
      return LexerError("indent after 'include' line is invalid.");
    for (;;) {
      if (lexer_.PeekToken(Lexer::CHDIR)) {
        break;
      }
      if (experimentalEnvvar_) {
        // Accept one or more "env foo = bar\n<indent><chdir>"
        StringPiece envkey;
        if (lexer_.ReadIdent(&envkey)) {
          if (envkey.compare("env")) {
            return LexerError("unexpected \"" + envkey.AsString() +
                              "\". Only 'chdir =' allowed here.");
          }
          StringPiece key;
          StringPiece val;
          if (!lexer_.ReadIdent(&key)) {
            return LexerError(
              "expected \"env VAR = value1 value2 value3\", only got \"env\"");
          }
          if (!key.compare("chdir")) {
            return LexerError("reserved word chdir: \"env chdir =\"");
          }
          if (!ExpectToken(Lexer::EQUALS)) {
            return false;  // ExpectToken() has already set Lexer error
          }
          std::string err;
          if (!lexer_.ReadBindingValue(&val, &err)) {
            return OutError(err);
          }

          const char* ws = " \t\r\n";
          std::string valstr = val.AsString();
          valstr.erase(valstr.find_last_not_of(ws) + 1);
          valstr.erase(0, valstr.find_first_not_of(ws));

          auto p = include->envvar.emplace(key.AsString(), valstr);
          if (!p.second) {
            return LexerError("duplicate env var");
          }
          if (!lexer_.PeekToken(Lexer::INDENT)) {
            return LexerError(
                "got \"env " + key.AsString() + " = " + valstr + "\" but missing chdir.");
          }
          continue;  // Successfully parsed an env statement.
        }
      }
      return LexerError("only 'chdir =' is allowed after 'subninja' line.");
    }
    if (!ExpectToken(Lexer::EQUALS))
      return LexerError("only 'chdir =' is allowed after 'subninja' line.");
    if (!lexer_.ReadPath(&include->chdir_, &err))
      return OutError(err);

    // Disallow '..' and '.' relative paths
    include->chdir_plus_slash_ = include->chdir_.str_.AsString();
    auto first_sep = include->chdir_plus_slash_.find('/');
    if (first_sep == std::string::npos) {
      first_sep = include->chdir_plus_slash_.size();
    }
#ifdef _WIN32
    auto first_win_sep = include->chdir_plus_slash_.find('\\');
    if (first_win_sep != std::string::npos && first_win_sep < first_sep) {
      first_sep = first_win_sep;
    }
#endif
    if ((first_sep == 1 && include->chdir_plus_slash_[0] == '.') ||
        (first_sep == 2 && !include->chdir_plus_slash_.compare(0, 2, "..")))
      return LexerError("invalid use of '.' or '..' in chdir");

    // The trailing '/' is added in manifest_parser.cc.
    OutItem(include);
    return true;
  }
  OutItem(include);
  return true;
}

bool ChunkParser::ParsePool() {
  Pool* pool = new Pool();

  StringPiece name;
  if (!lexer_.ReadIdent(&name))
    return LexerError("expected pool name");
  pool->name_ = name;

  if (!ExpectToken(Lexer::NEWLINE))
    return false;

  pool->parse_state_.pool_name_diag_pos = lexer_.GetLastTokenOffset();

  while (lexer_.PeekIndent()) {
    StringPiece key;
    StringPiece value;
    if (!ParseLet(&key, &value))
      return false;

    if (key == "depth") {
      pool->parse_state_.depth = value;
      pool->parse_state_.depth_diag_pos = lexer_.GetLastTokenOffset();
    } else {
      return LexerError("unexpected variable '" + key.AsString() + "'");
    }
  }

  if (pool->parse_state_.depth.empty())
    return LexerError("expected 'depth =' line");

  Clump* clump = MakeClump();
  pool->pos_ = clump->AllocNextPos();
  clump->pools_.push_back(pool);
  return true;
}

bool ChunkParser::ParseDefault() {
  bool first = true;
  while (true) {
    LexedPath path;
    std::string err;
    if (!lexer_.ReadPath(&path, &err))
      return OutError(err);
    if (path.str_.empty()) {
      if (first)
        return LexerError("expected target name");
      else
        break;
    }
    first = false;

    DefaultTarget* target = new DefaultTarget();
    target->parsed_path_ = std::move(path);
    target->diag_pos_ = lexer_.GetLastTokenOffset();

    Clump* clump = MakeClump();
    target->pos_ = clump->AllocNextPos();
    clump->default_targets_.push_back(target);
  }
  return ExpectToken(Lexer::NEWLINE);
}

static const HashedStrView kNinjaRequiredVersion { "ninja_required_version" };

bool ChunkParser::ParseBinding() {
  Binding* binding = new Binding();

  lexer_.UnreadToken();
  StringPiece name;
  if (!ParseLet(&name, &binding->parsed_value_))
    return false;
  binding->name_ = name;

  // Record a ninja_required_version binding specially. We want to do this
  // version check ASAP in case we encounter unexpected syntax. We can't do the
  // check immediately because the version string is evaluated and might need
  // bindings from an earlier chunk. We could probably do this version check as
  // part of ordinary scope setup while processing a Clump, but keeping it
  // separate guarantees that the version check is done early enough.
  if (binding->name_ == kNinjaRequiredVersion) {
    OutItem(new RequiredVersion { binding->parsed_value_ });
  }

  Clump* clump = MakeClump();
  binding->pos_ = clump->AllocNextPos();
  clump->bindings_.push_back(binding);
  return true;
}

static const HashedStrView kRspFile         { "rspfile" };
static const HashedStrView kRspFileContent  { "rspfile_content" };
static const HashedStrView kCommand         { "command" };

bool ChunkParser::ParseRule() {
  Rule* rule = new Rule();

  StringPiece rule_name;
  if (!lexer_.ReadIdent(&rule_name))
    return LexerError("expected rule name");
  rule->name_ = rule_name;

  if (!ExpectToken(Lexer::NEWLINE))
    return false;

  rule->parse_state_.rule_name_diag_pos = lexer_.GetLastTokenOffset();

  while (lexer_.PeekIndent()) {
    StringPiece key;
    StringPiece value;
    if (!ParseLet(&key, &value))
      return false;

    if (!Rule::IsReservedBinding(key)) {
      // Die on other keyvals for now; revisit if we want to add a scope here.
      // If we allow arbitrary key values here, we'll need to revisit how cycle
      // detection works when evaluating a rule variable.
      return LexerError("unexpected variable '" + key.AsString() + "'");
    }

    rule->bindings_.emplace_back(std::piecewise_construct,
                                 std::tuple<StringPiece>(key),
                                 std::tuple<const char*, size_t>(value.data(),
                                                                 value.size()));
  }

  if (static_cast<bool>(rule->GetBinding(kRspFile)) !=
      static_cast<bool>(rule->GetBinding(kRspFileContent))) {
    return LexerError("rspfile and rspfile_content need to be both specified");
  }

  if (rule->GetBinding(kCommand) == nullptr)
    return LexerError("expected 'command =' line");

  Clump* clump = MakeClump();
  rule->pos_ = clump->AllocNextPos();
  clump->rules_.push_back(rule);
  return true;
}

bool ChunkParser::ParseEdge() {
  Edge* edge = new Edge();

  auto parse_path_list = [this, edge](Edge::DeferredPathList::Type type, int& count) -> bool {
    const char* start_pos = lexer_.GetPos();
    while (true) {
      LexedPath path;
      std::string err;
      if (!lexer_.ReadPath(&path, &err))
        return OutError(err);
      if (path.str_.empty()) {
        // In the initial parsing pass, the manifest's bindings aren't ready
        // yet, so paths can't be evaluated. Rather than store the path itself
        // (wasting memory), store just enough information to parse the path
        // lists again once bindings are ready.
        edge->parse_state_.deferred_path_lists.push_back({
          start_pos, type, count,
        });
        return true;
      }
      count++;
    }
  };

  if (!parse_path_list(Edge::DeferredPathList::OUTPUT,
      edge->explicit_outs_))
    return false;

  // Add all implicit outs, counting how many as we go.
  if (lexer_.PeekToken(Lexer::PIPE)) {
    if (!parse_path_list(Edge::DeferredPathList::OUTPUT,
        edge->implicit_outs_))
      return false;
  }

  if (edge->explicit_outs_ + edge->implicit_outs_ == 0)
    return LexerError("expected path");

  if (!ExpectToken(Lexer::COLON))
    return false;

  StringPiece rule_name;
  if (!lexer_.ReadIdent(&rule_name))
    return LexerError("expected build command name");
  edge->parse_state_.rule_name = rule_name;
  edge->parse_state_.rule_name_diag_pos = lexer_.GetLastTokenOffset();

  if (!parse_path_list(Edge::DeferredPathList::INPUT,
      edge->explicit_deps_))
    return false;

  // Add all implicit deps, counting how many as we go.
  if (lexer_.PeekToken(Lexer::PIPE)) {
    if (!parse_path_list(Edge::DeferredPathList::INPUT,
        edge->implicit_deps_))
      return false;
  }

  // Add all order-only deps, counting how many as we go.
  if (lexer_.PeekToken(Lexer::PIPE2)) {
    if (!parse_path_list(Edge::DeferredPathList::INPUT,
        edge->order_only_deps_))
      return false;
  }

  // Add all validation deps, counting how many as we go.
  if (lexer_.PeekToken(Lexer::PIPEAT)) {
    if (!parse_path_list(Edge::DeferredPathList::VALIDATION,
        edge->validation_deps_))
      return false;
  }


  if (!ExpectToken(Lexer::NEWLINE))
    return false;

  while (lexer_.PeekIndent()) {
    StringPiece key;
    StringPiece val;
    if (!ParseLet(&key, &val))
      return false;

    std::tuple<const char*, size_t> val_ctor_params(val.data(), val.size());
    edge->unevaled_bindings_.emplace_back(std::piecewise_construct,
                                          std::tuple<StringPiece>(key),
                                          val_ctor_params);
  }

  edge->parse_state_.final_diag_pos = lexer_.GetLastTokenOffset();

  Clump* clump = MakeClump();
  edge->pos_ = clump->AllocNextPos();
  // Can't edge->onPosResolvedToScope(clump->pos_.scope.scope) right here, since
  // clump->pos_.scope.scope is not set until DfsParser::HandleClump().
  clump->edges_.push_back(edge);
  clump->edge_output_count_ += edge->explicit_outs_;
  return true;
}

bool ChunkParser::ParseChunk() {
  while (true) {
    if (lexer_.GetPos() >= chunk_end_) {
      assert(lexer_.GetPos() == chunk_end_ &&
             "lexer scanned beyond the end of a manifest chunk");
      return true;
    }

    Lexer::Token token = lexer_.ReadToken();
    bool success = true;
    switch (token) {
    case Lexer::INCLUDE:  success = ParseFileInclude(false); break;
    case Lexer::SUBNINJA: success = ParseFileInclude(true); break;
    case Lexer::POOL:     success = ParsePool(); break;
    case Lexer::DEFAULT:  success = ParseDefault(); break;
    case Lexer::IDENT:    success = ParseBinding(); break;
    case Lexer::RULE:     success = ParseRule(); break;
    case Lexer::BUILD:    success = ParseEdge(); break;
    case Lexer::NEWLINE:  break;
    case Lexer::ERROR:    return LexerError(lexer_.DescribeLastError());
    case Lexer::TNUL:     return LexerError("unexpected NUL byte");
    case Lexer::TEOF:
      assert(false && "EOF should have been detected before reading a token");
      break;
    default:
      return LexerError(std::string("unexpected ") + Lexer::TokenName(token));
    }
    if (!success) return false;
  }
  return false;  // not reached
}

void ParseChunk(const LoadedFile& file, StringPiece chunk_content,
                std::vector<ParserItem>* out, bool experimentalEnvvar) {
  ChunkParser parser(file, chunk_content, out, experimentalEnvvar);
  if (!parser.ParseChunk()) {
    assert(!out->empty());
    assert(out->back().kind == ParserItem::kError);
    assert(!out->back().u.error->msg_.empty());
  }
}

std::vector<StringPiece> SplitManifestIntoChunks(StringPiece input) {
  METRIC_RECORD(".ninja load : split manifest");

  // The lexer requires that a NUL character appear after the file's content.
  // Make the NUL implicit for the rest of the manifest parsing.
  //
  // In general, parsing a manifest chunk examines the terminating text after
  // the chunk's explicit end. For example, suppose we have two consecutive
  // chunks:
  //  - "build foo: gen\n  pool = mypool\n"
  //  - "build bar: gen\n"
  // The parser for the "foo" chunk will examine the start of "build" from the
  // "bar" chunk when it looks for another indented edge binding.
  assert(!input.empty() && input.back() == '\0' &&
         "input lacks a NUL terminator");
  input.remove_suffix(1);

  // For efficiency, avoid splitting small files. kChunkMinSize can be lowered
  // to 0 for testing.
  const size_t kChunkMinSize = 1024 * 1024;

  const size_t chunk_count = GetOptimalThreadPoolJobCount();
  const size_t chunk_size = std::max(kChunkMinSize,
                                     input.size() / chunk_count + 1);

  std::vector<StringPiece> result;
  result.reserve(chunk_count);

  size_t start = 0;
  while (start < input.size()) {
    size_t next = std::min(start + chunk_size, input.size());
    next = AdvanceToNextManifestChunk(input, next);
    result.push_back(input.substr(start, next - start));
    start = next;
  }

  return result;
}

} // namespace manifest_chunk
