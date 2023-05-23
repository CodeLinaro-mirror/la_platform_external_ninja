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

#include "build.h"

#include <assert.h>
#include <errno.h>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <stdio.h>
#include <stdlib.h>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#if defined(__SVR4) && defined(__sun)
#include <sys/termios.h>
#endif

#include "build_log.h"
#include "clparser.h"
#include "debug_flags.h"
#include "depfile_parser.h"
#include "deps_log.h"
#include "disk_interface.h"
#include "graph.h"
#include "metrics.h"
#include "parallel_map.h"
#include "state.h"
#include "status.h"
#include "subprocess.h"
#include "util.h"

namespace {

/// A CommandRunner that doesn't actually run the commands.
struct DryRunCommandRunner : public CommandRunner {
  virtual ~DryRunCommandRunner() {}

  // Overridden from CommandRunner:
  virtual bool CanRunMore();
  virtual bool StartCommand(Edge* edge);
  virtual bool WaitForCommand(Result* result);

 private:
  queue<Edge*> finished_;
};

bool DryRunCommandRunner::CanRunMore() {
  return true;
}

bool DryRunCommandRunner::StartCommand(Edge* edge) {
  finished_.push(edge);
  return true;
}

bool DryRunCommandRunner::WaitForCommand(Result* result) {
   if (finished_.empty())
     return false;

   result->status = ExitSuccess;
   result->edge = finished_.front();
   finished_.pop();
   return true;
}

struct WeightDataSource {
public:
  void Put(std::string&& key, int64_t value) {
    const auto& [it, inserted] = key_data_.emplace(std::move(key));
    if (inserted) {
      map_data_.try_emplace(*it, value);
    } else {
      map_data_.find(*it)->second = value;
    }
  }

  std::optional<int64_t> Get(const HashedStrView& key) const {
    if (auto it = map_data_.find(key); it != map_data_.end()) {
      return it->second;
    }
    return {};
  }
private:
  std::unordered_set<HashedStr> key_data_;
  std::unordered_map<HashedStrView, int64_t> map_data_;
};
}  // namespace

Plan::Plan(Builder* builder)
  : builder_(builder)
  , command_edges_(0)
  , wanted_edges_(0)
{}

void Plan::Reset() {
  command_edges_ = 0;
  wanted_edges_ = 0;
  ready_.clear();
  want_.clear();
}

bool Plan::AddTarget(Node* node, string* err) {
  return AddSubTarget(node, NULL, err, NULL);
}

bool Plan::AddSubTarget(Node* node, Node* dependent, string* err,
                        set<Edge*>* dyndep_walk) {
  Edge* edge = node->in_edge();
  if (!edge) {  // Leaf node.
    if (node->dirty()) {
      string referenced;
      if (dependent)
        referenced = ", needed by '" + dependent->globalPath().h.str_view().AsString() + "',";
      *err = "'" + node->globalPath().h.str_view().AsString() + "'" + referenced +
             " missing and no known rule to make it";
    }
    return false;
  }

  if (edge->outputs_ready())
    return false;  // Don't need to do anything.

  // If an entry in want_ does not already exist for edge, create an entry which
  // maps to kWantNothing, indicating that we do not want to build this entry itself.
  pair<map<Edge*, Want>::iterator, bool> want_ins =
    want_.insert(make_pair(edge, kWantNothing));
  Want& want = want_ins.first->second;

  if (dyndep_walk && want == kWantToFinish)
    return false;  // Don't need to do anything with already-scheduled edge.

  // If we do need to build edge and we haven't already marked it as wanted,
  // mark it now.
  if (node->dirty() && want == kWantNothing) {
    want = kWantToStart;
    EdgeWanted(edge);
    if (!dyndep_walk && edge->AllInputsReady())
      ScheduleWork(want_ins.first);
  }

  if (dyndep_walk)
    dyndep_walk->insert(edge);

  if (!want_ins.second)
    return true;  // We've already processed the inputs.

  for (vector<Node*>::iterator i = edge->inputs_.begin();
       i != edge->inputs_.end(); ++i) {
    if (!AddSubTarget(*i, node, err, dyndep_walk) && !err->empty())
      return false;
  }

  return true;
}

void Plan::EdgeWanted(Edge* edge) {
  ++wanted_edges_;
  if (!edge->is_phony())
    ++command_edges_;
}

Edge* Plan::FindWork() {
  if (ready_.empty())
    return NULL;
  EdgeSet::iterator e = ready_.begin();
  Edge* edge = *e;
  ready_.erase(e);
  return edge;
}

void Plan::ScheduleWork(map<Edge*, Want>::iterator want_e) {
  if (want_e->second == kWantToFinish) {
    // This edge has already been scheduled.  We can get here again if an edge
    // and one of its dependencies share an order-only input, or if a node
    // duplicates an out edge (see https://github.com/ninja-build/ninja/pull/519).
    // Avoid scheduling the work again.
    return;
  }
  assert(want_e->second == kWantToStart);
  want_e->second = kWantToFinish;

  Edge* edge = want_e->first;
  Pool* pool = edge->pool();
  if (pool->ShouldDelayEdge()) {
    pool->DelayEdge(edge);
    pool->RetrieveReadyEdges(&ready_);
  } else {
    pool->EdgeScheduled(*edge);
    ready_.insert(edge);
  }
}

