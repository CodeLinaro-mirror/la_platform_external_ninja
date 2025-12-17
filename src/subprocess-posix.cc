// Copyright 2012 Google Inc. All Rights Reserved.
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

#include <sys/select.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <spawn.h>
#include <filesystem>
#include <iostream>
#include <system_error>

extern char** environ;

#include "google/protobuf/text_format.h"
#include "google/protobuf/io/zero_copy_stream_impl.h"

#include "build.h"
// nsjail config
#include "config.pb.h"
#include "graph.h"
#include "subprocess.h"
#include "util.h"

Subprocess::Subprocess(bool use_console) : fd_(-1), pid_(-1),
                                           use_console_(use_console) {
}

Subprocess::~Subprocess() {
  if (fd_ >= 0)
    close(fd_);
  // Reap child if forgotten.
  if (pid_ != -1)
    Finish();
}

std::filesystem::path Subprocess::OutPathToNsjailOutPath(const std::string& out) {
    const std::string& build_dir = config_->build_dir;
    if (build_dir.empty()) {
      Fatal("builddir must be set when using nsjail sandboxing");
    }
    if (out.rfind(build_dir, 0) != 0) {
      Fatal("All outputs must be in %s, but %s wasn't", build_dir.c_str(), out.c_str());
    }

    auto rel = out.substr(build_dir.size());
    return nsjail_workdir_.value().path() / "out" / rel;
}

