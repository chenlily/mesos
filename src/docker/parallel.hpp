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

// Execute work in parallel. If the size of the work exceeds the concurrency
// limit, the work may be postponed. The work will be parallelized in batches.
// Returns the result of the work.
template <typename T>
Future<std::list<T>> batch(
    size_t maxConcurrency,
    std::list<lambda::function<Future<T>(void)>& work);

namespace internal {

template <typename T>
inline Future<std::list<T>> batch(
    size_t maxConcurrency,
    std::list<lambda::function<Future<T>(void)>& work)
{
  if (work.empty()) {
    return std::list<T>();
  }

  Promise<std::list<T>>* promise = new Promise<std::list<T>>();
  Future<std::list<T>> future = promise->future();
  spawn(new internal::BatchProcess<T>(work, promise), true);
  return future;
}


template <typename T>
class BatchProcess : public Process<BatchProcess<T>>
{
public:
  BatchProcess(
      const std::list<lambda::function<Future<T>(void)>& _futures,
      Promise<std::list<T>>* _promise)
    : futures(_futures),
      promise(_promise),
      ready(0) {}

  virtual ~CollectProcess()
  {
    delete promise;
  }

  virtual void initialize()
  {
    promise->future().onDiscard(defer(this, &CollectProcess::discarded));

    // Split work into batches.
    vector<vector<lambda::function<Future<T>(void)>>> batches;
    vector<lambda::function<Future<T>(void)>> batch;
    batches.push_back(batch);
    foreach (const lambda::function<Future<T>(void)>& callback, work)
    {
      if(batches.empty() || batches.back().size() == maxConcurrency) {
        batches.push_back(batch);
      }
      batches.back().push_back(callback);
    }

    list<Future<Nothing>> futures{ Nothing() };
    foreach (const vector<lambda::function<Future<T>(void)>>& batch, batches) {
    futures.push_back(
        futures.back().then(
          defer(self(), &Self::processBatch, batch)));
    }

    collect(futures)
      .onAny([=](Future<list<Nothing>> & status) {
        if (status.isError()) {
          promise->fail("Failed to proceses in parallel: " + status.error());
          terminate(this);
        }
        if (status.isDiscarded()) {
          promise->fail("Failed to process in batch, future discarded.");
          terminate(this);
        }
        promise->set(results);
        terminate(this);
      });
  }

private:
  void discarded()
  {
    promise->discard();
    terminate(this);
  }

  Future<Nothing> processBatch(batch)
  {
    foreach(job, batch) {
      Future<T> result = internal::run(job);
      result.onAny(const Future<T>& status) {
        if
      }
      if (result.isError()) {
        return Failure("Failed to run an job in parallel process batch");
      }
      results.push_back(result.get());
    }
    return Nothing();
  }

  Promise<std::list<T>>* promise;
  std::list<T> results;
};

} // namespace internal {
} // namespace process {

#endif // __PROCESS_Parallel_HPP__
