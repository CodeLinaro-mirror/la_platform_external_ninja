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

#include "manifest_parser.h"

#include <map>
#include <vector>

#include "graph.h"
#include "state.h"
#include "test.h"

struct ParserTest : public testing::Test {
  void AssertParse(const char* input) {
    ManifestParser parser(&state, &fs_);
    string err;
    EXPECT_TRUE(parser.ParseTest(input, &err));
    ASSERT_EQ("", err);
    VerifyGraph(state);
  }

  std::string VerifyCwd(VirtualFileSystem &f_) {
    std::string result;
    std::string err;
    EXPECT_TRUE(f_.Getcwd(&result, &err));
    EXPECT_EQ("", err);
    return result;
  }

  State state;
  VirtualFileSystem fs_;

  Node* GetNode(const string& path) {
    return state.GetNode(state.root_scope_.GlobalPath(path), 0);
  }

  Node* LookupNode(const string& path) {
    return state.LookupNode(state.root_scope_.GlobalPath(path));
  }
};

TEST_F(ParserTest, Empty) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(""));
}

TEST_F(ParserTest, Rules) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"rule cat\n"
"  command = cat $in > $out\n"
"\n"
"rule date\n"
"  command = date > $out\n"
"\n"
"build result: cat in_1.cc in-2.O\n"));

  ASSERT_EQ(3u, state.root_scope_.GetRules().size());
  const Rule* rule = state.root_scope_.GetRules().begin()->second;
  EXPECT_EQ("cat", rule->name());
  EXPECT_EQ("cat $in > $out\n", *rule->GetBinding("command"));
}

TEST_F(ParserTest, RuleAttributes) {
  // Check that all of the allowed rule attributes are parsed ok.
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"rule cat\n"
"  command = a\n"
"  depfile = a\n"
"  deps = a\n"
"  description = a\n"
"  phony_output = a\n"
"  generator = a\n"
"  restat = a\n"
"  rspfile = a\n"
"  rspfile_content = a\n"
));
}

TEST_F(ParserTest, IgnoreIndentedComments) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"  #indented comment\n"
"rule cat\n"
"  command = cat $in > $out\n"
"  #generator = 1\n"
"  restat = 1 # comment\n"
"  #comment\n"
"build result: cat in_1.cc in-2.O\n"
"  #comment\n"));

  ASSERT_EQ(2u, state.root_scope_.GetRules().size());
  const Rule* rule = state.root_scope_.GetRules().begin()->second;
  EXPECT_EQ("cat", rule->name());
  Edge* edge = GetNode("result")->in_edge();
  EXPECT_TRUE(edge->IsRestat());
  EXPECT_FALSE(edge->IsGenerator());
}

TEST_F(ParserTest, IgnoreIndentedBlankLines) {
  // the indented blanks used to cause parse errors
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"  \n"
"rule cat\n"
"  command = cat $in > $out\n"
"  \n"
"build result: cat in_1.cc in-2.O\n"
"  \n"
"variable=1\n"));

  // the variable must be in the top level environment
  EXPECT_EQ("1", state.root_scope_.LookupVariable("variable"));
}

TEST_F(ParserTest, ResponseFiles) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"rule cat_rsp\n"
"  command = cat $rspfile > $out\n"
"  rspfile = $rspfile\n"
"  rspfile_content = $in\n"
"\n"
"build out: cat_rsp in\n"
"  rspfile=out.rsp\n"));

  ASSERT_EQ(2u, state.root_scope_.GetRules().size());
  const Rule* rule = state.root_scope_.GetRules().begin()->second;
  EXPECT_EQ("cat_rsp", rule->name());
  EXPECT_EQ("cat $rspfile > $out\n", *rule->GetBinding("command"));
  EXPECT_EQ("$rspfile\n", *rule->GetBinding("rspfile"));
  EXPECT_EQ("$in\n", *rule->GetBinding("rspfile_content"));
}

TEST_F(ParserTest, InNewline) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"rule cat_rsp\n"
"  command = cat $in_newline > $out\n"
"\n"
"build out: cat_rsp in in2\n"
"  rspfile=out.rsp\n"));

  ASSERT_EQ(2u, state.root_scope_.GetRules().size());
  const Rule* rule = state.root_scope_.GetRules().begin()->second;
  EXPECT_EQ("cat_rsp", rule->name());
  EXPECT_EQ("cat $in_newline > $out\n", *rule->GetBinding("command"));

  Edge* edge = state.edges_[0];
  EXPECT_EQ("cat in\nin2 > out", edge->EvaluateCommand());
}

TEST_F(ParserTest, Variables) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"l = one-letter-test\n"
"rule link\n"
"  command = ld $l $extra $with_under -o $out $in\n"
"\n"
"extra = -pthread\n"
"with_under = -under\n"
"build a: link b c\n"
"nested1 = 1\n"
"nested2 = $nested1/2\n"
"build supernested: link x\n"
"  extra = $nested2/3\n"));

  ASSERT_EQ(2u, state.edges_.size());
  Edge* edge = state.edges_[0];
  EXPECT_EQ("ld one-letter-test -pthread -under -o a b c",
            edge->EvaluateCommand());
  EXPECT_EQ("1/2", state.root_scope_.LookupVariable("nested2"));

  edge = state.edges_[1];
  EXPECT_EQ("ld one-letter-test 1/2/3 -under -o supernested x",
            edge->EvaluateCommand());
}

TEST_F(ParserTest, VariableScope) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"foo = bar\n"
"rule cmd\n"
"  command = cmd $foo $in $out\n"
"\n"
"build inner: cmd a\n"
"  foo = baz\n"
"build outer: cmd b\n"
"\n"  // Extra newline after build line tickles a regression.
));

  ASSERT_EQ(2u, state.edges_.size());
  EXPECT_EQ("cmd baz a inner", state.edges_[0]->EvaluateCommand());
  EXPECT_EQ("cmd bar b outer", state.edges_[1]->EvaluateCommand());
}

TEST_F(ParserTest, Continuation) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"rule link\n"
"  command = foo bar $\n"
"    baz\n"
"\n"
"build a: link c $\n"
" d e f\n"));

  ASSERT_EQ(2u, state.root_scope_.GetRules().size());
  const Rule* rule = state.root_scope_.GetRules().begin()->second;
  EXPECT_EQ("link", rule->name());
  std::string command = *rule->GetBinding("command");
  EXPECT_EQ("foo bar $\n    baz\n", command);
  EXPECT_EQ("foo bar baz", EvaluateBindingForTesting(command));
}

