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

#include "eval_env.h"

#include <assert.h>

#include "graph.h"
#include "state.h"

Rule::Rule(const HashedStrView& name) : name_(name) {
  pos_.base = new BasePosition {{ &State::kBuiltinScope, 0 }}; // leaked
}

bool Rule::IsReservedBinding(StringPiece var) {
  // Cycle detection for rule variable evaluation uses a fixed recursion depth
  // that's guaranteed to be larger than the number of reserved binding names
  // listed below.
  static_assert(EdgeEval::kEvalRecursionLimit == 16,
                "Unexpected rule variable evaluation recursion limit");

  return var == "command" ||
      var == "depfile" ||
      var == "dyndep" ||
      var == "description" ||
      var == "deps" ||
      var == "generator" ||
      var == "pool" ||
      var == "restat" ||
      var == "rspfile" ||
      var == "rspfile_content" ||
      var == "phony_output" ||
      var == "symlink_outputs" ||
      var == "msvc_deps_prefix";
}

void Binding::Evaluate(std::string* out_append) {
  if (is_evaluated_.load()) {
    out_append->append(final_value_);
    return;
  }

  std::string str;
  EvaluateBindingInScope(&str, parsed_value_, pos_.scope_pos());
  out_append->append(str);

  // Try to store the result so we can use it again later. If we can't acquire
  // the lock, then another thread has already acquired it and will set the
  // final binding value.
  std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
  if (!lock.owns_lock())
    return;

  // Check the flag again. Another thread could have set it before this thread
  // acquired the lock, and if it had, then other threads could already be using
  // this binding's saved value.
  if (is_evaluated_.load())
    return;

  final_value_ = std::move(str);
  is_evaluated_.store(true);
}

template <typename K, typename V>
static void ExpandTable(std::unordered_map<K, V>& table, size_t extra_size) {
  size_t needed_buckets = table.size() + extra_size + 1;
  if (table.bucket_count() >= needed_buckets)
    return;
  table.rehash(needed_buckets * 2);
};

void Scope::ReserveTableSpace(size_t new_bindings, size_t new_rules) {
  ExpandTable(bindings_, new_bindings);
  ExpandTable(rules_, new_rules);
}

std::map<std::string, const Rule*> Scope::GetRules() const {
  std::map<std::string, const Rule*> result;
  for (std::pair<HashedStrView, Rule*> pair : rules_) {
    Rule* rule = pair.second;
    result[rule->name()] = rule;
  }
  return result;
}

Binding* Scope::LookupBindingAtPos(const HashedStrView& var, ScopePosition pos) {
  Scope* scope = pos.scope;
  if (scope == nullptr) return nullptr;

  auto it = scope->bindings_.find(var);
  if (it == scope->bindings_.end())
    return LookupBindingAtPos(var, scope->parent_);

  struct BindingCmp {
    bool operator()(Binding* x, DeclIndex y) const {
      return x->pos_.scope_index() < y;
    }
  };

  // A binding "Foo = $Foo" is valid; the "$Foo" is not a self-reference but
  // a reference to the previous binding of Foo. The evaluation of "$Foo"
  // happens with the same DeclIndex as the "Foo = $Foo" binding, so we want
  // to find a binding whose ScopePosition is strictly less than the one we're
  // searching for, not equal to it.
  std::vector<Binding*>& decls = it->second;
  auto it2 = std::lower_bound(
    decls.begin(), decls.end(),
    pos.index, BindingCmp());
  if (it2 == decls.begin())
    return LookupBindingAtPos(var, scope->parent_);
  return *(it2 - 1);
}

Binding* Scope::LookupBinding(const HashedStrView& var, Scope* scope) {
  if (scope == nullptr) return nullptr;

  auto it = scope->bindings_.find(var);
  if (it == scope->bindings_.end()) {
    // When we delegate to the parent scope, we match bindings at the end of the
    // parent's scope, even if they weren't in scope when the subninja scope was
    // parsed. The behavior matters after parsing is complete and we're
    // evaluating edge bindings that involve rule variable expansions.
    return LookupBinding(var, scope->parent_.scope);
  }

  std::vector<Binding*>& decls = it->second;
  assert(!decls.empty());

  // Evaluate the binding.
  return decls.back();
}

void Scope::EvaluateVariableAtPos(std::string* out_append,
                                  const HashedStrView& var, ScopePosition pos) {
  if (Binding* binding = LookupBindingAtPos(var, pos))
    binding->Evaluate(out_append);
}

void Scope::EvaluateVariable(std::string* out_append, const HashedStrView& var,
                             Scope* scope) {
  if (Binding* binding = LookupBinding(var, scope))
    binding->Evaluate(out_append);
}

std::string Scope::LookupVariable(const HashedStrView& var) {
  std::string result;
  EvaluateVariable(&result, var, this);
  return result;
}

Rule* Scope::LookupRuleAtPos(const HashedStrView& rule_name,
                             ScopePosition pos) {
  Scope* scope = pos.scope;
  if (scope == nullptr) return nullptr;
  auto it = scope->rules_.find(rule_name);
  if (it != scope->rules_.end()) {
    Rule* rule = it->second;
    if (rule->pos_.scope_index() < pos.index)
      return rule;
  }
  return LookupRuleAtPos(rule_name, scope->parent_);
}

GlobalPathStr Scope::GlobalPath(const HashedStrView& path) {
  if (chdir_.empty()) {
    return GlobalPathStr{path};
  }

  std::lock_guard<std::mutex> lock(global_paths_mutex_);
  auto it = global_paths_.find(path);
  if (it != global_paths_.end()) {
    return GlobalPathStr{*it->second};
  }

  auto global = make_shared<HashedStr>(chdir_ + path.str_view().AsString());
  auto result = global_paths_.insert(
      std::pair<HashedStrView, std::shared_ptr<HashedStr>>(path, global));

  const HashedStr &h = *result.first->second;
  return GlobalPathStr{HashedStrView(h)};
}

void Scope::resetGlobalPath(const std::string& path) {
  HashedStr hpath(path);
  std::lock_guard<std::mutex> lock(global_paths_mutex_);
  auto it = global_paths_.find(hpath);
  if (it != global_paths_.end()) {
    global_paths_.erase(it);
  }
}

std::string Scope::ResolveChdir(Scope* child, std::string path) {
  Scope* it = child;
  while (it != NULL && it != this) {
    if (!it->chdir().empty())
      // path was relative to 'child'. Now make it relative to 'it'.
      path = it->chdir() + path;

    Scope* parent = it->parent_.scope;
    parent = (!parent) ? it->chdirParent_ : parent;
    if (!parent && it->chdirParent_ == NULL) {
      Warning("Node \"%s\" not in a child of target %p \"%s\"\n", path.c_str(),
              this, chdir().c_str());
      break;
    }
    it = parent;
  }
  return path;
}