bool Plan::EdgeFinished(Edge* edge, EdgeResult result, string* err) {
  map<Edge*, Want>::iterator e = want_.find(edge);
  assert(e != want_.end());
  bool directly_wanted = e->second != kWantNothing;

  // See if this job frees up any delayed jobs.
  if (directly_wanted)
    edge->pool()->EdgeFinished(*edge);
  edge->pool()->RetrieveReadyEdges(&ready_);

  // The rest of this function only applies to successful commands.
  if (result != kEdgeSucceeded)
    return true;

  if (directly_wanted)
    --wanted_edges_;
  want_.erase(e);
  edge->outputs_ready_ = true;

  // Check off any nodes we were waiting for with this edge.
  for (vector<Node*>::iterator o = edge->outputs_.begin();
       o != edge->outputs_.end(); ++o) {
    if (!NodeFinished(*o, err))
      return false;
  }
  return true;
}

bool Plan::NodeFinished(Node* node, string* err) {
  // If this node provides dyndep info, load it now.
  if (node->dyndep_pending()) {
    assert(builder_ && "dyndep requires Plan to have a Builder");
    // Load the now-clean dyndep file.  This will also update the
    // build plan and schedule any new work that is ready.
    return builder_->LoadDyndeps(node, err);
  }

  // See if we we want any edges from this node.
  const std::vector<Edge*> out_edges = node->GetOutEdges();
  for (vector<Edge*>::const_iterator oe = out_edges.begin();
       oe != out_edges.end(); ++oe) {
    map<Edge*, Want>::iterator want_e = want_.find(*oe);
    if (want_e == want_.end())
      continue;

    // See if the edge is now ready.
    if (!EdgeMaybeReady(want_e, err))
      return false;
  }
  return true;
}

bool Plan::EdgeMaybeReady(map<Edge*, Want>::iterator want_e, string* err) {
  Edge* edge = want_e->first;
  if (edge->AllInputsReady()) {
    if (want_e->second != kWantNothing) {
      ScheduleWork(want_e);
    } else {
      // We do not need to build this edge, but we might need to build one of
      // its dependents.
      if (!EdgeFinished(edge, kEdgeSucceeded, err))
        return false;
    }
  }
  return true;
}

bool Plan::CleanNode(DependencyScan* scan, Node* node, string* err) {
  node->set_dirty(false);

  const std::vector<Edge*> out_edges = node->GetOutEdges();
  for (vector<Edge*>::const_iterator oe = out_edges.begin();
       oe != out_edges.end(); ++oe) {
    // Don't process edges that we don't actually want.
    map<Edge*, Want>::iterator want_e = want_.find(*oe);
    if (want_e == want_.end() || want_e->second == kWantNothing)
      continue;

    // Don't attempt to clean an edge if it failed to load deps.
    if ((*oe)->deps_missing_)
      continue;

    // No need to clean a phony output edge, as it's always dirty
    if ((*oe)->IsPhonyOutput())
      continue;

    // If all non-order-only inputs for this edge are now clean,
    // we might have changed the dirty state of the outputs.
    vector<Node*>::iterator
        begin = (*oe)->inputs_.begin(),
        end = (*oe)->inputs_.end() - (*oe)->order_only_deps_;
#if __cplusplus < 201703L
#define MEM_FN mem_fun
#else
#define MEM_FN mem_fn  // mem_fun was removed in C++17.
#endif
    if (find_if(begin, end, MEM_FN(&Node::dirty)) == end) {
      // Recompute most_recent_input.
      Node* most_recent_input = NULL;
      for (vector<Node*>::iterator i = begin; i != end; ++i) {
        if (!most_recent_input || (*i)->mtime() > most_recent_input->mtime())
          most_recent_input = *i;
      }

      // Now, this edge is dirty if any of the outputs are dirty.
      // If the edge isn't dirty, clean the outputs and mark the edge as not
      // wanted.
      bool outputs_dirty = false;
      if (!scan->RecomputeOutputsDirty(*oe, most_recent_input,
                                       &outputs_dirty, err)) {
        return false;
      }
      if (!outputs_dirty) {
        for (vector<Node*>::iterator o = (*oe)->outputs_.begin();
             o != (*oe)->outputs_.end(); ++o) {
          if (!CleanNode(scan, *o, err))
            return false;
        }

        want_e->second = kWantNothing;
        --wanted_edges_;
        if (!(*oe)->is_phony())
          --command_edges_;
      }
    }
  }
  return true;
}