TEST_F(ParserTest, Backslash) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"foo = bar\\baz\n"
"foo2 = bar\\ baz\n"
));
  EXPECT_EQ("bar\\baz", state.root_scope_.LookupVariable("foo"));
  EXPECT_EQ("bar\\ baz", state.root_scope_.LookupVariable("foo2"));
}

TEST_F(ParserTest, Comment) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"# this is a comment\n"
"foo = not # a comment\n"));
  EXPECT_EQ("not # a comment", state.root_scope_.LookupVariable("foo"));
}

TEST_F(ParserTest, Dollars) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"rule foo\n"
"  command = ${out}bar$$baz$$$\n"
"blah\n"
"x = $$dollar\n"
"build $x: foo y\n"
));
  EXPECT_EQ("$dollar", state.root_scope_.LookupVariable("x"));
#ifdef _WIN32
  EXPECT_EQ("$dollarbar$baz$blah", state.edges_[0]->EvaluateCommand());
#else
  EXPECT_EQ("'$dollar'bar$baz$blah", state.edges_[0]->EvaluateCommand());
#endif
}

TEST_F(ParserTest, EscapeSpaces) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"rule spaces\n"
"  command = something\n"
"build foo$ bar: spaces $$one two$$$ three\n"
));
  EXPECT_TRUE(LookupNode("foo bar"));
  EXPECT_EQ(state.edges_[0]->outputs_[0]->path(), "foo bar");
  EXPECT_EQ(state.edges_[0]->inputs_[0]->path(), "$one");
  EXPECT_EQ(state.edges_[0]->inputs_[1]->path(), "two$ three");
  EXPECT_EQ(state.edges_[0]->EvaluateCommand(), "something");
}

TEST_F(ParserTest, CanonicalizeFile) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"rule cat\n"
"  command = cat $in > $out\n"
"build out: cat in/1 in//2\n"
"build in/1: cat\n"
"build in/2: cat\n"));

  EXPECT_TRUE(LookupNode("in/1"));
  EXPECT_TRUE(LookupNode("in/2"));
  EXPECT_FALSE(LookupNode("in//1"));
  EXPECT_FALSE(LookupNode("in//2"));
}

#ifdef _WIN32
TEST_F(ParserTest, CanonicalizeFileBackslashes) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"rule cat\n"
"  command = cat $in > $out\n"
"build out: cat in\\1 in\\\\2\n"
"build in\\1: cat\n"
"build in\\2: cat\n"));

  Node* node = LookupNode("in/1");;
  EXPECT_TRUE(node);
  EXPECT_EQ(1, node->slash_bits());
  node = LookupNode("in/2");
  EXPECT_TRUE(node);
  EXPECT_EQ(1, node->slash_bits());
  EXPECT_FALSE(LookupNode("in//1"));
  EXPECT_FALSE(LookupNode("in//2"));
}
#endif

TEST_F(ParserTest, PathVariables) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"rule cat\n"
"  command = cat $in > $out\n"
"dir = out\n"
"build $dir/exe: cat src\n"));

  EXPECT_FALSE(LookupNode("$dir/exe"));
  EXPECT_TRUE(LookupNode("out/exe"));
}

TEST_F(ParserTest, CanonicalizePaths) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"rule cat\n"
"  command = cat $in > $out\n"
"build ./out.o: cat ./bar/baz/../foo.cc\n"));

  EXPECT_FALSE(LookupNode("./out.o"));
  EXPECT_TRUE(LookupNode("out.o"));
  EXPECT_FALSE(LookupNode("./bar/baz/../foo.cc"));
  EXPECT_TRUE(LookupNode("bar/foo.cc"));
}

#ifdef _WIN32
TEST_F(ParserTest, CanonicalizePathsBackslashes) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"rule cat\n"
"  command = cat $in > $out\n"
"build ./out.o: cat ./bar/baz/../foo.cc\n"
"build .\\out2.o: cat .\\bar/baz\\..\\foo.cc\n"
"build .\\out3.o: cat .\\bar\\baz\\..\\foo3.cc\n"
));

  EXPECT_FALSE(LookupNode("./out.o"));
  EXPECT_FALSE(LookupNode(".\\out2.o"));
  EXPECT_FALSE(LookupNode(".\\out3.o"));
  EXPECT_TRUE(LookupNode("out.o"));
  EXPECT_TRUE(LookupNode("out2.o"));
  EXPECT_TRUE(LookupNode("out3.o"));
  EXPECT_FALSE(LookupNode("./bar/baz/../foo.cc"));
  EXPECT_FALSE(LookupNode(".\\bar/baz\\..\\foo.cc"));
  EXPECT_FALSE(LookupNode(".\\bar/baz\\..\\foo3.cc"));
  Node* node = LookupNode("bar/foo.cc");
  EXPECT_TRUE(node);
  EXPECT_EQ(0, node->slash_bits());
  node = LookupNode("bar/foo3.cc");
  EXPECT_TRUE(node);
  EXPECT_EQ(1, node->slash_bits());
}
#endif

TEST_F(ParserTest, DuplicateEdgeWithMultipleOutputs) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"rule cat\n"
"  command = cat $in > $out\n"
"build out1 out2: cat in1\n"
"build out1: cat in2\n"
"build final: cat out1\n"
));
  // AssertParse() checks that the generated build graph is self-consistent.
  // That's all the checking that this test needs.
}

TEST_F(ParserTest, NoDeadPointerFromDuplicateEdge) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"rule cat\n"
"  command = cat $in > $out\n"
"build out: cat in\n"
"build out: cat in\n"
));
  // AssertParse() checks that the generated build graph is self-consistent.
  // That's all the checking that this test needs.
}

TEST_F(ParserTest, DuplicateEdgeWithMultipleOutputsError) {
  const char kInput[] =
"rule cat\n"
"  command = cat $in > $out\n"
"build out1 out2: cat in1\n"
"build out1: cat in2\n"
"build final: cat out1\n";
  ManifestParserOptions parser_opts;
  parser_opts.dupe_edge_action_ = kDupeEdgeActionError;
  ManifestParser parser(&state, &fs_, parser_opts);
  string err;
  EXPECT_FALSE(parser.ParseTest(kInput, &err));
  EXPECT_EQ("input:5: multiple rules generate out1 [-w dupbuild=err]\n", err);
}

