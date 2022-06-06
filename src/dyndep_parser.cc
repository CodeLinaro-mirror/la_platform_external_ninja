// Copyright 2015 Google Inc. All Rights Reserved.
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

#include "dyndep_parser.h"

#include <vector>

#include "dyndep.h"
#include "graph.h"
#include "state.h"
#include "util.h"
#include "version.h"

DyndepParser::DyndepParser(State* state, FileReader* file_reader,
                           DyndepFile* dyndep_file, Scope* scope)
    : Parser(state, file_reader)
    , dyndep_file_(dyndep_file)
    , scope_(scope) {
}

bool DyndepParser::Parse(const string& filename, const string& input,
                         string* err) {
  Lexer lexer(filename, input, input.data());

  // Require a supported ninja_dyndep_version value immediately so
  // we can exit before encountering any syntactic surprises.
  bool haveDyndepVersion = false;

  for (;;) {
    Lexer::Token token = lexer.ReadToken();
    switch (token) {
    case Lexer::BUILD: {
      if (!haveDyndepVersion)
        return lexer.Error("expected 'ninja_dyndep_version = ...'", err);
      if (!ParseEdge(lexer, err))
        return false;
      break;
    }
    case Lexer::IDENT: {
      lexer.UnreadToken();
      if (haveDyndepVersion)
        return lexer.Error(string("unexpected ") + Lexer::TokenName(token),
                            err);
      if (!ParseDyndepVersion(lexer, err))
        return false;
      haveDyndepVersion = true;
      break;
    }
    case Lexer::ERROR:
      return lexer.Error(lexer.DescribeLastError(), err);
    case Lexer::TEOF:
      if (!haveDyndepVersion)
        return lexer.Error("expected 'ninja_dyndep_version = ...'", err);
      return true;
    case Lexer::NEWLINE:
      break;
    default:
      return lexer.Error(string("unexpected ") + Lexer::TokenName(token),
                          err);
    }
  }
  return false;  // not reached
}

static const HashedStrView kNinjaDyndepVersion { "ninja_dyndep_version" };

bool DyndepParser::ParseDyndepVersion(Lexer& lexer, string* err) {
  StringPiece name;
  StringPiece let_value;
  if (!ParseLet(lexer, &name, &let_value, err))
    return false;
  if (name != kNinjaDyndepVersion) {
    return lexer.Error("expected 'ninja_dyndep_version = ...'", err);
  }
  string version;
  EvaluateBindingInScope(&version, let_value, scope_);
  int major, minor;
  ParseVersion(version, &major, &minor);
  if (major != 1 || minor != 0) {
    return lexer.Error(
      string("unsupported 'ninja_dyndep_version = ") + version + "'", err);
    return false;
  }
  return true;
}

bool DyndepParser::ParseLet(Lexer& lexer, StringPiece* key, StringPiece* value, string* err) {
  if (!lexer.ReadIdent(key))
    return lexer.Error("expected variable name", err);
  if (!ExpectToken(lexer, Lexer::EQUALS, err))
    return false;
  if (!lexer.ReadBindingValue(value, err))
    return false;
  return true;
}

static const HashedStrView kDyndep { "dyndep" };
static const HashedStrView kRestat { "restat" };

bool DyndepParser::ParseEdge(Lexer& lexer, string* err) {
  // Parse one explicit output.  We expect it to already have an edge.
  // We will record its dynamically-discovered dependency information.
  Dyndeps* dyndeps = NULL;
  {
    LexedPath out0;
    if (!lexer.ReadPath(&out0, err))
      return false;
    if (out0.str_.empty())
      return lexer.Error("expected path", err);

    string path;
    EvaluatePathInScope(&path, out0, scope_);
    string path_err;
    uint64_t slash_bits;
    if (!CanonicalizePath(&path, &slash_bits, &path_err))
      return lexer.Error(path_err, err);
    Node* node = state_->LookupNode(scope_.scope->GlobalPath(path));
    if (!node || !node->in_edge())
      return lexer.Error("no build statement exists for '" + path + "'", err);
    Edge* edge = node->in_edge();
    std::pair<DyndepFile::iterator, bool> res =
      dyndep_file_->insert(DyndepFile::value_type(edge, Dyndeps()));
    if (!res.second)
      return lexer.Error("multiple statements for '" + path + "'", err);
    dyndeps = &res.first->second;
  }

  // Disallow explicit outputs.
  {
    LexedPath out;
    if (!lexer.ReadPath(&out, err))
      return false;
    if (!out.str_.empty())
      return lexer.Error("explicit outputs not supported", err);
  }

  // Parse implicit outputs, if any.
  vector<LexedPath> outs;
  if (lexer.PeekToken(Lexer::PIPE)) {
    for (;;) {
      LexedPath out;
      if (!lexer.ReadPath(&out, err))
        return err;
      if (out.str_.empty())
        break;
      outs.push_back(out);
    }
  }

  if (!ExpectToken(lexer, Lexer::COLON, err))
    return false;

  StringPiece rule_name;
  if (!lexer.ReadIdent(&rule_name) || rule_name != kDyndep)
    return lexer.Error("expected build command name 'dyndep'", err);

  // Disallow explicit inputs.
  {
    LexedPath in;
    if (!lexer.ReadPath(&in, err))
      return false;
    if (!in.str_.empty())
      return lexer.Error("explicit inputs not supported", err);
  }

  // Parse implicit inputs, if any.
  vector<LexedPath> ins;
  if (lexer.PeekToken(Lexer::PIPE)) {
    for (;;) {
      LexedPath in;
      if (!lexer.ReadPath(&in, err))
        return err;
      if (in.str_.empty())
        break;
      ins.push_back(in);
    }
  }

  // Disallow order-only inputs.
  if (lexer.PeekToken(Lexer::PIPE2))
    return lexer.Error("order-only inputs not supported", err);

  if (!ExpectToken(lexer, Lexer::NEWLINE, err))
    return false;

  if (lexer.PeekToken(Lexer::INDENT)) {
    StringPiece key;
    StringPiece val;
    if (!ParseLet(lexer, &key, &val, err))
      return false;
    if (key != kRestat)
      return lexer.Error("binding is not 'restat'", err);
    string value;
    EvaluateBindingInScope(&value, val, scope_);
    dyndeps->restat_ = !value.empty();
  }

  dyndeps->implicit_inputs_.reserve(ins.size());
  for (vector<LexedPath>::iterator i = ins.begin(); i != ins.end(); ++i) {
    string path;
    EvaluatePathInScope(&path, *i, scope_);
    string path_err;
    uint64_t slash_bits;
    if (!CanonicalizePath(&path, &slash_bits, &path_err))
      return lexer.Error(path_err, err);
    Node* n = state_->GetNode(scope_.scope->GlobalPath(path), slash_bits);
    dyndeps->implicit_inputs_.push_back(n);
  }

  dyndeps->implicit_outputs_.reserve(outs.size());
  for (vector<LexedPath>::iterator i = outs.begin(); i != outs.end(); ++i) {
    string path;
    EvaluatePathInScope(&path, *i, scope_);
    string path_err;
    uint64_t slash_bits;
    if (!CanonicalizePath(&path, &slash_bits, &path_err))
      return lexer.Error(path_err, err);
    Node* n = state_->GetNode(scope_.scope->GlobalPath(path), slash_bits);
    dyndeps->implicit_outputs_.push_back(n);
  }

  return true;
}