bool Plan::DyndepsLoaded(DependencyScan* scan, Node* node,
                         const DyndepFile& ddf, string* err) {
  // Recompute the dirty state of all our direct and indirect dependents now
  // that our dyndep information has been loaded.
  if (!RefreshDyndepDependents(scan, node, err))
    return false;

  // We loaded dyndep information for those out_edges of the dyndep node that
  // specify the node in a dyndep binding, but they may not be in the plan.
  // Starting with those already in the plan, walk newly-reachable portion
  // of the graph through the dyndep-discovered dependencies.

  // Find edges in the the build plan for which we have new dyndep info.
  std::vector<DyndepFile::const_iterator> dyndep_roots;
  for (DyndepFile::const_iterator oe = ddf.begin(); oe != ddf.end(); ++oe) {
    Edge* edge = oe->first;

    // If the edge outputs are ready we do not need to consider it here.
    if (edge->outputs_ready())
      continue;

    map<Edge*, Want>::iterator want_e = want_.find(edge);

    // If the edge has not been encountered before then nothing already in the
    // plan depends on it so we do not need to consider the edge yet either.
    if (want_e == want_.end())
      continue;

    // This edge is already in the plan so queue it for the walk.
    dyndep_roots.push_back(oe);
  }

  // Walk dyndep-discovered portion of the graph to add it to the build plan.
  std::set<Edge*> dyndep_walk;
  for (std::vector<DyndepFile::const_iterator>::iterator
       oei = dyndep_roots.begin(); oei != dyndep_roots.end(); ++oei) {
    DyndepFile::const_iterator oe = *oei;
    for (vector<Node*>::const_iterator i = oe->second.implicit_inputs_.begin();
         i != oe->second.implicit_inputs_.end(); ++i) {
      if (!AddSubTarget(*i, oe->first->outputs_[0], err, &dyndep_walk) &&
          !err->empty())
        return false;
    }
  }

  // Add out edges from this node that are in the plan (just as
  // Plan::NodeFinished would have without taking the dyndep code path).
  const std::vector<Edge*> out_edges = node->GetOutEdges();
  for (vector<Edge*>::const_iterator oe = out_edges.begin();
       oe != out_edges.end(); ++oe) {
    map<Edge*, Want>::iterator want_e = want_.find(*oe);
    if (want_e == want_.end())
      continue;
    dyndep_walk.insert(want_e->first);
  }

  // See if any encountered edges are now ready.
  for (set<Edge*>::iterator wi = dyndep_walk.begin();
       wi != dyndep_walk.end(); ++wi) {
    map<Edge*, Want>::iterator want_e = want_.find(*wi);
    if (want_e == want_.end())
      continue;
    if (!EdgeMaybeReady(want_e, err))
      return false;
  }

  return true;
}

bool Plan::RefreshDyndepDependents(DependencyScan* scan, Node* node,
                                   string* err) {
  // Collect the transitive closure of dependents and mark their edges
  // as not yet visited by RecomputeDirty.
  set<Node*> dependents;
  UnmarkDependents(node, &dependents);

  // Update the dirty state of all dependents and check if their edges
  // have become wanted.
  for (set<Node*>::iterator i = dependents.begin();
       i != dependents.end(); ++i) {
    Node* n = *i;

    // Check if this dependent node is now dirty.  Also checks for new cycles.
    std::vector<Node*> validation_nodes;
    if (!scan->RecomputeDirty(n, &validation_nodes, err))
      return false;

    // Add any validation nodes found during RecomputeDirty as new top level
    // targets.
    for (std::vector<Node*>::iterator v = validation_nodes.begin();
         v != validation_nodes.end(); ++v) {
      if (Edge* in_edge = (*v)->in_edge()) {
        if (!in_edge->outputs_ready() &&
            !AddTarget(*v, err)) {
          return false;
        }
      }
    }
    if (!n->dirty())
      continue;

    // This edge was encountered before.  However, we may not have wanted to
    // build it if the outputs were not known to be dirty.  With dyndep
    // information an output is now known to be dirty, so we want the edge.
    Edge* edge = n->in_edge();
    assert(edge && !edge->outputs_ready());
    map<Edge*, Want>::iterator want_e = want_.find(edge);
    assert(want_e != want_.end());
    if (want_e->second == kWantNothing) {
      want_e->second = kWantToStart;
      EdgeWanted(edge);
    }
  }
  return true;
}

void Plan::UnmarkDependents(Node* node, set<Node*>* dependents) {
  const std::vector<Edge*> out_edges = node->GetOutEdges();
  for (vector<Edge*>::const_iterator oe = out_edges.begin();
       oe != out_edges.end(); ++oe) {
    Edge* edge = *oe;

    map<Edge*, Want>::iterator want_e = want_.find(edge);
    if (want_e == want_.end())
      continue;

    if (edge->mark_ != Edge::VisitNone) {
      edge->mark_ = Edge::VisitNone;
      for (vector<Node*>::iterator o = edge->outputs_.begin();
           o != edge->outputs_.end(); ++o) {
        if (dependents->insert(*o).second)
          UnmarkDependents(*o, dependents);
      }
    }
  }
}

void Plan::Dump() {
  printf("pending: %d\n", (int)want_.size());
  for (map<Edge*, Want>::iterator e = want_.begin(); e != want_.end(); ++e) {
    if (e->second != kWantNothing)
      printf("want ");
    e->first->Dump();
  }
  printf("ready: %d\n", (int)ready_.size());
}

struct RealCommandRunner : public CommandRunner {
  explicit RealCommandRunner(const BuildConfig& config) : config_(config) {}
  virtual ~RealCommandRunner() {}
  virtual bool CanRunMore();
  virtual bool StartCommand(Edge* edge);
  virtual bool WaitForCommand(Result* result);
  virtual vector<Edge*> GetActiveEdges();
  virtual void Abort();