TEST_F(ParserTest, DuplicateEdgeInIncludedFile) {
  fs_.Create("sub.ninja",
    "rule cat\n"
    "  command = cat $in > $out\n"
    "build out1 out2: cat in1\n"
    "build out1: cat in2\n"
    "build final: cat out1\n");
  const char kInput[] =
    "subninja sub.ninja\n";
  ManifestParserOptions parser_opts;
  parser_opts.dupe_edge_action_ = kDupeEdgeActionError;
  ManifestParser parser(&state, &fs_, parser_opts);
  string err;
  EXPECT_FALSE(parser.ParseTest(kInput, &err));
  EXPECT_EQ("sub.ninja:5: multiple rules generate out1 [-w dupbuild=err]\n",
            err);
}

TEST_F(ParserTest, PhonySelfReferenceIgnored) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"build a: phony a\n"
));

  Node* node = LookupNode("a");
  Edge* edge = node->in_edge();
  ASSERT_TRUE(edge->inputs_.empty());
}

TEST_F(ParserTest, PhonySelfReferenceKept) {
  const char kInput[] =
"build a: phony a\n";
  ManifestParserOptions parser_opts;
  parser_opts.phony_cycle_action_ = kPhonyCycleActionError;
  ManifestParser parser(&state, &fs_, parser_opts);
  string err;
  EXPECT_TRUE(parser.ParseTest(kInput, &err));
  EXPECT_EQ("", err);

  Node* node = LookupNode("a");
  Edge* edge = node->in_edge();
  ASSERT_EQ(edge->inputs_.size(), 1);
  ASSERT_EQ(edge->inputs_[0], node);
}

TEST_F(ParserTest, ReservedWords) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"rule build\n"
"  command = rule run $out\n"
"build subninja: build include default foo.cc\n"
"default subninja\n"));
}

TEST_F(ParserTest, NulCharErrors) {
  {
    State local_state;
    ManifestParser parser(&local_state, NULL);
    std::string err;
    EXPECT_FALSE(parser.ParseTest("\0"_s, &err));
    EXPECT_EQ("input:1: unexpected NUL byte\n", err);
  }

  {
    State local_state;
    ManifestParser parser(&local_state, NULL);
    std::string err;
    EXPECT_FALSE(parser.ParseTest("build foo\0"_s, &err));
    EXPECT_EQ("input:1: unexpected NUL byte\n"
              "build foo\n"
              "         ^ near here"
              , err);
  }

  {
    State local_state;
    ManifestParser parser(&local_state, NULL);
    std::string err;
    EXPECT_FALSE(parser.ParseTest("foo\0"_s, &err));
    EXPECT_EQ("input:1: expected '=', got nul byte\n"
              "foo\n"
              "   ^ near here"
              , err);
  }
}

