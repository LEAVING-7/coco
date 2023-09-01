#pragma once
#include "preclude.hpp"

#include "util/queue.hpp"

#include <coroutine>

namespace coco {
inline auto genJobId() -> std::size_t
{
  static std::atomic<std::size_t> id = 0;
  return id.fetch_add(1, std::memory_order_relaxed);
}

struct WorkerJob {
  using fn_type = void (*)(WorkerJob* task, void* args) noexcept;
  WorkerJob(fn_type fn) noexcept : run(fn), next(nullptr), id(genJobId()) {}

  WorkerJob* next;
  fn_type run;
  std::size_t id;
};

using WorkerJobQueue = util::Queue<&WorkerJob::next>;

// TODO: need better implementation
struct CoroJob : WorkerJob {
  CoroJob(std::coroutine_handle<> handle, WorkerJob::fn_type fn) noexcept : handle(handle), WorkerJob(fn) {}
  static auto run(WorkerJob* job, void* args) noexcept -> void
  {
    auto coroJob = static_cast<CoroJob*>(job);
    coroJob->handle.resume();
  }
  std::coroutine_handle<> handle;
};

class Executor {
public:
  Executor() = default;
  virtual ~Executor() noexcept = default;

  virtual auto enqueue(WorkerJobQueue&& queue, std::size_t count) noexcept -> void = 0;
  virtual auto enqueue(WorkerJob* handle) noexcept -> void = 0;
};
} // namespace coco