  const BuildConfig& config_;
  SubprocessSet subprocs_;
  map<Subprocess*, Edge*> subproc_to_edge_;
};

vector<Edge*> RealCommandRunner::GetActiveEdges() {
  vector<Edge*> edges;
  for (map<Subprocess*, Edge*>::iterator e = subproc_to_edge_.begin();
       e != subproc_to_edge_.end(); ++e)
    edges.push_back(e->second);
  return edges;
}

void RealCommandRunner::Abort() {
  subprocs_.Clear();
}

bool RealCommandRunner::CanRunMore() {
  size_t subproc_number =
      subprocs_.running_.size() + subprocs_.finished_.size();
  return (int)subproc_number < config_.parallelism
    && ((subprocs_.running_.empty() || config_.max_load_average <= 0.0f)
        || GetLoadAverage() < config_.max_load_average);
}

bool RealCommandRunner::StartCommand(Edge* edge) {
  EdgeCommand c;
  edge->EvaluateCommand(&c);
  Subprocess* subproc = subprocs_.Add(c);
  if (!subproc)
    return false;
  subproc_to_edge_.insert(make_pair(subproc, edge));

  return true;
}

bool RealCommandRunner::WaitForCommand(Result* result) {
  Subprocess* subproc;
  while ((subproc = subprocs_.NextFinished()) == NULL) {
    bool interrupted = subprocs_.DoWork();
    if (interrupted)
      return false;
  }

  result->status = subproc->Finish();
#ifndef _WIN32
  result->rusage = *subproc->GetUsage();
#endif
  result->output = subproc->GetOutput();

  map<Subprocess*, Edge*>::iterator e = subproc_to_edge_.find(subproc);
  result->edge = e->second;
  subproc_to_edge_.erase(e);

  delete subproc;
  return true;
}

Builder::Builder(State* state, const BuildConfig& config,
                 BuildLog* build_log, DepsLog* deps_log,
                 DiskInterface* disk_interface, Status* status,
                 int64_t start_time_millis)
    : state_(state), config_(config), plan_(this), status_(status),
      start_time_millis_(start_time_millis), disk_interface_(disk_interface),
      scan_(state, build_log, deps_log, disk_interface,
            &config_.depfile_parser_options, config.uses_phony_outputs) {
}

Builder::~Builder() {
  Cleanup();
}

void Builder::Cleanup() {
  if (command_runner_.get()) {
    vector<Edge*> active_edges = command_runner_->GetActiveEdges();
    command_runner_->Abort();

    for (vector<Edge*>::iterator e = active_edges.begin();
         e != active_edges.end(); ++e) {
      if ((*e)->IsPhonyOutput())
        continue;
      string depfile = (*e)->GetUnescapedDepfile();
      for (vector<Node*>::iterator o = (*e)->outputs_.begin();
           o != (*e)->outputs_.end(); ++o) {
        // Only delete this output if it was actually modified.  This is
        // important for things like the generator where we don't want to
        // delete the manifest file if we can avoid it.  But if the rule
        // uses a depfile, always delete.  (Consider the case where we
        // need to rebuild an output because of a modified header file
        // mentioned in a depfile, and the command touches its depfile
        // but is interrupted before it touches its output file.)
        string err;
        bool is_dir = false;
        const string pathStr = (*o)->globalPath().h.data();
        TimeStamp new_mtime =
            disk_interface_->LStat(pathStr, &is_dir, nullptr, &err);
        if (new_mtime == -1)  // Log and ignore LStat() errors.
          status_->Error("%s", err.c_str());
        if (!is_dir && (!depfile.empty() || (*o)->mtime() != new_mtime))
          disk_interface_->RemoveFile(pathStr);
      }
      if (!depfile.empty())
        // depfile is relative, disk_interface_ uses global paths.
        disk_interface_->RemoveFile(
            (*e)->pos_.scope()->GlobalPath(depfile).h.data());
    }
  }
}

Node* Builder::AddTarget(const string& name, string* err) {
  Node* node = state_->LookupNode(state_->root_scope_.GlobalPath(name));
  if (!node) {
    *err = "unknown target: '" + name + "'";
    return NULL;
  }
  if (!AddTargets({ node }, err))
    return NULL;
  return node;
}