TEST_F(ParserTest, Errors) {
  {
    State local_state;
    ManifestParser parser(&local_state, NULL);
    string err;
    EXPECT_FALSE(parser.ParseTest(string("subn", 4), &err));
    EXPECT_EQ("input:1: expected '=', got eof\n"
              "subn\n"
              "    ^ near here"
              , err);
  }

  {
    State local_state;
    ManifestParser parser(&local_state, NULL);
    string err;
    EXPECT_FALSE(parser.ParseTest("foobar", &err));
    EXPECT_EQ("input:1: expected '=', got eof\n"
              "foobar\n"
              "      ^ near here"
              , err);
  }

  {
    State local_state;
    ManifestParser parser(&local_state, NULL);
    string err;
    EXPECT_FALSE(parser.ParseTest("x 3", &err));
    EXPECT_EQ("input:1: expected '=', got identifier\n"
              "x 3\n"
              "  ^ near here"
              , err);
  }

  {
    State local_state;
    ManifestParser parser(&local_state, NULL);
    string err;
    EXPECT_FALSE(parser.ParseTest("x = 3", &err));
    EXPECT_EQ("input:1: unexpected EOF\n"
              "x = 3\n"
              "     ^ near here"
              , err);
  }

  {
    State local_state;
    ManifestParser parser(&local_state, NULL);
    string err;
    EXPECT_FALSE(parser.ParseTest("x = 3\ny 2", &err));
    EXPECT_EQ("input:2: expected '=', got identifier\n"
              "y 2\n"
              "  ^ near here"
              , err);
  }

  {
    State local_state;
    ManifestParser parser(&local_state, NULL);
    string err;
    EXPECT_FALSE(parser.ParseTest("x = $", &err));
    EXPECT_EQ("input:1: bad $-escape (literal $ must be written as $$)\n"
              "x = $\n"
              "    ^ near here"
              , err);
  }

  {
    State local_state;
    ManifestParser parser(&local_state, NULL);
    string err;
    EXPECT_FALSE(parser.ParseTest("x = $\n $[\n", &err));
    EXPECT_EQ("input:2: bad $-escape (literal $ must be written as $$)\n"
              " $[\n"
              " ^ near here"
              , err);
  }

  {
    State local_state;
    ManifestParser parser(&local_state, NULL);
    string err;
    EXPECT_FALSE(parser.ParseTest("x = a$\n b$\n $\n", &err));
    EXPECT_EQ("input:4: unexpected EOF\n"
              , err);
  }

  {
    State local_state;
    ManifestParser parser(&local_state, NULL);
    string err;
    EXPECT_FALSE(parser.ParseTest("build\n", &err));
    EXPECT_EQ("input:1: expected path\n"
              "build\n"
              "     ^ near here"
              , err);
  }

  {
    State local_state;
    ManifestParser parser(&local_state, NULL);
    string err;
    EXPECT_FALSE(parser.ParseTest("build x: y z\n", &err));
    EXPECT_EQ("input:1: unknown build rule 'y'\n"
              "build x: y z\n"
              "         ^ near here"
              , err);
  }

  {
    State local_state;
    ManifestParser parser(&local_state, NULL);
    string err;
    EXPECT_FALSE(parser.ParseTest("build x:: y z\n", &err));
    EXPECT_EQ("input:1: expected build command name\n"
              "build x:: y z\n"
              "        ^ near here"
              , err);
  }

  {
    State local_state;
    ManifestParser parser(&local_state, NULL);
    string err;
    EXPECT_FALSE(parser.ParseTest("rule cat\n  command = cat ok\n"
                                  "build x: cat $\n :\n",
                                  &err));
    EXPECT_EQ("input:4: expected newline, got ':'\n"
              " :\n"
              " ^ near here"
              , err);
  }

  {
    State local_state;
    ManifestParser parser(&local_state, NULL);
    string err;
    EXPECT_FALSE(parser.ParseTest("rule cat\n",
                                  &err));
    EXPECT_EQ("input:2: expected 'command =' line\n", err);
  }

  {
    State local_state;
    ManifestParser parser(&local_state, NULL);
    string err;
    EXPECT_FALSE(parser.ParseTest("rule cat\n"
                                  "  command = echo\n"
                                  "rule cat\n"
                                  "  command = echo\n", &err));
    EXPECT_EQ("input:3: duplicate rule 'cat'\n"
              "rule cat\n"
              "        ^ near here"
              , err);
  }

  {
    State local_state;
    ManifestParser parser(&local_state, NULL);
    string err;
    EXPECT_FALSE(parser.ParseTest("rule cat\n"
                                  "  command = echo\n"
                                  "  rspfile = cat.rsp\n", &err));
    EXPECT_EQ(
        "input:4: rspfile and rspfile_content need to be both specified\n",
        err);
  }

  {
    State local_state;
    ManifestParser parser(&local_state, NULL);
    string err;
    EXPECT_FALSE(parser.ParseTest("rule cat\n"
                                  "  command = ${fafsd\n"
                                  "foo = bar\n",
                                  &err));
    EXPECT_EQ("input:2: bad $-escape (literal $ must be written as $$)\n"
              "  command = ${fafsd\n"
              "            ^ near here"
              , err);
  }


  {
    State local_state;
    ManifestParser parser(&local_state, NULL);
    string err;
    EXPECT_FALSE(parser.ParseTest("rule cat\n"
                                  "  command = cat\n"
                                  "build $.: cat foo\n",
                                  &err));
    EXPECT_EQ("input:3: bad $-escape (literal $ must be written as $$)\n"
              "build $.: cat foo\n"
              "      ^ near here"
              , err);
  }


  {
    State local_state;
    ManifestParser parser(&local_state, NULL);
    string err;
    EXPECT_FALSE(parser.ParseTest("rule cat\n"
                                  "  command = cat\n"
                                  "build $: cat foo\n",
                                  &err));
    EXPECT_EQ("input:3: expected ':', got newline ($ also escapes ':')\n"
              "build $: cat foo\n"
              "                ^ near here"
              , err);
  }

  {
    State local_state;
    ManifestParser parser(&local_state, NULL);
    string err;
    EXPECT_FALSE(parser.ParseTest("rule %foo\n",
                                  &err));
    EXPECT_EQ("input:1: expected rule name\n"
              "rule %foo\n"
              "     ^ near here",
              err);
  }

  {
    State local_state;
    ManifestParser parser(&local_state, NULL);
    string err;
    EXPECT_FALSE(parser.ParseTest("rule cc\n"
                                  "  command = foo\n"
                                  "  othervar = bar\n",
                                  &err));
    EXPECT_EQ("input:3: unexpected variable 'othervar'\n"
              "  othervar = bar\n"
              "                ^ near here"
              , err);
  }

  {
    State local_state;
    ManifestParser parser(&local_state, NULL);
    string err;
    EXPECT_FALSE(parser.ParseTest("rule cc\n  command = foo\n"
                                  "build $.: cc bar.cc\n",
                                  &err));
    EXPECT_EQ("input:3: bad $-escape (literal $ must be written as $$)\n"
              "build $.: cc bar.cc\n"
              "      ^ near here"
              , err);
  }

  {
    State local_state;
    ManifestParser parser(&local_state, NULL);
    string err;
    EXPECT_FALSE(parser.ParseTest("rule cc\n  command = foo\n  && bar",
                                  &err));
    EXPECT_EQ("input:3: expected variable name\n"
              "  && bar\n"
              "  ^ near here",
              err);
  }

  {
    State local_state;
    ManifestParser parser(&local_state, NULL);
    string err;
    EXPECT_FALSE(parser.ParseTest("rule cc\n  command = foo\n"
                                  "build $: cc bar.cc\n",
                                  &err));
    EXPECT_EQ("input:3: expected ':', got newline ($ also escapes ':')\n"
              "build $: cc bar.cc\n"
              "                  ^ near here"
              , err);
  }

  {
    State local_state;
    ManifestParser parser(&local_state, NULL);
    string err;
    EXPECT_FALSE(parser.ParseTest("default\n",
                                  &err));
    EXPECT_EQ("input:1: expected target name\n"
              "default\n"
              "       ^ near here"
              , err);
  }

  {
    // The default statement's target must be listed earlier in a build
    // statement.
    State local_state;
    ManifestParser parser(&local_state, NULL);
    string err;
    EXPECT_FALSE(parser.ParseTest("default nonexistent\n"
                                  "rule cat\n"
                                  "  command = cat $in > $out\n"
                                  "build nonexistent: cat existent\n",
                                  &err));
    EXPECT_EQ("input:1: unknown target 'nonexistent'\n"
              "default nonexistent\n"
              "                   ^ near here"
              , err);
  }

  {
    State local_state;
    ManifestParser parser(&local_state, NULL);
    string err;
    EXPECT_FALSE(parser.ParseTest("rule r\n  command = r\n"
                                  "build b: r\n"
                                  "default b:\n",
                                  &err));
    EXPECT_EQ("input:4: expected newline, got ':'\n"
              "default b:\n"
              "         ^ near here"
              , err);
  }

  {
    State local_state;
    ManifestParser parser(&local_state, NULL);
    string err;
    EXPECT_FALSE(parser.ParseTest("default $a\n", &err));
    EXPECT_EQ("input:1: empty path\n"
              "default $a\n"
              "          ^ near here"
              , err);
  }

  {
    State local_state;
    ManifestParser parser(&local_state, NULL);
    string err;
    EXPECT_FALSE(parser.ParseTest("rule r\n"
                                  "  command = r\n"
                                  "build $a: r $c\n", &err));
    // XXX the line number is wrong; we should evaluate paths in ParseEdge
    // as we see them, not after we've read them all!
    EXPECT_EQ("input:4: empty path\n", err);
  }

  {
    State local_state;
    ManifestParser parser(&local_state, NULL);
    string err;
    // the indented blank line must terminate the rule
    // this also verifies that "unexpected (token)" errors are correct
    EXPECT_FALSE(parser.ParseTest("rule r\n"
                                  "  command = r\n"
                                  "  \n"
                                  "  generator = 1\n", &err));
    EXPECT_EQ("input:4: unexpected indent\n", err);
  }

  {
    State local_state;
    ManifestParser parser(&local_state, NULL);
    string err;
    EXPECT_FALSE(parser.ParseTest("pool\n", &err));
    EXPECT_EQ("input:1: expected pool name\n"
              "pool\n"
              "    ^ near here", err);
  }

  {
    State local_state;
    ManifestParser parser(&local_state, NULL);
    string err;
    EXPECT_FALSE(parser.ParseTest("pool foo\n", &err));
    EXPECT_EQ("input:2: expected 'depth =' line\n", err);
  }

  {
    State local_state;
    ManifestParser parser(&local_state, NULL);
    string err;
    EXPECT_FALSE(parser.ParseTest("pool foo\n"
                                  "  depth = 4\n"
                                  "pool foo\n"
                                  "  depth = 2\n", &err));
    EXPECT_EQ("input:3: duplicate pool 'foo'\n"
              "pool foo\n"
              "        ^ near here"
              , err);
  }

  {
    State local_state;
    ManifestParser parser(&local_state, NULL);
    string err;
    EXPECT_FALSE(parser.ParseTest("pool foo\n"
                                  "  depth = -1\n", &err));
    EXPECT_EQ("input:2: invalid pool depth\n"
              "  depth = -1\n"
              "            ^ near here"
              , err);
  }

  {
    State local_state;
    ManifestParser parser(&local_state, NULL);
    string err;
    EXPECT_FALSE(parser.ParseTest("pool foo\n"
                                  "  bar = 1\n", &err));
    EXPECT_EQ("input:2: unexpected variable 'bar'\n"
              "  bar = 1\n"
              "         ^ near here"
              , err);
  }

  {
    State local_state;
    ManifestParser parser(&local_state, NULL);
    string err;
    // Pool names are dereferenced at edge parsing time.
    EXPECT_FALSE(parser.ParseTest("rule run\n"
                                  "  command = echo\n"
                                  "  pool = unnamed_pool\n"
                                  "build out: run in\n", &err));
    EXPECT_EQ("input:5: unknown pool name 'unnamed_pool'\n", err);
  }
}