bool Subprocess::Start(SubprocessSet* set, const EdgeCommand& cmd, Edge* edge,
                       int extra_fd) {
  edge_ = edge;
  config_ = &set->config_;

  int output_pipe[2];
  if (pipe(output_pipe) < 0)
    Fatal("pipe: %s", strerror(errno));
  fd_ = output_pipe[0];
#if !defined(USE_PPOLL)
  // If available, we use ppoll in DoWork(); otherwise we use pselect
  // and so must avoid overly-large FDs.
  if (fd_ >= static_cast<int>(FD_SETSIZE))
    Fatal("pipe: %s", strerror(EMFILE));
#endif  // !USE_PPOLL
  SetCloseOnExec(fd_);

  posix_spawn_file_actions_t action;
  int err = posix_spawn_file_actions_init(&action);
  if (err != 0)
    Fatal("posix_spawn_file_actions_init: %s", strerror(err));

  err = posix_spawn_file_actions_addclose(&action, output_pipe[0]);
  if (err != 0)
    Fatal("posix_spawn_file_actions_addclose: %s", strerror(err));

  if (extra_fd >= 0) {
    if (posix_spawn_file_actions_adddup2(&action, extra_fd, 3) != 0)
      Fatal("posix_spawn_file_actions_adddup2: %s", strerror(errno));
  }

  posix_spawnattr_t attr;
  err = posix_spawnattr_init(&attr);
  if (err != 0)
    Fatal("posix_spawnattr_init: %s", strerror(err));

  short flags = 0;

  flags |= POSIX_SPAWN_SETSIGMASK;
  err = posix_spawnattr_setsigmask(&attr, &set->old_mask_);
  if (err != 0)
    Fatal("posix_spawnattr_setsigmask: %s", strerror(err));
  // Signals which are set to be caught in the calling process image are set to
  // default action in the new process image, so no explicit
  // POSIX_SPAWN_SETSIGDEF parameter is needed.

  if (!use_console_) {
    // Put the child in its own process group, so ctrl-c won't reach it.
    flags |= POSIX_SPAWN_SETPGROUP;
    // No need to posix_spawnattr_setpgroup(&attr, 0), it's the default.

    // Open /dev/null over stdin.
    err = posix_spawn_file_actions_addopen(&action, 0, "/dev/null", O_RDONLY,
          0);
    if (err != 0) {
      Fatal("posix_spawn_file_actions_addopen: %s", strerror(err));
    }

    err = posix_spawn_file_actions_adddup2(&action, output_pipe[1], 1);
    if (err != 0)
      Fatal("posix_spawn_file_actions_adddup2: %s", strerror(err));
    err = posix_spawn_file_actions_adddup2(&action, output_pipe[1], 2);
    if (err != 0)
      Fatal("posix_spawn_file_actions_adddup2: %s", strerror(err));
    err = posix_spawn_file_actions_addclose(&action, output_pipe[1]);
    if (err != 0)
      Fatal("posix_spawn_file_actions_addclose: %s", strerror(err));
    // In the console case, output_pipe is still inherited by the child and
    // closed when the subprocess finishes, which then notifies ninja.
  }
#ifdef POSIX_SPAWN_USEVFORK
  flags |= POSIX_SPAWN_USEVFORK;
#endif

  err = posix_spawnattr_setflags(&attr, flags);
  if (err != 0)
    Fatal("posix_spawnattr_setflags: %s", strerror(err));

  std::vector<const char*> args;
  std::vector<std::string> buff;

  bool use_nsjail = !set->config_.nsjail_path.empty() &&
                    cmd.sandbox != EdgeSandbox::NONE &&
                    edge != nullptr &&
                    edge->GetBinding("sandbox_disabled").empty();

  if (use_nsjail) {
    for (Node* output : edge->outputs_) {
      if (output->path().rfind(config_->build_dir, 0) != 0) {
        use_nsjail = false;
        break;
      }
    }
  }

  if (!use_nsjail) {
    args.push_back("/bin/sh");
    args.push_back("-c");
    args.push_back(cmd.command.c_str());
  } else {
    std::error_code ec;
    nsjail_workdir_ = TempDir::createInDir(set->config_.nsjail_workdir, ec);
    if (ec) {
      Fatal("Failed to create temporary directory: %s", ec.message().c_str());
    }
    for (Node* output : edge->outputs_) {
      auto dir = OutPathToNsjailOutPath(output->path()).parent_path();
      std::filesystem::create_directories(dir, ec);
      if (ec) {
        Fatal("Failed to create temporary directory: %s", ec.message().c_str());
      }
    }
    args.push_back(set->config_.nsjail_path.c_str());
    nsjail::NsJailConfig nsjailConfig;
    // equivalent to -q, for quiet execution
    nsjailConfig.set_log_level(nsjail::WARNING);
    nsjailConfig.set_disable_rl(true);
    nsjailConfig.set_cwd("/src");

    // TODO: Better environment variable sandboxing
    nsjailConfig.set_keep_env(true);

    // TODO: Make the globally included directories like this customizable, and eventually phase
    // them out.
    auto binMount = nsjailConfig.add_mount();
    binMount->set_src("/bin");
    binMount->set_dst("/bin");
    binMount->set_is_bind(true);
    binMount->set_is_dir(true);
    auto libMount = nsjailConfig.add_mount();
    libMount->set_src("/lib");
    libMount->set_dst("/lib");
    libMount->set_is_bind(true);
    binMount->set_is_dir(true);
    auto lib64Mount = nsjailConfig.add_mount();
    lib64Mount->set_src("/lib64");
    lib64Mount->set_dst("/lib64");
    lib64Mount->set_is_bind(true);
    lib64Mount->set_is_dir(true);
    auto usrMount = nsjailConfig.add_mount();
    usrMount->set_src("/usr");
    usrMount->set_dst("/usr");
    usrMount->set_is_bind(true);
    usrMount->set_is_dir(true);
    auto devMount = nsjailConfig.add_mount();
    devMount->set_src("/dev");
    devMount->set_dst("/dev");
    devMount->set_is_bind(true);
    devMount->set_is_dir(true);

    nsjailConfig.set_mount_proc(true);


    // Add a tmp directory. Using a directory in the working directory instead of a tmpfs mount
    // so that we don't have to worry about how big of a tmpfs to make.
    auto temp_dir = nsjail_workdir_.value().path() / "tmp";
    std::filesystem::create_directory(temp_dir);
    auto tmpMount = nsjailConfig.add_mount();
    tmpMount->set_src(temp_dir.generic_string());
    tmpMount->set_dst("/tmp");
    tmpMount->set_is_bind(true);
    tmpMount->set_is_dir(true);
    tmpMount->set_rw(true);

    // Add the source directory. Normally this is not necessary, because the -R flags for the
    // input files would cause nsjail to create it. But in cases where the action has no inputs
    // or all the inputs are from an absolute out/ directory, it won't be created by nsjail
    // automatically and the --cwd /src flag will fail.
    auto src_dir = nsjail_workdir_.value().path() / "src";
    std::filesystem::create_directory(src_dir);
    auto srcMount = nsjailConfig.add_mount();
    srcMount->set_src(src_dir.generic_string());
    srcMount->set_dst("/src");
    srcMount->set_is_dir(true);
    srcMount->set_is_bind(true);


    // Add the out directory. It needs to be in a location that still matches all the
    // output paths in the ninja file, so that we don't need to rewrite those paths. So if
    // the out directory is at an absolute path, keep it in the same location. If it's at a
    // relative path, move it to be relative to /src/.
    const std::string& out_dir = set->config_.build_dir;
    if (out_dir.empty()) {
      Fatal("builddir must be set when using nsjail sandboxing");
    }
    auto absolute_out_dir = nsjail_workdir_.value().path() / "out";
    std::filesystem::create_directory(absolute_out_dir);
    auto absolute_out_dir_in_sandbox = out_dir;
    if (out_dir.rfind("/", 0) != 0) {
      absolute_out_dir_in_sandbox = std::filesystem::path("/src") / absolute_out_dir_in_sandbox;
    }

    auto outMount = nsjailConfig.add_mount();
    outMount->set_src(absolute_out_dir.generic_string());
    outMount->set_dst(absolute_out_dir_in_sandbox);
    outMount->set_is_bind(true);
    outMount->set_is_dir(true);
    outMount->set_rw(true);

    // Mount the rsp file, if there is any.
    string rspFile = edge->GetUnescapedRspfile();
    if (!rspFile.empty()) {
      auto rspMount = nsjailConfig.add_mount();
      auto rspFilePath = std::filesystem::path(rspFile);
      rspMount->set_src((set->config_.cwd / rspFilePath).generic_string());
      rspMount->set_dst((std::filesystem::path("/src") / rspFilePath).generic_string());
      rspMount->set_is_bind(true);
      rspMount->set_is_dir(false);
    }

    std::vector<Node*> nodes_to_process = edge->inputs_;
    std::unordered_set<Node*> processed_nodes;

    while (!nodes_to_process.empty()) {
      Node* input = nodes_to_process.back();
      nodes_to_process.pop_back();

      if (processed_nodes.count(input))
        continue;
      processed_nodes.insert(input);

      if (input->in_edge() && input->in_edge()->is_phony()) {
        nodes_to_process.insert(nodes_to_process.end(),
                                input->in_edge()->inputs_.begin(),
                                input->in_edge()->inputs_.end());
        continue;
      }

      auto input_path = set->config_.cwd / input->path();
      // input_path_in_sandbox is always rooted at /src, unless the path from the ninja file was
      // an absolute path (like when setting OUT_DIR to an absolute path).
      auto input_path_in_sandbox = std::filesystem::path("/src") / input->path();
      std::error_code ec;
      // TODO: we should reuse the stat-ing from the main ninja work calculations
      auto stat = std::filesystem::symlink_status(input_path, ec);
      if (ec) {
        Fatal("Failed to lstat %s: %s", input_path.generic_string().c_str(), ec.message().c_str());
      }
      auto type = stat.type();
      if (type == std::filesystem::file_type::symlink) {
        auto link = std::filesystem::read_symlink(input_path);
        auto inputMount = nsjailConfig.add_mount();
        inputMount->set_src(link.generic_string());
        inputMount->set_dst(input_path_in_sandbox.generic_string());
        inputMount->set_is_symlink(true);
        inputMount->set_is_dir(false);
      } else if (type == std::filesystem::file_type::regular) {
        auto inputMount = nsjailConfig.add_mount();
        inputMount->set_src(input_path.generic_string());
        inputMount->set_dst(input_path_in_sandbox.generic_string());
        inputMount->set_is_bind(true);
        inputMount->set_is_dir(false);
      } else {
        Fatal("Unsupported input file type (usually means this is a directory): %s", input_path.generic_string().c_str());
      }
    }
    auto exe = nsjailConfig.mutable_exec_bin();
    exe->set_path("/bin/sh");
    exe->add_arg("-c");
    exe->add_arg(cmd.command);

    args.push_back("-C");

    auto nsjailConfigFilePath = nsjail_workdir_.value().path() / "nsjail.config";
    int fd = open(nsjailConfigFilePath.generic_string().c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    // we mostly ignore errors for simplicity here, but nsjail will error out if seeing a -C without
    // an argument or to a nonexistent file.
    if (fd >= 0) {
      {
        google::protobuf::io::FileOutputStream file_stream(fd);
        google::protobuf::TextFormat::Print(nsjailConfig, &file_stream);
      }
      close(fd);

      buff.push_back(nsjailConfigFilePath.generic_string());
      args.push_back(buff.back().c_str());
    }
  }
  args.push_back(nullptr);

  err = posix_spawn(&pid_, args.front(), &action, &attr,
        (char* const*)args.data(), cmd.env ? cmd.env : environ);
  if (err != 0)
    Fatal("posix_spawn: %s", strerror(err));

  err = posix_spawnattr_destroy(&attr);
  if (err != 0)
    Fatal("posix_spawnattr_destroy: %s", strerror(err));
  err = posix_spawn_file_actions_destroy(&action);
  if (err != 0)
    Fatal("posix_spawn_file_actions_destroy: %s", strerror(err));

  close(output_pipe[1]);
  return true;
}

void Subprocess::OnPipeReady() {
  char buf[4 << 10];
  ssize_t len = read(fd_, buf, sizeof(buf));
  if (len > 0) {
    buf_.append(buf, len);
  } else {
    if (len < 0)
      Fatal("read: %s", strerror(errno));
    close(fd_);
    fd_ = -1;
  }
}

const struct rusage* Subprocess::GetUsage() const {
  return &rusage_;
}

ExitStatus Subprocess::Finish() {
  assert(pid_ != -1);
  int status;
  if (wait4(pid_, &status, 0, &rusage_) < 0)
    Fatal("wait4(%d): %s", pid_, strerror(errno));
  pid_ = -1;

  if (WIFEXITED(status)) {
    int exit = WEXITSTATUS(status);
    if (exit == 0) {
      if (nsjail_workdir_.has_value()) {
        for (Node* output : edge_->outputs_) {
          auto finalOutPath = output->path();
          auto sandboxedOutPath = OutPathToNsjailOutPath(output->path());
          std::error_code ec;
          std::filesystem::rename(sandboxedOutPath, finalOutPath, ec);
          if (ec) {
            Fatal("Failed to move %s -> %s: %s", sandboxedOutPath.generic_string().c_str(), finalOutPath.c_str(), ec.message().c_str());
          }
        }
      }
      return ExitSuccess;
    }
  } else if (WIFSIGNALED(status)) {
    if (WTERMSIG(status) == SIGINT || WTERMSIG(status) == SIGTERM
        || WTERMSIG(status) == SIGHUP)
      return ExitInterrupted;
  }
  if (nsjail_workdir_.has_value()) {
    // keep the directory around for debugging
    nsjail_workdir_.value().leak();

    buf_.append("Failed command ran in sandbox directory: ");
    buf_.append(nsjail_workdir_.value().path().generic_string());
    buf_.append("\n");
  }
  return ExitFailure;
}

bool Subprocess::Done() const {
  return fd_ == -1;
}

const string& Subprocess::GetOutput() const {
  return buf_;
}

int SubprocessSet::interrupted_;

void SubprocessSet::SetInterruptedFlag(int signum) {
  interrupted_ = signum;
}

void SubprocessSet::HandlePendingInterruption() {
  sigset_t pending;
  sigemptyset(&pending);
  if (sigpending(&pending) == -1) {
    perror("ninja: sigpending");
    return;
  }
  if (sigismember(&pending, SIGINT))
    interrupted_ = SIGINT;
  else if (sigismember(&pending, SIGTERM))
    interrupted_ = SIGTERM;
  else if (sigismember(&pending, SIGHUP))
    interrupted_ = SIGHUP;
}

SubprocessSet::SubprocessSet(const BuildConfig& config): config_(config) {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);
  sigaddset(&set, SIGHUP);
  if (sigprocmask(SIG_BLOCK, &set, &old_mask_) < 0)
    Fatal("sigprocmask: %s", strerror(errno));

  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_handler = SetInterruptedFlag;
  if (sigaction(SIGINT, &act, &old_int_act_) < 0)
    Fatal("sigaction: %s", strerror(errno));
  if (sigaction(SIGTERM, &act, &old_term_act_) < 0)
    Fatal("sigaction: %s", strerror(errno));
  if (sigaction(SIGHUP, &act, &old_hup_act_) < 0)
    Fatal("sigaction: %s", strerror(errno));
}

