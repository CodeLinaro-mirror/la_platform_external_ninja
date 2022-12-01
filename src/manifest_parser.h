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

#ifndef NINJA_MANIFEST_PARSER_H_
#define NINJA_MANIFEST_PARSER_H_

#include <string>

struct FileReader;
struct State;

enum DupeEdgeAction {
  kDupeEdgeActionWarn,
  kDupeEdgeActionError,
};

enum PhonyCycleAction {
  kPhonyCycleActionWarn,
  kPhonyCycleActionError,
};

struct ManifestParserOptions {
  DupeEdgeAction dupe_edge_action_ = kDupeEdgeActionWarn;
  PhonyCycleAction phony_cycle_action_ = kPhonyCycleActionWarn;
  bool experimentalEnvvar = false;
};

struct ManifestParser {
  ManifestParser(State* state, FileReader* file_reader,
                 ManifestParserOptions options = ManifestParserOptions())
      : state_(state),
        file_reader_(file_reader),
        options_(options) {}

  /// Load and parse a file.
  bool Load(const std::string& filename, std::string* err);

  /// Parse a text string of input. Used by tests.
  ///
  /// Some tests may call ParseTest multiple times with the same State object.
  /// Each call adds to the previous state; it doesn't replace it.
  bool ParseTest(const std::string& input, std::string* err);

private:
  State* state_ = nullptr;
  FileReader* file_reader_ = nullptr;
  ManifestParserOptions options_;
};

#endif  // NINJA_MANIFEST_PARSER_H_