void Builder::RefreshPriority(const std::vector<Node*>& start_nodes) {
  METRIC_RECORD("RefreshPriority");
  WeightDataSource data_source;
  if (config_.weight_list_path) {
    std::ifstream file(config_.weight_list_path.value());
    std::string line;
    while (std::getline(file, line)) {
      if (auto pos = line.find_first_of(','); pos != string::npos) {
        auto path = line.substr(0, pos);
        auto raw_weight = line.substr(pos + 1);
        char* idx;
        int64_t weight = std::strtoll(raw_weight.c_str(), &idx, 10);
        if (idx != nullptr) {
          data_source.Put(std::move(path), weight);
        }
      }
    }
  }
  std::vector<std::pair<Edge*, int64_t>> todos;
  std::unique_ptr<ThreadPool> thread_pool = CreateThreadPool();
  for (auto* node: start_nodes) {
    if (node && node->in_edge()) {
      todos.emplace_back(std::make_pair(node->in_edge(), 0));
    }
  }

  // Get weight from data source or ninja log, it isn't related to Edge.weight,
  // because Edge.weight is used for task distribution across pools which we don't
  // want to do that in this context.
  auto weight_getter = [&data_source, this](Edge* edge) -> int64_t {
    if (!edge || edge->outputs_ready()) {
      return 0;
    }
    if (edge->is_phony()) {
      return 1;
    }
    if (config_.weight_list_path) {
      return data_source.Get(edge->outputs_[0]->globalPath().h).value_or(1);
    } else if (config_.ninja_log_as_weight_list) {
      if (scan_.build_log()) {
        auto* entry = scan_.build_log()->LookupByOutput(edge->outputs_[0]->globalPath());
        if (entry) {
          return entry->end_time - entry->start_time + 1;
        }
      }
      return 1;
    } else {
      Fatal("either weight_list_path or ninja_log_as_weight_list should be set.");
    }
  };
  while (!todos.empty()) {
    // Traverse edges in BFS manner, and update each edge's critical path based priority from
    // accumulated weight.
    const auto& result = ParallelMap(thread_pool.get(), todos, [&weight_getter] (auto& p) {
      // the pair is composed of a visiting edge and accumulated critical path based priority.
      auto* e = p.first;
      auto acc = p.second;

      if (!e) {
        return std::unordered_map<Edge*, int64_t>();
      }
      auto run = weight_getter(e);
      auto new_priority = run + acc;
      // Skip if priority isn't updated
      if (new_priority <= e->priority()) {
        return std::unordered_map<Edge*, int64_t>();
      }
      e->priority_ = new_priority;

      std::set<Edge*, EdgeCmp> next_edges;
      for (auto* next_node : e->inputs_) {
        // Skip if the next node isn't dirty because actual build also skips the node.
        if (!next_node || !next_node->dirty()) {
          continue;
        }
        auto* next_e = next_node->in_edge();
        if (next_e) {
          next_edges.insert(next_e);
        }
      }

      std::unordered_map<Edge*, int64_t> next_todo_map;
      for (auto* ne : next_edges) {
        auto next_run = weight_getter(ne);
        if (next_run + e->priority() > ne->priority()) {
          next_todo_map.try_emplace(ne, e->priority());
        }
      }
      return next_todo_map;
    });
    todos.clear();

    std::unordered_map<Edge*, int64_t> next_todo_map_total;
    for (const auto& todo_map : result) {
      for (const auto& [k, v] : todo_map) {
        auto [it, inserted] = next_todo_map_total.try_emplace(k, v);
          if (!inserted) {
            it->second = std::max(it->second, v);
          }
      }
    }
    for (const auto& [key, value] : next_todo_map_total) {
      todos.emplace_back(key, value);
    }
  }
}

bool Builder::AddTargets(const std::vector<Node*> &nodes, string* err) {
  std::vector<Node*> validation_nodes;
  if (!scan_.RecomputeNodesDirty(nodes, &validation_nodes, err))
    return false;

  if (config_.weight_list_path || config_.ninja_log_as_weight_list) {
    RefreshPriority(nodes);
  }

  for (Node* node : nodes) {
    std::string plan_err;
    if (!plan_.AddTarget(node, &plan_err)) {
      if (!plan_err.empty()) {
        *err = plan_err;
        return false;
      } else {
        // Added a target that is already up-to-date; not really
        // an error.
      }
    }
  }

  for (Node* node : validation_nodes) {
    std::string plan_err;
    if (!plan_.AddTarget(node, &plan_err)) {
      if (!plan_err.empty()) {
        *err = plan_err;
        return false;
      } else {
        // Added a target that is already up-to-date; not really
        // an error.
      }
    }
  }

  return true;
}

bool Builder::AlreadyUpToDate() const {
  return !plan_.more_to_do();
}