TEST_F(ParserTest, MissingInput) {
  State local_state;
  ManifestParser parser(&local_state, &fs_);
  string err;
  EXPECT_FALSE(parser.Load("build.ninja", &err));
  EXPECT_EQ("loading 'build.ninja': No such file or directory", err);
}

TEST_F(ParserTest, MultipleOutputs) {
  State local_state;
  ManifestParser parser(&local_state, NULL);
  string err;
  EXPECT_TRUE(parser.ParseTest("rule cc\n  command = foo\n  depfile = bar\n"
                               "build a.o b.o: cc c.cc\n",
                               &err));
  EXPECT_EQ("", err);
}

TEST_F(ParserTest, MultipleImplicitOutputsWithDeps) {
  State local_state;
  ManifestParser parser(&local_state, NULL);
  string err;
  EXPECT_TRUE(parser.ParseTest("rule cc\n  command = foo\n  deps = gcc\n"
                               "build a.o | a.gcno: cc c.cc\n",
                               &err));
  EXPECT_EQ("", err);
}

TEST_F(ParserTest, MultipleOutputsWithDeps) {
  State local_state;
  ManifestParser parser(&local_state, NULL);
  string err;
  EXPECT_FALSE(parser.ParseTest("rule cc\n  command = foo\n  deps = gcc\n"
                               "build a.o b.o: cc c.cc\n",
                               &err));
  EXPECT_EQ("input:5: multiple outputs aren't (yet?) supported by depslog; "
            "bring this up on the mailing list if it affects you\n", err);
}

TEST_F(ParserTest, SubNinja) {
  fs_.Create("test.ninja",
    "var = inner\n"
    "build $builddir/inner: varref\n");
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"builddir = some_dir/\n"
"rule varref\n"
"  command = varref $var\n"
"var = outer\n"
"build $builddir/outer: varref\n"
"subninja test.ninja\n"
"build $builddir/outer2: varref\n"));
  ASSERT_EQ(1u, fs_.files_read_.size());

  EXPECT_EQ("test.ninja", fs_.files_read_[0]);
  EXPECT_TRUE(LookupNode("some_dir/outer"));
  // Verify our builddir setting is inherited.
  EXPECT_TRUE(LookupNode("some_dir/inner"));

  ASSERT_EQ(3u, state.edges_.size());
  EXPECT_EQ("varref outer", state.edges_[0]->EvaluateCommand());
  EXPECT_EQ("varref inner", state.edges_[1]->EvaluateCommand());
  EXPECT_EQ("varref outer", state.edges_[2]->EvaluateCommand());
}

TEST_F(ParserTest, SubNinjaChdir) {
  EXPECT_TRUE(fs_.MakeDir("a"));
  fs_.Create("a/foo.ninja",
    "var = inner\n"
    "rule innerrule\n"
    "  command = foo $var\n"
    "build $builddir/inner: innerrule\n"
    "build inner2: innerrule\n");

  ASSERT_NO_FATAL_FAILURE(AssertParse(
"builddir = a/\n"
"rule varref\n"
"  command = varref $var\n"
"var = outer\n"
"build $builddir/outer: varref\n"
"subninja a/foo.ninja\n"
"  chdir = a\n"
"build $builddir/outer2: varref\n"));
  ASSERT_EQ(1u, fs_.files_read_.size());
  ASSERT_EQ("/", VerifyCwd(fs_));

  EXPECT_EQ("a/foo.ninja", fs_.files_read_[0]);
  HashedStrView outer("a/outer");
  EXPECT_TRUE(state.LookupNode(GlobalPathStr{outer}));
  HashedStrView outer2("a/outer2");
  EXPECT_TRUE(state.LookupNode(GlobalPathStr{outer2}));
  // Verify builddir setting is *not* inherited.
  HashedStrView inner("a/inner");
  EXPECT_FALSE(state.LookupNode(GlobalPathStr{inner}));
  // Verify that the chdir "a/" is always concatenated with the filename.
  // (in this case, the filename is /inner because a/foo.ninja says
  // "build $bulddir/inner" and this validates that builddir from
  // the unnamed top-level .ninja file is not visible in a/foo.ninja)
  HashedStrView innerslash("a/" "/inner");
  EXPECT_TRUE(state.LookupNode(GlobalPathStr{innerslash}));
  HashedStrView inner2("a/inner2");
  Node* inner2node = state.LookupNode(GlobalPathStr{inner2});
  EXPECT_TRUE(inner2node);
  HashedStrView slashinner("/inner");
  EXPECT_FALSE(state.LookupNode(GlobalPathStr{slashinner}));

  // Verify command includes correct chdir for subninja chdir.
  Edge* edge = inner2node->in_edge();
  EXPECT_TRUE(edge);
#ifdef _WIN32
  EXPECT_EQ(NINJA_WIN32_CD_DELIM "a/" NINJA_WIN32_CD_DELIM "foo inner", edge->EvaluateCommand());
#else /* _WIN32 */
  EXPECT_EQ("cd \"a/\" && foo inner", edge->EvaluateCommand());
#endif /* _WIN32 */
}

