#ifndef __PROCESS_PARALLEL_HPP__
#define __PROCESS_PARALLEL_HPP__

#include <utility>
#include <queue>
#include <cstring>
#include <string>

#include <glog/logging.h>

#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

#include <stout/lambda.hpp>
#include <stout/nothing.hpp>

namespace process {

// Forward declaration.
template <typename T>
class ParallelProcess;

// Provides an abstraction that limits the concurrency of asynchronous
// operations.
template <typename T>
class Parallel
{
public:
  Parallel(size_t maxConcurrency);
  ~Parallel();

  // Adds a unit of asynchronous work to execute. The work may be postponed to
  // ensure the concurrency limit is not violated.
  // Returns the result of the work.
  Future<T> add(const lambda::function<Future<T>()>& work);

private:
  // Not copyable, not assignable.
  Parallel<T>(const Parallel<T>&);
  Parallel<T>& operator = (const Parallel<T>&);

  ParallelProcess<T>* process;
};

template <typename T>
class ParallelProcess : public Process<ParallelProcess<T>>
{
public:
  ParallelProcess(std::size_t _maxConcurrency)
  {
    maxConcurrency = _maxConcurrency;
    CHECK_GT(maxConcurrency, 0);
    workSize = 0;
  }

  Future<T> add(const lambda::function<Future<T>()>& work)
  {
    // This is the future that will be returned to the user.
    Owned<Promise<T>> promise(new Promise<T>());

    promise->future().onAny([=]() { completed(); });

    if (workSize > maxConcurrency) {
        waitQueue.push(std::make_pair(promise, work));
    } else {
      notified(promise, work);
    }

    return promise->future();
  }

private:
  // Invoked when a callback is done.
  void completed()
  {
    workSize--;
    if (!waitQueue.empty()) {
      notified(waitQueue.front().first, waitQueue.front().second);
      waitQueue.pop();
    }
  }

  void notified(
      Owned<Promise<T>> promise,
      const lambda::function<Future<T>()>& work)
  {
    if (promise->future().hasDiscard()) {
      promise->discard();
    } else {
      promise->associate(work());
      workSize++;
    }
  }

  // Size of currently running jobs.
  size_t workSize;
  size_t maxConcurrency;

  // TODO(chenlily): Split tuple into a struct w/ const member variables.
  std::queue<
      std::pair<Owned<Promise<T>>, lambda::function<Future<T>()> >> waitQueue;
};

template <typename T>
inline Parallel<T>::Parallel(size_t maxConcurrency)
{
  process = new typename process::ParallelProcess<T>(maxConcurrency);
  process::spawn(process);
}

template <typename T>
inline Parallel<T>::~Parallel()
{
  process::terminate(process);
  process::wait(process);
  delete process;
}


template <typename T>
Future<T> Parallel<T>::add(const lambda::function<Future<T>()>& work)
{
  return dispatch(process, &ParallelProcess<T>::add, work);
}

} // namespace process {

#endif // __PROCESS_Parallel_HPP__
