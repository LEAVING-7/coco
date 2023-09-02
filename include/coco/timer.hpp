#pragma once
#include "coco/__preclude.hpp"

#include "coco/util/heap.hpp"
#include "coco/util/lockfree_queue.hpp"
#include "coco/worker_job.hpp"

#include <chrono>
#include <queue>
#include <unordered_set>

namespace coco {

using Instant = std::chrono::steady_clock::time_point;
using Duration = std::chrono::steady_clock::duration;

enum class TimerOpKind : std::uint8_t {
  Add,
  Delete,
};

struct TimerOp {
  Instant instant;
  union {
    WorkerJob* job;    // add a timer
    std::size_t jobId; // for delete a timer
  };
  TimerOpKind kind;
};

inline auto operator<(TimerOp const& lhs, TimerOp const& rhs) noexcept -> bool { return lhs.instant < rhs.instant; }

class TimerManager {
public:
  TimerManager() noexcept = default;
  TimerManager(std::size_t capcacity) : mTimers(capcacity), mPendingJobs() {}
  ~TimerManager() = default;

  // MT-Safe
  auto addTimer(Instant time, WorkerJob* job) noexcept -> void;
  // MT-Safe
  auto deleteTimer(std::size_t jobId) noexcept -> void;
  auto nextInstant() const noexcept -> Instant;
  auto processTimers() -> std::pair<WorkerJobQueue, std::size_t>;

private:
  std::mutex mPendingJobsMt;
  std::queue<TimerOp> mPendingJobs;
  std::unordered_set<std::size_t> mDeleted;
  util::Heap<TimerOp, 4> mTimers;
};
} // namespace coco