TEST_F(ParserTest, SubNinjaChdirNoSuchFile) {
  EXPECT_TRUE(fs_.MakeDir("a"));
  fs_.Create("a/foo.ninja",
    "var = inner\n"
    "rule innerrule\n"
    "  command = foo $var\n"
    "build $builddir/inner: innerrule\n"
    "build inner2: innerrule\n");

  ManifestParser parser(&state, &fs_);
  string err;
  EXPECT_FALSE(parser.ParseTest(
"builddir = a/\n"
"rule varref\n"
"  command = varref $var\n"
"var = outer\n"
"build $builddir/outer: varref\n"
"subninja foo.ninja\n"  // NOTE: deliberately wrong, not "a/foo.ninja"
"  chdir = a\n"
"build $builddir/outer2: varref\n", &err));
  EXPECT_EQ(err,
      "input:6: loading 'foo.ninja': No such file or directory\n"
      "subninja foo.ninja\n"
      "                  ^ near here");
  ASSERT_EQ(1u, fs_.files_read_.size());
  ASSERT_EQ("/", VerifyCwd(fs_));
}

TEST_F(ParserTest, TwoChdirs) {
  fs_.MakeDir("test-a");
  fs_.Create("test-a/a.ninja",
    "rule cat\n"
    "  command = cat $in > $out\n"
    "build out2: cat in1\n"
    "build out1: cat in2\n"
    "build final: cat out1\n"
    "default final\n");

  fs_.MakeDir("test-b");
  fs_.Create("test-b/b.ninja",
    "rule cat\n"
    "  command = cat $in > $out\n"
    "build out2: cat in1\n"
    "build out3: cat in2\n"
    "build final: cat out3\n"
    "default final\n");

  // Verify that duplicate 'default final' lines in subninjas do not conflict.
  // Verify that 'default final' in a chdir is ignored.
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"subninja test-a/a.ninja\n"
"    chdir = test-a\n"
"\n"
"rule pipe-through-test-a\n"
"    command = $in | test-a/final > $out\n"
"\n"
"build foo: pipe-through-test-a | test-b/final\n"
"default foo\n"
"\n"
"subninja test-b/b.ninja\n"
"    chdir = test-b\n"
  ));
  EXPECT_EQ("/", VerifyCwd(fs_));  // Verify cwd was restored

  // Verify edge command includes correct 'cd test-a'
  Edge* edge = GetNode("test-a/final")->in_edge();
#if _WIN32
  EXPECT_EQ(NINJA_WIN32_CD_DELIM "test-a/" NINJA_WIN32_CD_DELIM
            "cat out1 > final", edge->EvaluateCommand());
#else
  EXPECT_EQ("cd \"test-a/\" && cat out1 > final", edge->EvaluateCommand());
#endif

  // Verify edge command includes correct 'cd test-b'
  edge = GetNode("test-b/final")->in_edge();
#if _WIN32
  EXPECT_EQ(NINJA_WIN32_CD_DELIM "test-b/" NINJA_WIN32_CD_DELIM
            "cat out3 > final", edge->EvaluateCommand());
#else
  EXPECT_EQ("cd \"test-b/\" && cat out3 > final", edge->EvaluateCommand());
#endif
}

TEST_F(ParserTest, SubNinjaErrors) {
  {
    // Test chdir failure.
    VirtualFileSystem fs;
    fs.MakeDir("test-a");
    fs.Create("test-a/a.ninja",
      "rule cat\n"
      "  command = cat $in > $out\n"
      "build out2: cat in1\n"
      "build out1: cat in2\n"
      "build final: cat out1\n");
    State local_state;
    ManifestParser parser(&local_state, &fs);
    string err;
    EXPECT_FALSE(parser.ParseTest(
"rule pipe-through-test-a\n"
"    command = $in | test-a/final > $out\n"
"\n"
"build foo: pipe-through-test-a | test-a/final\n"
"\n"
"default foo\n"
"\n"
"subninja test-b/a.ninja\n"
"    chdir = test-b\n", &err));
    EXPECT_EQ(
        "input:8: loading 'test-b/a.ninja': No such file or directory\n"
        "subninja test-b/a.ninja\n"
        "                       ^ near here", err);
    EXPECT_EQ("/", VerifyCwd(fs));  // Verify cwd was restored
  }

  {
    // Test that an error in a.ninja flows through the chdir.
    // Verify that 'unknown target' includes the chdir in the message.
    VirtualFileSystem fs;
    fs.MakeDir("test-a");
    fs.Create("test-a/a.ninja",
      "rule cat\n"
      "  command = cat $in > $out\n"
      "build out2: cat in1\n"
      "build out1: cat in2\n"
      "build final: cat out1\n"
      "default somethingweird\n");
    ManifestParserOptions parser_opts;
    parser_opts.dupe_edge_action_ = kDupeEdgeActionError;
    State local_state;
    ManifestParser parser(&local_state, &fs, parser_opts);
    string err;
    EXPECT_FALSE(parser.ParseTest(
"rule pipe-through-test-a\n"
"    command = $in | test-a/final > $out\n"
"\n"
"build foo: pipe-through-test-a | test-a/final\n"
"\n"
"default foo\n"
"\n"
"subninja test-a/a.ninja\n"
"    chdir = test-a\n", &err));
    EXPECT_EQ("test-a/a.ninja:6: unknown target 'somethingweird'\n"
              "default somethingweird\n"
              "                      ^ near here"
              , err);
    EXPECT_EQ("/", VerifyCwd(fs));  // Verify cwd was restored
  }

  {
    // Test missing subninja.
    ManifestParser parser(&state, &fs_);
    string err;
    EXPECT_FALSE(parser.ParseTest("subninja foo.ninja\n", &err));
    EXPECT_EQ("input:1: loading 'foo.ninja': No such file or directory\n"
              "subninja foo.ninja\n"
              "                  ^ near here"
              , err);
  }
}