bool Builder::Build(string* err) {
  assert(!AlreadyUpToDate());

  status_->PlanHasTotalEdges(plan_.command_edge_count());
  int pending_commands = 0;
  int failures_allowed = config_.failures_allowed;

  // Set up the command runner if we haven't done so already.
  if (!command_runner_.get()) {
    if (config_.dry_run)
      command_runner_.reset(new DryRunCommandRunner);
    else
      command_runner_.reset(new RealCommandRunner(config_));
  }

  // We are about to start the build process.
  status_->BuildStarted();

  // This main loop runs the entire build process.
  // It is structured like this:
  // First, we attempt to start as many commands as allowed by the
  // command runner.
  // Second, we attempt to wait for / reap the next finished command.
  while (plan_.more_to_do()) {
    // See if we can start any more commands.
    if (failures_allowed && command_runner_->CanRunMore()) {
      if (Edge* edge = plan_.FindWork()) {
        if (!StartEdge(edge, err)) {
          Cleanup();
          status_->BuildFinished();
          return false;
        }

        if (edge->is_phony()) {
          if (!plan_.EdgeFinished(edge, Plan::kEdgeSucceeded, err)) {
            Cleanup();
            status_->BuildFinished();
            return false;
          }
        } else {
          ++pending_commands;
        }

        // We made some progress; go back to the main loop.
        continue;
      }
    }

    // See if we can reap any finished commands.
    if (pending_commands) {
      CommandRunner::Result result;
      if (!command_runner_->WaitForCommand(&result) ||
          result.status == ExitInterrupted) {
        Cleanup();
        status_->BuildFinished();
        *err = "interrupted by user";
        return false;
      }

      --pending_commands;
      if (!FinishCommand(&result, err)) {
        Cleanup();
        status_->BuildFinished();
        return false;
      }

      if (!result.success()) {
        if (failures_allowed)
          failures_allowed--;
      }

      // We made some progress; start the main loop over.
      continue;
    }

    // If we get here, we cannot make any more progress.
    status_->BuildFinished();
    if (failures_allowed == 0) {
      if (config_.failures_allowed > 1)
        *err = "subcommands failed";
      else
        *err = "subcommand failed";
    } else if (failures_allowed < config_.failures_allowed)
      *err = "cannot make progress due to previous errors";
    else
      *err = "stuck [this is a bug]";

    return false;
  }

  status_->BuildFinished();
  return true;
}

bool Builder::StartEdge(Edge* edge, string* err) {
  METRIC_RECORD("StartEdge");
  if (edge->is_phony())
    return true;

  int64_t start_time_millis = GetTimeMillis() - start_time_millis_;
  running_edges_.insert(make_pair(edge, start_time_millis));

  status_->BuildEdgeStarted(edge, start_time_millis);

  if (!edge->IsPhonyOutput()) {
    for (vector<Node*>::iterator o = edge->outputs_.begin();
         o != edge->outputs_.end(); ++o) {
      // Create directories necessary for outputs.
      // XXX: this will block; do we care?
      if (!disk_interface_->MakeDirs(
          (*o)->globalPath().h.data()))
        return false;

      if (!(*o)->exists())
        continue;

      // Remove existing outputs for non-restat rules.
      // XXX: this will block; do we care?
      if (config_.pre_remove_output_files && !edge->IsRestat() && !config_.dry_run) {
        if (disk_interface_->RemoveFile(
            (*o)->globalPath().h.data()) < 0)
          return false;
      }
    }
  }

  // Create response file, if needed
  // XXX: this may also block; do we care?
  string rspfile = edge->GetUnescapedRspfile();
  if (!rspfile.empty()) {
    string content = edge->GetBinding("rspfile_content");
    if (!disk_interface_->WriteFile(
        // rspfile is relative, disk_interface_ uses global paths.
        edge->pos_.scope()->GlobalPath(rspfile).h.data(),
        content))
      return false;
  }

  // start command computing and run it
  if (!command_runner_->StartCommand(edge)) {
    EdgeCommand c;
    edge->EvaluateCommand(&c);
    err->assign("command '" + c.command + "' failed.");
    return false;
  }

  return true;
}

