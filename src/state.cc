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

#include "state.h"

#include <assert.h>
#include <stdio.h>

#include "edit_distance.h"
#include "graph.h"
#include "metrics.h"
#include "util.h"

Pool::Pool(const HashedStrView& name, int depth) : name_(name), depth_(depth) {
  pos_.base = new BasePosition {{ &State::kBuiltinScope, 0 }}; // leaked
}

void Pool::EdgeScheduled(const Edge& edge) {
  if (depth_ != 0)
    current_use_ += edge.weight();
}

void Pool::EdgeFinished(const Edge& edge) {
  if (depth_ != 0)
    current_use_ -= edge.weight();
}

void Pool::DelayEdge(Edge* edge) {
  assert(depth_ != 0);
  delayed_.insert(edge);
}

void Pool::RetrieveReadyEdges(EdgeSet* ready_queue) {
  DelayedEdges::iterator it = delayed_.begin();
  while (it != delayed_.end()) {
    Edge* edge = *it;
    if (current_use_ + edge->weight() > depth_)
      break;
    ready_queue->insert(edge);
    EdgeScheduled(*edge);
    ++it;
  }
  delayed_.erase(delayed_.begin(), it);
}

void Pool::Dump() const {
  printf("%s (%d/%d) ->\n", name_.c_str(), current_use_, depth_);
  for (DelayedEdges::const_iterator it = delayed_.begin();
       it != delayed_.end(); ++it)
  {
    printf("\t");
    (*it)->Dump();
  }
}

Scope State::kBuiltinScope({});
Pool State::kDefaultPool("", 0);
Pool State::kConsolePool("console", 1);
Rule State::kPhonyRule("phony");

State::State() {
  // Reserve scope position (root, 0) for built-in rules.
  root_scope_.AllocDecls(1);

  root_scope_.AddAllBuiltinRules();
  AddPool(&kDefaultPool, &root_scope_);
  AddPool(&kConsolePool, &root_scope_);
}

bool State::AddPool(Pool* pool, Scope* scope) {
  // Find root scope of this subninja chdir.
  while (scope->parent() != nullptr) {
    scope = scope->parent();
  }
  pool->parent_ = scope;

  auto r = pools_.insert({ pool->name_hashed(), {} });
  if (r.second) {
    // This pool has a unique name, so a new SameNamePools was inserted.
    bool poolUnique = r.first->second.pool.insert({ scope, pool }).second;
    if (!poolUnique) {
      Fatal("SameNamePools should be empty, but says it has a duplicate.");
      return false;
    }
    return true;
  }

  // This pool does not have a unique name. Nothing was changed in pools_.
  SameNamePools& sims = r.first->second;
  auto i = sims.pool.find(scope);
  if (i != sims.pool.end()) {
    return false;  // Found a duplicate in the same subninja chdir.
  }
  // This pool is unique within its subninja chdir.
  // If it has the same depth as any pool in r.first.pool, merge it.
  for (i = sims.pool.begin(); i != sims.pool.end(); ++i) {
    if (i->second->depth_ == pool->depth_) {
      // The Pool* pool is not used. The previous Pool* in i->second is kept.
      // However, the previous Pool* is now available in this scope.
      bool poolUnique = sims.pool.insert({ scope, i->second }).second;
      if (!poolUnique) {
        Fatal("SameNamePools should not have a duplicate, but does.");
        return false;
      }
      return true;
    }
  }

  // This pool is unique within its subninja chdir and unique globally.
  bool poolUnique = sims.pool.insert({ scope, pool }).second;
  if (!poolUnique) {
    Fatal("SameNamePools did not accept another pool.");
    return false;
  }
  return true;
}

Edge* State::AddEdge(const Rule* rule) {
  Edge* edge = new Edge();
  edge->pos_.base = new BasePosition {{ &root_scope_, 0 }}; // leaked
  edge->onPosResolvedToScope(&root_scope_);
  edge->rule_ = rule;
  edge->pool_ = &State::kDefaultPool;
  edge->id_ = edges_.size();
  edges_.push_back(edge);
  return edge;
}

Pool* State::LookupPool(const HashedStrView& pool_name, const ScopePosition edge_pos) {
  if (edge_pos.scope == nullptr) {
    Fatal("LookupPool(%s): edge_pos.scope is NULL",
          pool_name.str_view().AsString().c_str());
    return nullptr;
  }
  // Find root scope of this subninja chdir.
  Scope* parent = edge_pos.scope;
  while (parent->parent() != nullptr) {
    parent = parent->parent();
  }

  auto i = pools_.find(pool_name);
  if (i == pools_.end())
    return nullptr;
  auto j = i->second.pool.find(parent);
  if (j == i->second.pool.end()) {
    return nullptr;
  }
  return j->second;
}