SubprocessSet::~SubprocessSet() {
  Clear();

  if (sigaction(SIGINT, &old_int_act_, 0) < 0)
    Fatal("sigaction: %s", strerror(errno));
  if (sigaction(SIGTERM, &old_term_act_, 0) < 0)
    Fatal("sigaction: %s", strerror(errno));
  if (sigaction(SIGHUP, &old_hup_act_, 0) < 0)
    Fatal("sigaction: %s", strerror(errno));
  if (sigprocmask(SIG_SETMASK, &old_mask_, 0) < 0)
    Fatal("sigprocmask: %s", strerror(errno));
}

Subprocess *SubprocessSet::Add(const EdgeCommand& cmd, Edge* edge, int extra_fd) {
  Subprocess *subprocess = new Subprocess(cmd.use_console);
  if (!subprocess->Start(this, cmd, edge, extra_fd)) {
    delete subprocess;
    return 0;
  }
  running_.push_back(subprocess);
  return subprocess;
}

#ifdef USE_PPOLL
bool SubprocessSet::DoWork() {
  vector<pollfd> fds;
  nfds_t nfds = 0;

  for (vector<Subprocess*>::iterator i = running_.begin();
       i != running_.end(); ++i) {
    int fd = (*i)->fd_;
    if (fd < 0)
      continue;
    pollfd pfd = { fd, POLLIN | POLLPRI, 0 };
    fds.push_back(pfd);
    ++nfds;
  }

  interrupted_ = 0;
  int ret = ppoll(&fds.front(), nfds, NULL, &old_mask_);
  if (ret == -1) {
    if (errno != EINTR) {
      perror("ninja: ppoll");
      return false;
    }
    return IsInterrupted();
  }

  HandlePendingInterruption();
  if (IsInterrupted())
    return true;

  nfds_t cur_nfd = 0;
  for (vector<Subprocess*>::iterator i = running_.begin();
       i != running_.end(); ) {
    int fd = (*i)->fd_;
    if (fd < 0)
      continue;
    assert(fd == fds[cur_nfd].fd);
    if (fds[cur_nfd++].revents) {
      (*i)->OnPipeReady();
      if ((*i)->Done()) {
        finished_.push(*i);
        i = running_.erase(i);
        continue;
      }
    }
    ++i;
  }

  return IsInterrupted();
}