bool Builder::FinishCommand(CommandRunner::Result* result, string* err) {
  METRIC_RECORD("FinishCommand");

  Edge* edge = result->edge;
  bool phony_output = edge->IsPhonyOutput();

  vector<Node*> deps_nodes;
  string deps_type = edge->GetBinding("deps");
  if (!phony_output) {
    // First try to extract dependencies from the result, if any.
    // This must happen first as it filters the command output (we want
    // to filter /showIncludes output, even on compile failure) and
    // extraction itself can fail, which makes the command fail from a
    // build perspective.
    const string deps_prefix = edge->GetBinding("msvc_deps_prefix");
    if (!deps_type.empty()) {
      string extract_err;
      if (!ExtractDeps(result, deps_type, deps_prefix, &deps_nodes,
                       &extract_err) &&
          result->success()) {
        if (!result->output.empty())
          result->output.append("\n");
        result->output.append(extract_err);
        result->status = ExitFailure;
      }
    }
  }

  int64_t start_time_millis, end_time_millis;
  RunningEdgeMap::iterator i = running_edges_.find(edge);
  start_time_millis = i->second;
  end_time_millis = GetTimeMillis() - start_time_millis_;
  running_edges_.erase(i);

  // Restat the edge outputs
  TimeStamp output_mtime = 0;
  if (result->success() && !config_.dry_run && !phony_output) {
    bool restat = edge->IsRestat();
    vector<Node*> nodes_cleaned;

    TimeStamp newest_input = 0;
    Node* newest_input_node = nullptr;
    for (vector<Node*>::iterator i = edge->inputs_.begin();
         i != edge->inputs_.end() - edge->order_only_deps_; ++i) {
      TimeStamp input_mtime = (*i)->mtime();
      if (input_mtime == -1)
        return false;
      if (input_mtime > newest_input) {
        newest_input = input_mtime;
        newest_input_node = (*i);
      }
    }

    set<string> declared_symlinks;
    if (config_.uses_symlink_outputs) {
      string symlink_outputs = edge->GetSymlinkOutputs();
      if (symlink_outputs.length() > 0) {
        stringstream ss(symlink_outputs);
        string path;
        /// Naively split symlink_outputs path by the empty ' ' space character.
        /// because the '$ ' escape doesn't exist at this stage. In experimentation
        /// and practice across a number of AOSP configurations, this is OK.
        ///
        /// We could modify the GetBindingImpl/GetSymlinkOutputs API to support lists,
        /// but it'd be an invasive change that'll require a little bit more designing.
        /// For example, how do we expand "${out}.d" if ${out} is a list?
        ///
        /// That said, keep in mind that this is a simple string split that could
        /// fail with paths containing spaces.
        while (getline(ss, path, ' ')) {
          uint64_t slash_bits;
          if (!CanonicalizePath(&path, &slash_bits, err)) {
            return false;
          }
          declared_symlinks.insert(move(path));
        }
      }
    }

    for (vector<Node*>::iterator o = edge->outputs_.begin();
         o != edge->outputs_.end(); ++o) {
      bool is_dir = false;
      bool is_symlink = false;
      TimeStamp old_mtime = (*o)->mtime();
      if (!(*o)->LStat(disk_interface_, &is_dir, &is_symlink, err))
        return false;

      TimeStamp new_mtime = (*o)->mtime();

      if (config_.uses_symlink_outputs) {
        /// Warn or error if created symlinks aren't declared in symlink_outputs,
        /// or if created files are declared in symlink_outputs.
        if (is_symlink) {
          if (declared_symlinks.find((*o)->path()) == declared_symlinks.end()) {
            // Not in declared_symlinks
            if (!result->output.empty())
              result->output.append("\n");
            result->output.append("ninja: " + (*o)->path() + " is a symlink, but it was not declared in symlink_outputs");
            if (config_.undeclared_symlink_outputs_should_err) {
              result->status = ExitFailure;
            }
          } else {
            declared_symlinks.erase((*o)->path());
          }
        } else if (!is_symlink && declared_symlinks.find((*o)->path()) != declared_symlinks.end()) {
          if (!result->output.empty())
            result->output.append("\n");
          result->output.append("ninja: " + (*o)->path() + " is not a symlink, but it was declared in symlink_outputs");
          declared_symlinks.erase((*o)->path());
          if (config_.undeclared_symlink_outputs_should_err) {
            result->status = ExitFailure;
          }
        }
      }

      if (config_.uses_phony_outputs) {
        if (new_mtime == 0) {
          if (!result->output.empty())
            result->output.append("\n");
          result->output.append("ninja: output file missing after successful execution: ");
          result->output.append((*o)->path());
          if (config_.missing_output_file_should_err) {
            result->status = ExitFailure;
          }
        } else if (!restat && new_mtime < newest_input) {
          if (!result->output.empty())
            result->output.append("\n");
          result->output.append("ninja: Missing `restat`? An output file is older than the most recent input:\n output: ");
          result->output.append((*o)->path());
          result->output.append("\n  input: ");
          result->output.append(newest_input_node->path());
          if (config_.old_output_should_err) {
            result->status = ExitFailure;
          }
        }
        if (is_dir) {
          if (!result->output.empty())
            result->output.append("\n");
          result->output.append("ninja: outputs should be files, not directories: ");
          result->output.append((*o)->path());
          if (config_.output_directory_should_err) {
            result->status = ExitFailure;
          }
        }
      }
      if (new_mtime > output_mtime)
        output_mtime = new_mtime;
      if (old_mtime == new_mtime && restat) {
        nodes_cleaned.push_back(*o);
        continue;
      }
    }

    /// Ensure that declared_symlinks is empty after verifying that symlink outputs
    /// were declared in the edge. A non-empty declared_symlinks set indicates that
    /// not all declared symlinks were created by the edge itself (over-specification).
    if (config_.uses_symlink_outputs && declared_symlinks.size() > 0) {
      string missing_outputs;
      for (string symlink : declared_symlinks) {
        missing_outputs = missing_outputs + " " + symlink;
      }
      result->output.append(
        "ninja: not all symlink_outputs were created for this edge:" + missing_outputs);
      if (config_.undeclared_symlink_outputs_should_err) {
        result->status = ExitFailure;
      }
    }

    status_->BuildEdgeFinished(edge, end_time_millis, result);

    if (result->success() && !nodes_cleaned.empty()) {
      for (vector<Node*>::iterator o = nodes_cleaned.begin();
           o != nodes_cleaned.end(); ++o) {
        // The rule command did not change the output.  Propagate the clean
        // state through the build graph.
        // Note that this also applies to nonexistent outputs (mtime == 0).
        if (!plan_.CleanNode(&scan_, *o, err))
          return false;
      }

      // If any output was cleaned, find the most recent mtime of any
      // (existing) non-order-only input or the depfile.
      TimeStamp restat_mtime = newest_input;

      string depfile = edge->GetUnescapedDepfile();
      if (restat_mtime != 0 && deps_type.empty() && !depfile.empty()) {
        TimeStamp depfile_mtime = disk_interface_->Stat(
            // depfile is relative, disk_interface_ uses global paths.
            edge->pos_.scope()->GlobalPath(depfile).h.data(),
            err);
        if (depfile_mtime == -1)
          return false;
        if (depfile_mtime > restat_mtime)
          restat_mtime = depfile_mtime;
      }

      // The total number of edges in the plan may have changed as a result
      // of a restat.
      status_->PlanHasTotalEdges(plan_.command_edge_count());

      output_mtime = restat_mtime;
    }
  } else {
    status_->BuildEdgeFinished(edge, end_time_millis, result);
  }

  if (!plan_.EdgeFinished(edge, result->success() ? Plan::kEdgeSucceeded : Plan::kEdgeFailed, err))
    return false;

  // The rest of this function only applies to successful commands.
  if (!result->success()) {
    return true;
  }

  // Delete any left over response file.
  string rspfile = edge->GetUnescapedRspfile();
  if (!rspfile.empty() && !g_keep_rsp)
    // rspfile is relative, disk_interface_ uses global paths.
    disk_interface_->RemoveFile(
        edge->pos_.scope()->GlobalPath(rspfile).h.data());

  if (scan_.build_log() && !phony_output) {
    if (!scan_.build_log()->RecordCommand(edge, start_time_millis,
                                          end_time_millis, output_mtime)) {
      *err = string("Error writing to build log: ") + strerror(errno);
      return false;
    }
  }

  if (!deps_type.empty() && !config_.dry_run && !phony_output) {
    Node* out = edge->outputs_[0];
    TimeStamp deps_mtime = disk_interface_->LStat(out->globalPath().h.data(), nullptr, nullptr, err);
    if (deps_mtime == -1)
      return false;
    if (!scan_.deps_log()->RecordDeps(out, deps_mtime, deps_nodes)) {
      *err = string("Error writing to deps log: ") + strerror(errno);
      return false;
    }
  }
  return true;
}

