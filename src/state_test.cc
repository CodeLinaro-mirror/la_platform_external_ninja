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

#include "graph.h"
#include "state.h"
#include "test.h"

namespace {

TEST(State, Basic) {
  State state;

  Rule* rule = new Rule("cat");
  rule->bindings_.emplace_back("command", "cat $in > $out\n");
  state.root_scope_.AddRule(rule);

  Edge* edge = state.AddEdge(rule);
  state.AddIn(edge, state.root_scope_.GlobalPath("in1"), 0);
  state.AddIn(edge, state.root_scope_.GlobalPath("in2"), 0);
  state.AddOut(edge, state.root_scope_.GlobalPath("out"), 0);

  EdgeCommand cmd;
  edge->EvaluateCommand(&cmd);
  EXPECT_EQ("cat in1 in2 > out", cmd.command);

  EXPECT_FALSE(state.GetNode(state.root_scope_.GlobalPath("in1"), 0)->dirty());
  EXPECT_FALSE(state.GetNode(state.root_scope_.GlobalPath("in2"), 0)->dirty());
  EXPECT_FALSE(state.GetNode(state.root_scope_.GlobalPath("out"), 0)->dirty());
}

}  // namespace