TEST_F(ParserTest, DuplicateRuleInDifferentSubninjas) {
  // Test that rules are scoped to subninjas.
  fs_.Create("test.ninja", "rule cat\n"
                         "  command = cat\n");
  ManifestParser parser(&state, &fs_);
  string err;
  EXPECT_TRUE(parser.ParseTest("rule cat\n"
                                "  command = cat\n"
                                "subninja test.ninja\n", &err));
}

TEST_F(ParserTest, DuplicateRuleInDifferentSubninjasWithInclude) {
  // Test that rules are scoped to subninjas even with includes.
  fs_.Create("rules.ninja", "rule cat\n"
                         "  command = cat\n");
  fs_.Create("test.ninja", "include rules.ninja\n"
                         "build x : cat\n");
  ManifestParser parser(&state, &fs_);
  string err;
  EXPECT_TRUE(parser.ParseTest("include rules.ninja\n"
                                "subninja test.ninja\n"
                                "build y : cat\n", &err));
}

TEST_F(ParserTest, Include) {
  fs_.Create("include.ninja", "var = inner\n");
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"var = outer\n"
"include include.ninja\n"));

  ASSERT_EQ(1u, fs_.files_read_.size());
  EXPECT_EQ("include.ninja", fs_.files_read_[0]);
  EXPECT_EQ("inner", state.root_scope_.LookupVariable("var"));
}

TEST_F(ParserTest, IncludeErrors) {
  fs_.Create("include.ninja", "build\n");
  {
    ManifestParser parser(&state, &fs_);
    string err;
    EXPECT_FALSE(parser.ParseTest("include include.ninja\n", &err));
    EXPECT_EQ("include.ninja:1: expected path\n"
              "build\n"
              "     ^ near here"
              , err);
  }
  {
    ManifestParser parser(&state, &fs_);
    string err;
    EXPECT_FALSE(parser.ParseTest(
        "include include.ninja\n"
        "  chdir = somedir\n", &err));
    EXPECT_EQ("input:2: indent after 'include' line is invalid.\n"
              , err);
  }
  {
    ManifestParser parser(&state, &fs_);
    string err;
    EXPECT_FALSE(parser.ParseTest(
        "subninja include.ninja\n"
        "  somedir = somedir\n", &err));
    EXPECT_EQ("input:2: only 'chdir =' is allowed after 'subninja' line.\n"
              "  somedir = somedir\n"
              "  ^ near here"
              , err);
  }
  {
    ManifestParser parser(&state, &fs_);
    string err;
    EXPECT_FALSE(parser.ParseTest(
        "subninja include.ninja\n"
        "  chdir dir\n", &err));
    EXPECT_EQ("input:2: expected '=', got identifier\n"
              "  chdir dir\n"
              "        ^ near here"
              , err);
  }
}

TEST_F(ParserTest, Implicit) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"rule cat\n"
"  command = cat $in > $out\n"
"build foo: cat bar | baz\n"));

  Edge* edge = LookupNode("foo")->in_edge();
  ASSERT_TRUE(edge->is_implicit(1));
}

TEST_F(ParserTest, OrderOnly) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"rule cat\n  command = cat $in > $out\n"
"build foo: cat bar || baz\n"));

  Edge* edge = LookupNode("foo")->in_edge();
  ASSERT_TRUE(edge->is_order_only(1));
}

TEST_F(ParserTest, Validations) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"rule cat\n  command = cat $in > $out\n"
"build foo: cat bar |@ baz\n"));

  Edge* edge = LookupNode("foo")->in_edge();
  ASSERT_EQ(edge->validations_.size(), 1);
  EXPECT_EQ(edge->validations_[0]->path(), "baz");
}

TEST_F(ParserTest, ImplicitOutput) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"rule cat\n"
"  command = cat $in > $out\n"
"build foo | imp: cat bar\n"));

  Edge* edge = LookupNode("imp")->in_edge();
  ASSERT_EQ(edge->outputs_.size(), 2);
  EXPECT_TRUE(edge->is_implicit_out(1));
}

TEST_F(ParserTest, ImplicitOutputEmpty) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"rule cat\n"
"  command = cat $in > $out\n"
"build foo | : cat bar\n"));

  Edge* edge = LookupNode("foo")->in_edge();
  ASSERT_EQ(edge->outputs_.size(), 1);
  EXPECT_FALSE(edge->is_implicit_out(0));
}

TEST_F(ParserTest, ImplicitOutputDupe) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"rule cat\n"
"  command = cat $in > $out\n"
"build foo baz | foo baq foo: cat bar\n"));

  Edge* edge = LookupNode("foo")->in_edge();
  ASSERT_EQ(edge->outputs_.size(), 3);
  EXPECT_FALSE(edge->is_implicit_out(0));
  EXPECT_FALSE(edge->is_implicit_out(1));
  EXPECT_TRUE(edge->is_implicit_out(2));
}

TEST_F(ParserTest, ImplicitOutputDupes) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"rule cat\n"
"  command = cat $in > $out\n"
"build foo foo foo | foo foo foo foo: cat bar\n"));

  Edge* edge = LookupNode("foo")->in_edge();
  ASSERT_EQ(edge->outputs_.size(), 1);
  EXPECT_FALSE(edge->is_implicit_out(0));
}

TEST_F(ParserTest, NoExplicitOutput) {
  ManifestParser parser(&state, NULL);
  string err;
  EXPECT_TRUE(parser.ParseTest(
"rule cat\n"
"  command = cat $in > $out\n"
"build | imp : cat bar\n", &err));
}

TEST_F(ParserTest, DefaultDefault) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"rule cat\n  command = cat $in > $out\n"
"build a: cat foo\n"
"build b: cat foo\n"
"build c: cat foo\n"
"build d: cat foo\n"));

  string err;
  EXPECT_EQ(4u, state.DefaultNodes(&err).size());
  EXPECT_EQ("", err);
}