bool Builder::ExtractDeps(CommandRunner::Result* result,
                          const string& deps_type,
                          const string& deps_prefix,
                          vector<Node*>* deps_nodes,
                          string* err) {
  if (deps_type == "msvc") {
    CLParser parser;
    string output;
    if (!parser.Parse(result->output, deps_prefix, &output, err))
      return false;
    result->output = output;
    for (set<string>::iterator i = parser.includes_.begin();
         i != parser.includes_.end(); ++i) {
      // ~0 is assuming that with MSVC-parsed headers, it's ok to always make
      // all backslashes (as some of the slashes will certainly be backslashes
      // anyway). This could be fixed if necessary with some additional
      // complexity in IncludesNormalize::Relativize.
      deps_nodes->push_back(
          state_->GetNode(result->edge->pos_.scope()->GlobalPath(*i), ~0u));
    }
  } else
  if (deps_type == "gcc") {
    string depfile = result->edge->GetUnescapedDepfile();
    if (depfile.empty()) {
      *err = string("edge with deps=gcc but no depfile makes no sense");
      return false;
    }

    // Read depfile content.  Treat a missing depfile as empty.
    string content;
    switch (disk_interface_->ReadFile(depfile, &content, err)) {
    case DiskInterface::Okay:
      break;
    case DiskInterface::NotFound:
      err->clear();
      // We only care if the depfile is missing when the tool succeeded.
      if (!config_.dry_run && result->status == ExitSuccess) {
        if (config_.missing_depfile_should_err) {
          *err = string("depfile is missing");
          return false;
        } else {
          status_->Warning("depfile is missing (%s for %s)", depfile.c_str(), result->edge->outputs_[0]->path().c_str());
        }
      }
      break;
    case DiskInterface::OtherError:
      return false;
    }
    if (content.empty())
      return true;

    DepfileParser deps(config_.depfile_parser_options);
    string warning;
    if (!deps.Parse(&content, &warning, err))
      return false;

    if (!warning.empty()) {
      if (!result->output.empty()) {
        result->output.append("\n");
      }
      result->output.append("ninja: warning: ");
      result->output.append(warning);
    }

    // XXX check depfile matches expected output.
    deps_nodes->reserve(deps.ins_.size());
    for (vector<StringPiece>::iterator i = deps.ins_.begin();
         i != deps.ins_.end(); ++i) {
      uint64_t slash_bits;
      if (!CanonicalizePath(const_cast<char*>(i->str_), &i->len_, &slash_bits,
                            err))
        return false;
      deps_nodes->push_back(
          state_->GetNode(result->edge->pos_.scope()->GlobalPath(*i),
          slash_bits));
    }

    if (!g_keep_depfile) {
      if (disk_interface_->RemoveFile(depfile) < 0) {
        *err = string("deleting depfile: ") + strerror(errno) + string("\n");
        return false;
      }
    }
  } else {
    Fatal("unknown deps type '%s'", deps_type.c_str());
  }

  return true;
}

bool Builder::LoadDyndeps(Node* node, string* err) {
  status_->BuildLoadDyndeps();

  // Load the dyndep information provided by this node.
  DyndepFile ddf;
  if (!scan_.LoadDyndeps(node, &ddf, err))
    return false;

  // Update the build plan to account for dyndep modifications to the graph.
  if (!plan_.DyndepsLoaded(&scan_, node, ddf, err))
    return false;

  // New command edges may have been added to the plan.
  status_->PlanHasTotalEdges(plan_.command_edge_count());

  return true;
}
