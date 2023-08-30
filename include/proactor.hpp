#pragma once
#include "preclude.hpp"

#include "timer.hpp"
#include "uring.hpp"
#include "worker_job.hpp"

namespace coco {
class Proactor {
public:
  static auto get() noexcept -> Proactor&
  {
    static thread_local auto instance = Proactor();
    return instance;
  }

  Proactor() = default;
  ~Proactor() = default;

  auto attachExecutor(Executor* executor) noexcept -> void { mExecutor = executor; }
  auto execute(WokerJobQueue&& queue) noexcept -> void { mExecutor->enqueue(std::move(queue), 0); }
  auto execute(WokerJobQueue&& queue, std::size_t count) noexcept -> void
  {
    mExecutor->enqueue(std::move(queue), count);
  }
  auto execute(std::coroutine_handle<> handle) noexcept -> void { mExecutor->enqueue(handle); }
  auto addTimer(Instant time, WorkerJob* job) noexcept -> void { mTimerManager.addTimer(time, job); }
  auto deleteTimer(std::size_t jobId) noexcept -> void { mTimerManager.deleteTimer(jobId); }

  auto notify() -> void { mUring.notify(); }
  auto wait(std::atomic_bool& notifying) -> WorkerJob*
  {
    auto [jobs, count] = mTimerManager.processTimers();
    execute(std::move(jobs));
    auto future = mTimerManager.nextInstant();
    auto duration = future - std::chrono::steady_clock::now();

    io_uring_cqe* cqe = nullptr;
    auto e = mUring.submitWait(cqe, duration);
    if (e == std::errc::stream_timeout) {
      // time out
    } else if (e != std::errc(0)) {
      // error occured
    } else {
      assert(cqe != nullptr);
      if (cqe->flags & IORING_CQE_F_MORE) {
        notifying = false;
      } else {
        return (WorkerJob*)cqe->user_data;
      }
      seen(cqe);
    }
    return nullptr;
  }
  // TODO
  auto seen(io_uring_cqe* cqe) -> void { mUring.seen(cqe); }

  auto prepRecv(Token token, int fd, BufSlice buf, int flag = 0) -> void { mUring.prepRecv(token, fd, buf, flag); }
  auto prepSend(Token token, int fd, BufView buf, int flag = 0) -> void { mUring.prepSend(token, fd, buf, flag); }
  auto prepAccept(Token token, int fd, sockaddr* addr, socklen_t* addrlen, int flags = 0) -> void
  {
    mUring.prepAccept(token, fd, addr, addrlen, flags);
  }
  auto prepCancel(int fd) -> void { mUring.prepCancel(fd); }
  auto prepCancel(Token token) -> void { mUring.prepCancel(token); }
  auto prepClose(Token token, int fd) -> void { mUring.prepClose(token, fd); }

private:
  Executor* mExecutor;
  TimerManager mTimerManager{64};
  IoUring mUring{};
};
} // namespace coco