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

#include "metrics.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#else
#include <windows.h>
#endif

#include <algorithm>

#include "status.h"
#include "util.h"

Metrics* g_metrics = NULL;

namespace {

#ifndef _WIN32
/// Compute a platform-specific high-res timer value that fits into an int64.
int64_t HighResTimer() {
  timeval tv;
  if (gettimeofday(&tv, NULL) < 0)
    Fatal("gettimeofday: %s", strerror(errno));
  return (int64_t)tv.tv_sec * 1000*1000 + tv.tv_usec;
}

/// Convert a delta of HighResTimer() values to microseconds.
int64_t TimerToMicros(int64_t dt) {
  // No conversion necessary.
  return dt;
}
#else
int64_t LargeIntegerToInt64(const LARGE_INTEGER& i) {
  return ((int64_t)i.HighPart) << 32 | i.LowPart;
}

int64_t HighResTimer() {
  LARGE_INTEGER counter;
  if (!QueryPerformanceCounter(&counter))
    Fatal("QueryPerformanceCounter: %s", GetLastErrorString().c_str());
  return LargeIntegerToInt64(counter);
}

int64_t TimerToMicros(int64_t dt) {
  static int64_t ticks_per_sec = 0;
  if (!ticks_per_sec) {
    LARGE_INTEGER freq;
    if (!QueryPerformanceFrequency(&freq))
      Fatal("QueryPerformanceFrequency: %s", GetLastErrorString().c_str());
    ticks_per_sec = LargeIntegerToInt64(freq);
  }

  // dt is in ticks.  We want microseconds.
  return (dt * 1000000) / ticks_per_sec;
}
#endif

}  // anonymous namespace


void ScopedMetric::RecordStart() {
  start_ = HighResTimer();
}

void ScopedMetric::RecordResult() {
  int64_t duration = TimerToMicros(HighResTimer() - start_);
  metric_->AddResult(1, duration);
}

Metric* Metrics::NewMetric(const string& name) {
  std::lock_guard<std::mutex> lock(mutex_);
  Metric* result = new Metric(name);
  metrics_.push_back(result);
  return result;
}

void Metrics::Report(Status *status) {
  std::lock_guard<std::mutex> lock(mutex_);

  int width = 0;
  for (vector<Metric*>::iterator i = metrics_.begin();
       i != metrics_.end(); ++i) {
    width = max((int)(*i)->name().size(), width);
  }

  status->Debug("%-*s\t%-6s\t%-9s\t%s", width,
         "metric", "count", "avg (us)", "total (ms)");
  for (vector<Metric*>::iterator i = metrics_.begin();
       i != metrics_.end(); ++i) {
    Metric* metric = *i;
    double total = metric->time() / (double)1000;
    double avg = metric->time() / (double)metric->count();
    status->Debug("%-*s\t%-6d\t%-8.1f\t%.1f", width, metric->name().c_str(),
           metric->count(), avg, total);
  }
}

uint64_t Stopwatch::Now() const {
  return TimerToMicros(HighResTimer());
}

int64_t GetTimeMillis() {
  return TimerToMicros(HighResTimer()) / 1000;
}

void DumpMemoryUsage(Status *status) {
#if defined(__linux__)
  std::vector<std::string> words;
  struct rusage usage {};
  if (getrusage(RUSAGE_SELF, &usage) == 0) {
    words.push_back(std::to_string(usage.ru_majflt) + " maj faults");
    words.push_back(std::to_string(usage.ru_minflt) + " min faults");
    words.push_back(std::to_string(usage.ru_maxrss / 1024) + " MiB maxrss");
  }
  char status_path[256];
  snprintf(status_path, sizeof(status_path), "/proc/%d/status",
           static_cast<int>(getpid()));
  if (FILE* status_fp = fopen(status_path, "r")) {
    char* line = nullptr;
    size_t n = 0;
    while (getline(&line, &n, status_fp) > 0) {
      unsigned long rss = 0;
      if (sscanf(line, "VmRSS:\t%lu kB\n", &rss) == 1) {
        words.push_back(std::to_string(rss / 1024) + " MiB rss");
      }
    }
    free(line);
    fclose(status_fp);
  }
  if (!words.empty()) {
    string final;
    for (size_t i = 0; i < words.size(); ++i) {
      if (i > 0) {
        final += ", ";
      }
      final += words[i];
    }
    status->Debug("%s", final.c_str());
  }
#endif
}
