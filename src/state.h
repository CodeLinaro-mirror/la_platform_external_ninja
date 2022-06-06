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

#ifndef NINJA_STATE_H_
#define NINJA_STATE_H_

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
using namespace std;

#include "eval_env.h"
#include "graph.h"
#include "string_piece.h"
#include "concurrent_hash_map.h"
#include "util.h"

struct Edge;
struct Node;
struct Rule;

/// A pool for delayed edges.
/// Pools are scoped to a State. Edges within a State will share Pools. A Pool
/// will keep a count of the total 'weight' of the currently scheduled edges. If
/// a Plan attempts to schedule an Edge which would cause the total weight to
/// exceed the depth of the Pool, the Pool will enqueue the Edge instead of
/// allowing the Plan to schedule it. The Pool will relinquish queued Edges when
/// the total scheduled weight diminishes enough (i.e. when a scheduled edge
/// completes).
struct Pool {
  Pool() {}

  /// Constructor for built-in pools.
  Pool(const HashedStrView& name, int depth);

  // A depth of 0 is infinite
  bool is_valid() const { return depth_ >= 0; }
  int depth() const { return depth_; }
  DeclIndex dfs_location() const { return pos_.dfs_location(); }
  const std::string& name() const { return name_.str(); }
  const HashedStr& name_hashed() const { return name_; }
  int current_use() const { return current_use_; }

  /// true if the Pool might delay this edge
  bool ShouldDelayEdge() const { return depth_ != 0; }

  /// informs this Pool that the given edge is committed to be run.
  /// Pool will count this edge as using resources from this pool.
  void EdgeScheduled(const Edge& edge);

  /// informs this Pool that the given edge is no longer runnable, and should
  /// relinquish its resources back to the pool
  void EdgeFinished(const Edge& edge);

  /// adds the given edge to this Pool to be delayed.
  void DelayEdge(Edge* edge);

  /// Pool will add zero or more edges to the ready_queue
  void RetrieveReadyEdges(EdgeSet* ready_queue);

  /// Dump the Pool and its edges (useful for debugging).
  void Dump() const;

  HashedStr name_;
  int depth_ = 0;
  RelativePosition pos_;

  // Temporary fields used only during manifest parsing.
  struct {
    size_t pool_name_diag_pos = 0;
    StringPiece depth;
    size_t depth_diag_pos = 0;
  } parse_state_;

private:
  /// |current_use_| is the total of the weights of the edges which are
  /// currently scheduled in the Plan (i.e. the edges in Plan::ready_).
  int current_use_ = 0;

  struct WeightedEdgeCmp {
    bool operator()(const Edge* a, const Edge* b) const {
      if (!a) return b;
      if (!b) return false;
      int weight_diff = a->weight() - b->weight();
      return ((weight_diff < 0) || (weight_diff == 0 && EdgeCmp()(a, b)));
    }
  };

  typedef set<Edge*, WeightedEdgeCmp> DelayedEdges;
  DelayedEdges delayed_;
};

/// Global state (file status) for a single run.
struct State {
  /// The built-in pools and rules use this dummy built-in scope to initialize
  /// their RelativePosition fields. The scope won't have anything in it.
  static Scope kBuiltinScope;

  static Pool kDefaultPool;
  static Pool kConsolePool;
  static Rule kPhonyRule;

  State();

  void AddBuiltinRule(Rule* rule);
  bool AddPool(Pool* pool);
  Edge* AddEdge(const Rule* rule);
  Pool* LookupPool(const HashedStrView& pool_name);

  /// Lookup a pool at a DFS position. Pools don't respect scopes. A pool's name
  /// must be unique across the entire manifest, and it must be declared before
  /// an edge references it.
  Pool* LookupPoolAtPos(const HashedStrView& pool_name, DeclIndex dfs_location);

  /// Creates the node if it doesn't exist. Never returns nullptr. Thread-safe.
  /// 'path' must be the Scope::GlobalPath(), globally unique.
  /// The hash table should be resized ahead of time for decent performance.
  Node* GetNode(GlobalPathStr path, uint64_t slash_bits);

  /// Finds the existing node, returns nullptr if it doesn't exist yet.
  /// 'path' must be the Scope::GlobalPath(), globally unique.
  /// Thread-safe.
  Node* LookupNode(GlobalPathStr path) const;
  Node* LookupNodeAtPos(GlobalPathStr path, DeclIndex dfs_location) const;

  Node* SpellcheckNode(StringPiece path);

  /// 'path' must be the Scope::GlobalPath(), globally unique.
  /// These methods aren't thread-safe.
  void AddIn(Edge* edge, GlobalPathStr path, uint64_t slash_bits);
  bool AddOut(Edge* edge, GlobalPathStr path, uint64_t slash_bits);
  void AddDefault(Node* node) { defaults_.push_back(node); }

  /// Reset state.  Keeps all nodes and edges, but restores them to the
  /// state where we haven't yet examined the disk for dirty state.
  void Reset();

  /// Dump the nodes and Pools (useful for debugging).
  void Dump();

  /// @return the root node(s) of the graph. (Root nodes have no output edges).
  /// @param error where to write the error message if somethings went wrong.
  vector<Node*> RootNodes(string* error) const;
  vector<Node*> DefaultNodes(string* error) const;

  DeclIndex AllocDfsLocation(DeclIndex count);

  /// Mapping of path -> Node.
  ///
  /// This hash map uses a value type of Node* rather than Node, which allows the
  /// map entry's key (a string view) to refer to the Node::name field.
  typedef ConcurrentHashMap<HashedStrView, Node*> Paths;
  Paths paths_;

  /// All the pools used in the graph.
  std::unordered_map<HashedStrView, Pool*> pools_;

  /// All the edges of the graph.
  vector<Edge*> edges_;

  Scope root_scope_ { ScopePosition {} };
  vector<Node*> defaults_;

private:
  /// Position 0 is used for built-in decls (e.g. pools).
  DeclIndex dfs_location_ = 1;
};

#endif  // NINJA_STATE_H_