#else  // !defined(USE_PPOLL)
bool SubprocessSet::DoWork() {
  fd_set set;
  int nfds = 0;
  FD_ZERO(&set);

  for (vector<Subprocess*>::iterator i = running_.begin();
       i != running_.end(); ++i) {
    int fd = (*i)->fd_;
    if (fd >= 0) {
      FD_SET(fd, &set);
      if (nfds < fd+1)
        nfds = fd+1;
    }
  }

  interrupted_ = 0;
  int ret = pselect(nfds, &set, 0, 0, 0, &old_mask_);
  if (ret == -1) {
    if (errno != EINTR) {
      perror("ninja: pselect");
      return false;
    }
    return IsInterrupted();
  }

  HandlePendingInterruption();
  if (IsInterrupted())
    return true;

  for (vector<Subprocess*>::iterator i = running_.begin();
       i != running_.end(); ) {
    int fd = (*i)->fd_;
    if (fd >= 0 && FD_ISSET(fd, &set)) {
      (*i)->OnPipeReady();
      if ((*i)->Done()) {
        finished_.push(*i);
        i = running_.erase(i);
        continue;
      }
    }
    ++i;
  }

  return IsInterrupted();
}
#endif  // !defined(USE_PPOLL)

Subprocess* SubprocessSet::NextFinished() {
  if (finished_.empty())
    return NULL;
  Subprocess* subproc = finished_.front();
  finished_.pop();
  return subproc;
}

void SubprocessSet::Clear() {
  for (vector<Subprocess*>::iterator i = running_.begin();
       i != running_.end(); ++i)
    // Since the foreground process is in our process group, it will receive
    // the interruption signal (i.e. SIGINT or SIGTERM) at the same time as us.
    if (!(*i)->use_console_)
      kill(-(*i)->pid_, interrupted_);
  for (vector<Subprocess*>::iterator i = running_.begin();
       i != running_.end(); ++i)
    delete *i;
  running_.clear();
}