Pool* State::LookupPoolAtPos(const HashedStrView& pool_name, const ScopePosition edge_pos,
                             DeclIndex dfs_location) {
  Pool* result = LookupPool(pool_name, edge_pos);
  if (result == nullptr) return nullptr;
  return result->dfs_location() < dfs_location ? result : nullptr;
}

Node* State::GetNode(GlobalPathStr pathG, uint64_t slash_bits) {
  if (Node** opt_node = paths_.Lookup(pathG.h))
    return *opt_node;
  // Create a new node and try to insert it.
  // All nodes are created in root_scope_. Nodes that live in a chdir scope
  // will be fixed up later.
  std::unique_ptr<Node> node(new Node(pathG.h, &root_scope_, slash_bits));
  pathG = node->globalPath();
  if (paths_.insert({pathG.h, node.get()}).second)
    return node.release();
  // Another thread beat us to it. Use its node instead.
  return *paths_.Lookup(pathG.h);
}

Node* State::LookupNode(GlobalPathStr pathG) const {
  if (Node* const* opt_node = paths_.Lookup(pathG.h))
    return *opt_node;
  return nullptr;
}

Node* State::LookupNodeAtPos(GlobalPathStr pathG,
                             DeclIndex dfs_location) const {
  Node* result = LookupNode(pathG);
  return result && result->dfs_location() < dfs_location ? result : nullptr;
}

Node* State::SpellcheckNode(StringPiece path) {
  const bool kAllowReplacements = true;
  const int kMaxValidEditDistance = 3;

  int min_distance = kMaxValidEditDistance + 1;
  Node* result = NULL;
  for (Paths::iterator i = paths_.begin(); i != paths_.end(); ++i) {
    int distance = EditDistance(
        i->first.str_view(), path, kAllowReplacements, kMaxValidEditDistance);
    if (distance < min_distance && i->second) {
      min_distance = distance;
      result = i->second;
    }
  }
  return result;
}

void State::AddIn(Edge* edge, GlobalPathStr path, uint64_t slash_bits) {
  // Note: AddIn() doesn't actually get called anywhere but unit tests;
  // see manifest_parser.cc AddPathToEdge() which directly mutates
  // edge->inputs_.
  Node* node = GetNode(path, slash_bits);
  edge->inputs_.push_back(node);
  node->AddOutEdge(edge);
}

bool State::AddOut(Edge* edge, GlobalPathStr path, uint64_t slash_bits) {
  // Note: AddOut() doesn't actually get called anywhere but unit tests;
  // see manifest_parser.cc AddPathToEdge() which directly mutates
  // edge->outputs_.
  Node* node = GetNode(path, slash_bits);
  if (node->in_edge())
    return false;
  edge->outputs_.push_back(node);
  node->set_in_edge(edge);
  return true;
}

vector<Node*> State::RootNodes(string* err) const {
  vector<Node*> root_nodes;
  // Search for nodes with no output.
  for (vector<Edge*>::const_iterator e = edges_.begin();
       e != edges_.end(); ++e) {
    for (vector<Node*>::const_iterator out = (*e)->outputs_.begin();
         out != (*e)->outputs_.end(); ++out) {
      if (!(*out)->has_out_edge())
        root_nodes.push_back(*out);
    }
  }

  if (!edges_.empty() && root_nodes.empty())
    *err = "could not determine root nodes of build graph";

  return root_nodes;
}

vector<Node*> State::DefaultNodes(string* err) const {
  return defaults_.empty() ? RootNodes(err) : defaults_;
}

DeclIndex State::AllocDfsLocation(DeclIndex count) {
  DeclIndex result = dfs_location_;
  dfs_location_ += count;
  return result;
}

void State::Reset() {
  for (Paths::iterator i = paths_.begin(); i != paths_.end(); ++i)
    i->second->ResetState();
  for (vector<Edge*>::iterator e = edges_.begin(); e != edges_.end(); ++e) {
    (*e)->outputs_ready_ = false;
    (*e)->deps_loaded_ = false;
    (*e)->mark_ = Edge::VisitNone;
    (*e)->precomputed_dirtiness_ = false;
  }
}

void State::Dump() {
  for (Paths::iterator i = paths_.begin(); i != paths_.end(); ++i) {
    Node* node = i->second;
    printf("%s %s [id:%d]\n",
           node->globalPath().h.data(),
           node->status_known() ? (node->dirty() ? "dirty" : "clean")
                                : "unknown",
           node->id());
  }
  if (!pools_.empty()) {
    printf("resource_pools:\n");
    for (auto it = pools_.begin(); it != pools_.end(); ++it) {
      for (auto sameIt = it->second.pool.begin(); sameIt != it->second.pool.end(); ++sameIt) {
        if (!sameIt->second->name().empty()) {
          sameIt->second->Dump();
        }
      }
    }
  }
}