TEST_F(ParserTest, DefaultDefaultCycle) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"rule cat\n  command = cat $in > $out\n"
"build a: cat a\n"));

  string err;
  EXPECT_EQ(0u, state.DefaultNodes(&err).size());
  EXPECT_EQ("could not determine root nodes of build graph", err);
}

TEST_F(ParserTest, DefaultStatements) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"rule cat\n  command = cat $in > $out\n"
"build a: cat foo\n"
"build b: cat foo\n"
"build c: cat foo\n"
"build d: cat foo\n"
"third = c\n"
"default a b\n"
"default $third\n"));

  string err;
  vector<Node*> nodes = state.DefaultNodes(&err);
  EXPECT_EQ("", err);
  ASSERT_EQ(3u, nodes.size());
  EXPECT_EQ("a", nodes[0]->path());
  EXPECT_EQ("b", nodes[1]->path());
  EXPECT_EQ("c", nodes[2]->path());
}

TEST_F(ParserTest, UTF8) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"rule utf8\n"
"  command = true\n"
"  description = compilaci\xC3\xB3\n"));
}

TEST_F(ParserTest, CRLF) {
  State local_state;
  ManifestParser parser(&local_state, NULL);
  string err;

  EXPECT_TRUE(parser.ParseTest("# comment with crlf\r\n", &err));
  EXPECT_TRUE(parser.ParseTest("foo = foo\nbar = bar\r\n", &err));
  EXPECT_TRUE(parser.ParseTest(
      "pool link_pool\r\n"
      "  depth = 15\r\n\r\n"
      "rule xyz\r\n"
      "  command = something$expand \r\n"
      "  description = YAY!\r\n",
      &err));
}

TEST_F(ParserTest, DyndepNotSpecified) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"rule cat\n"
"  command = cat $in > $out\n"
"build result: cat in\n"));
  Edge* edge = GetNode("result")->in_edge();
  ASSERT_FALSE(edge->dyndep_);
}

TEST_F(ParserTest, DyndepNotInput) {
  State lstate;
  ManifestParser parser(&lstate, NULL);
  string err;
  EXPECT_FALSE(parser.ParseTest(
"rule touch\n"
"  command = touch $out\n"
"build result: touch\n"
"  dyndep = notin\n",
                               &err));
  EXPECT_EQ("input:5: dyndep 'notin' is not an input\n", err);
}

TEST_F(ParserTest, DyndepExplicitInput) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"rule cat\n"
"  command = cat $in > $out\n"
"build result: cat in\n"
"  dyndep = in\n"));
  Edge* edge = GetNode("result")->in_edge();
  ASSERT_TRUE(edge->dyndep_);
  EXPECT_TRUE(edge->dyndep_->dyndep_pending());
  EXPECT_EQ(edge->dyndep_->path(), "in");
}

TEST_F(ParserTest, DyndepImplicitInput) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"rule cat\n"
"  command = cat $in > $out\n"
"build result: cat in | dd\n"
"  dyndep = dd\n"));
  Edge* edge = GetNode("result")->in_edge();
  ASSERT_TRUE(edge->dyndep_);
  EXPECT_TRUE(edge->dyndep_->dyndep_pending());
  EXPECT_EQ(edge->dyndep_->path(), "dd");
}

TEST_F(ParserTest, DyndepOrderOnlyInput) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"rule cat\n"
"  command = cat $in > $out\n"
"build result: cat in || dd\n"
"  dyndep = dd\n"));
  Edge* edge = GetNode("result")->in_edge();
  ASSERT_TRUE(edge->dyndep_);
  EXPECT_TRUE(edge->dyndep_->dyndep_pending());
  EXPECT_EQ(edge->dyndep_->path(), "dd");
}

TEST_F(ParserTest, DyndepRuleInput) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"rule cat\n"
"  command = cat $in > $out\n"
"  dyndep = $in\n"
"build result: cat in\n"));
  Edge* edge = GetNode("result")->in_edge();
  ASSERT_TRUE(edge->dyndep_);
  EXPECT_TRUE(edge->dyndep_->dyndep_pending());
  EXPECT_EQ(edge->dyndep_->path(), "in");
}

TEST_F(ParserTest, IncludeUsingAVariable) {
  // Each include statement should use the $path binding from the previous
  // manifest declarations.
  fs_.Create("foo.ninja", "path = bar.ninja\n"
                          "include $path\n");
  fs_.Create("bar.ninja", "");
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"path = foo.ninja\n"
"include $path\n"
"include $path\n"
"path = nonexistent.ninja\n"));
}

TEST_F(ParserTest, UnscopedPool) {
  // Pools aren't scoped.
  fs_.Create("foo.ninja", "pool link\n"
                          "  depth = 3\n");
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"rule cat\n"
"  command = cat $in > $out\n"
"  pool = link\n"
"subninja foo.ninja\n"
"build a: cat b\n"));
}

TEST_F(ParserTest, PoolDeclaredAfterUse) {
  // A pool must be declared before an edge that uses it.
  ManifestParser parser(&state, nullptr);
  std::string err;
  EXPECT_FALSE(parser.ParseTest("rule cat\n"
                                "  command = cat $in > $out\n"
                                "  pool = link\n"
                                "build a: cat b\n"
                                "pool link\n"
                                "  depth = 3\n", &err));
  EXPECT_EQ("input:5: unknown pool name 'link'\n", err);
}

TEST_F(ParserTest, DefaultReferencesEdgeInput) {
  // The default statement's target must be listed before the default statement.
  // It's OK if the target's first reference is an input, though.
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"rule cat\n  command = cat $in > $out\n"
"build a1: cat b1\n"
"build a2: cat b2\n"
"default b1 b2\n"
"build b1: cat c1\n"));
}

TEST_F(ParserTest, SelfVarExpansion) {
  ASSERT_NO_FATAL_FAILURE(AssertParse(
"var = xxx\n"
"var1 = ${var}\n"
"var = a${var}a\n"
"var2 = ${var}\n"
"var = b${var}b\n"
"var3 = ${var}\n"
"var = c${var}c\n"));
  EXPECT_EQ("xxx", state.root_scope_.LookupVariable("var1"));
  EXPECT_EQ("axxxa", state.root_scope_.LookupVariable("var2"));
  EXPECT_EQ("baxxxab", state.root_scope_.LookupVariable("var3"));
  EXPECT_EQ("cbaxxxabc", state.root_scope_.LookupVariable("var"));
}
