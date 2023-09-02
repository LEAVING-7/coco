#pragma once
#include "coco/__preclude.hpp"

#include "coco/inl_executor.hpp"
#include "coco/mt_executor.hpp"

#include <functional>

namespace coco {
enum class RuntimeType {
  Inline,
  Multi,
};
constexpr static RuntimeType MT = RuntimeType::Multi;
constexpr static RuntimeType INL = RuntimeType::Inline;

class Runtime {
  struct [[nodiscard]] CondJob : WorkerJob {
    CondJob(std::atomic_size_t* count, WorkerJob* nextJob) : mCount(count), mNextJob(nextJob), WorkerJob(&CondJob::run)
    {
    }

    static auto run(WorkerJob* job, void*) noexcept -> void
    {
      auto condJob = static_cast<CondJob*>(job);
      auto count = condJob->mCount->fetch_sub(1);
      if (count == 1) {
        Proactor::get().execute(condJob->mNextJob);
      }
      delete condJob;
    }
    std::atomic_size_t* mCount;
    WorkerJob* mNextJob;
  };

  struct [[nodiscard]] SleepAwaiter {
    SleepAwaiter(Duration duration) : mDuration(duration) {}
    auto await_ready() const noexcept -> bool { return false; }
    template <typename Promise>
    auto await_suspend(std::coroutine_handle<Promise> handle) noexcept -> void
    {
      auto job = handle.promise().getThisJob();
      Proactor::get().addTimer(std::chrono::steady_clock::now() + mDuration, job);
    }
    auto await_resume() const noexcept -> void {}

  private:
    Duration mDuration;
  };

  template <typename TaskTupleTy>
  struct [[nodiscard]] WaitNAwaiter {
    constexpr WaitNAwaiter(std::size_t waiting, Executor* executor, TaskTupleTy&& tasks)
        : mWaitingCount(waiting), mExecutor(executor), mTasks(std::move(tasks))
    {
    }

    auto await_ready() const noexcept -> bool { return false; }
    template <typename Promise>
    auto await_suspend(std::coroutine_handle<Promise> handle) -> void
    {
      std::vector<std::atomic<PromiseState>*> jobConds;
      auto nextJob = handle.promise().getThisJob();

      auto setupTask = [&](auto&& task) { jobConds.push_back(&task.promise().mState); };
      std::apply([&](auto&&... tuple) { (setupTask(tuple), ...); }, mTasks);

      auto addWaitingJob = [&](auto&& task) { task.promise().addWaitingJob(new CondJob(&mWaitingCount, nextJob)); };

      std::apply([&](auto&&... tuple) { (addWaitingJob(tuple), ...); }, mTasks);

      assert(jobConds.size() == std::tuple_size_v<TaskTupleTy>);

      auto runTask = [this](auto&& task) { mExecutor->enqueue(task.promise().getThisJob()); };
      std::apply([&](auto&&... tuple) { (runTask(tuple), ...); }, mTasks);
    }
    auto await_resume() const noexcept -> void {}

    std::atomic_size_t mWaitingCount;
    Executor* mExecutor;
    TaskTupleTy mTasks;
  };

  struct [[nodiscard]] JoinAwaiter {
    JoinAwaiter(std::atomic<WorkerJob*>* done) : mDone(done) {}
    auto await_ready() const noexcept -> bool { return false; }
    template <typename Promise>
    auto await_suspend(std::coroutine_handle<Promise> handle) noexcept -> bool
    {
      auto job = handle.promise().getThisJob();
      WorkerJob* expected = &emptyJob;
      if (mDone->compare_exchange_strong(expected, job)) {
        return true;
      } else {
        return false;
      }
    }
    auto await_resume() const noexcept -> void {}
    std::atomic<WorkerJob*>* mDone;
  };

public:
  constexpr Runtime(RuntimeType type, std::size_t threadNum = 0) : mType(type)
  {
    if (type == RuntimeType::Inline) {
      mExecutor = std::make_shared<InlExecutor>();
    } else if (type == RuntimeType::Multi) {
      mExecutor = std::make_shared<MtExecutor>(threadNum);
    }
  }

  auto block(Task<> task) -> void { mExecutor->runMain(std::move(task)); }

  template <typename Rep, typename Per>
  [[nodiscard]] auto sleep(std::chrono::duration<Rep, Per> duration) -> Task<>
  {
    auto sleepTime = std::chrono::duration_cast<coco::Duration>(duration);
    co_return co_await SleepAwaiter(sleepTime);
  }

  template <TaskConcept... TasksTy>
  [[nodiscard]] constexpr auto waitAll(TasksTy&&... tasks) -> decltype(auto)
  {
    auto tasksTuple = std::make_tuple(std::forward<TasksTy>(tasks)...);
    constexpr auto taskCount = std::tuple_size_v<decltype(tasksTuple)>;
    return WaitNAwaiter<std::tuple<TasksTy...>>(taskCount, mExecutor.get(), std::move(tasksTuple));
  }

  template <typename TasksTuple>
  [[nodiscard]] constexpr auto waitN(std::size_t n, TasksTuple&& tuple) -> decltype(auto)
  {
    static_assert(n <= std::tuple_size_v<TasksTuple>, "n must be less than the task count");
    return WaitNAwaiter<TasksTuple>(n, mExecutor.get(), std::move(tuple));
  }

  // FIXME: this function is not correct
  template <TaskConcept... TasksTy>
  [[deprecated("function incorrect")]] constexpr auto waitAny(TasksTy&&... tasks) -> decltype(auto)
  {
    auto tasksTuple = std::make_tuple(std::forward<TasksTy>(tasks)...);
    return WaitNAwaiter<std::tuple<TasksTy...>>(1, mExecutor.get(), std::move(tasksTuple));
  }

  template <typename TaskTy>
  struct JoinHandle {
    JoinHandle(TaskTy&& task) noexcept
        : mTask(std::forward<TaskTy>(task)), mDone(std::make_unique<std::atomic<WorkerJob*>>(&emptyJob))
    {
      mTask.promise().addContinueJob(mDone.get());
      Proactor::get().execute(mTask.promise().getThisJob());
    }
    JoinHandle(JoinHandle&&) : mTask(std::move(mTask)), mDone(std::move(mDone)) {}
    ~JoinHandle() noexcept
    {
      if (mDone->load() == &emptyJob) {
        assert(false && "looks like you forget to call join()");
      }
    }
    [[nodiscard]] auto join() -> decltype(auto) { return JoinAwaiter(mDone.get()); }

    // atomic variable cannot be moved, so I use unique_ptr to wrap it
    std::unique_ptr<std::atomic<WorkerJob*>> mDone;
    TaskTy mTask;
  };

  template <TaskConcept TaskTy>
  [[nodiscard]] constexpr auto spawn(TaskTy&& task) -> JoinHandle<TaskTy>
  {
    return JoinHandle<TaskTy>(std::move(task));
  }

  // !!! task must be done before block() function returns, otherwise it will cause memory leak
  template <TaskConcept TaskTy>
  constexpr auto spawnDetach(TaskTy&& task) -> void
  {
    auto& promise = task.promise();
    promise.mState.store(coco::PromiseState::NeedDetach);
    Proactor::get().execute(promise.getThisJob());
    [[maybe_unused]] auto handel = task.take();
  }

private:
  const RuntimeType mType;
  std::shared_ptr<Executor> mExecutor;
};
} // namespace coco
