#pragma once
#include "preclude.hpp"

#include <liburing.h>
#include <sys/eventfd.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <chrono>
#include <coroutine>

#if !IO_URING_CHECK_VERSION(2, 5)
  #error "io_uring version is too low"
#endif

namespace coco {
class Worker;
class WorkerJob;
class MtExecutor;

using BufView = std::span<std::byte const>;
using BufSlice = std::span<std::byte>;

constexpr std::uint32_t kIoUringQueueSize = 2048;
using Token = void*;
struct GlobalUringInfo {
  static auto get() -> GlobalUringInfo&
  {
    static auto info = GlobalUringInfo();
    return info;
  }
  std::atomic_size_t uringTaskCount = 0;
};

template <typename Rep, typename Ratio>
static auto convertTime(std::chrono::duration<Rep, Ratio> duration, struct __kernel_timespec& out) noexcept -> void
{
  auto sec = std::chrono::duration_cast<std::chrono::seconds>(duration);
  auto nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(duration - sec);
  out.tv_sec = sec.count();
  out.tv_nsec = nsec.count();
}

class UringInstance {
public:
  static auto get() noexcept -> UringInstance&
  {
    static thread_local auto instance = UringInstance();
    return instance;
  }

  UringInstance();
  ~UringInstance();

  UringInstance(UringInstance&& other) = delete;
  auto operator=(UringInstance&& other) -> UringInstance& = delete;
  UringInstance(UringInstance const& other) = delete;
  auto operator=(UringInstance const& other) -> UringInstance& = delete;

  auto attachWorker(Worker* worker) noexcept -> void { mWorker = worker; }
  auto attachMtExecutor(MtExecutor* exe) noexcept -> void { mMtExecutor = exe; }
  auto execute(WorkerJob* job) noexcept -> void;
  auto execute(std::coroutine_handle<> handle) -> void;

  auto prepRecv(Token token, int fd, BufSlice buf, int flag = 0) -> void;
  auto prepSend(Token token, int fd, BufView buf, int flag = 0) -> void;
  auto prepAccept(Token token, int fd, sockaddr* addr, socklen_t* addrlen, int flags = 0) -> void;
  auto prepCancel(int fd) -> void;
  auto prepCancel(Token token) -> void;
  auto prepClose(Token token, int fd) -> void;

  auto seen(io_uring_cqe* cqe) -> void;
  auto submitWait(int waitn) -> int;
  template <typename Rep, typename Ratio>
  auto submitWait(io_uring_cqe*& cqe, std::chrono::duration<Rep, Ratio> duration) -> std::errc
  {
    auto timeout = __kernel_timespec{};
    convertTime(duration, timeout);
    auto r = ::io_uring_submit_and_wait_timeout(&mUring, &cqe, 1, &timeout, 0);
    return r < 0 ? std::errc(-r) : std::errc(0);
  }
  template <typename Rep, typename Ratio>
  auto submitWait(std::span<io_uring_cqe*> cqes, std::chrono::duration<Rep, Ratio> duration) -> std::errc
  {
    auto timeout = __kernel_timespec{};
    convertTime(duration, timeout);
    auto r = ::io_uring_submit_and_wait_timeout(&mUring, cqes.data(), cqes.size(), &timeout, 0);
    return r < 0 ? std::errc(-r) : std::errc(0);
  }

  // TODO: I can't find a method to notify a uring without a real fd :(.
  auto notify() -> void;

private:
  auto fetchSqe() -> io_uring_sqe*;

private:
  MtExecutor* mMtExecutor;
  Worker* mWorker;
  int mEventFd;
  ::io_uring mUring;
};
} // namespace coco