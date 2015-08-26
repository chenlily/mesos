/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __MESOS_DOCKER_REMOTE_STORE__
#define __MESOS_DOCKER_REMOTE_STORE__

#include "slave/containerizer/provisioners/docker/store.hpp"

namespace mesos {
namespace internal {
namespace slave {
namespace docker {

// Forward declaration.
class RemoteStoreProcess;


class RemoteStore : public Store
{
public:
  virtual ~RemoteStore();

  static Try<process::Owned<Store>> create(
      const Flags& flags,
      Fetcher* fetcher);

  virtual process::Future<DockerImage> put(
      const std::string& name,
      const std::string& sandbox);

  virtual process::Future<Option<DockerImage>> get(
      const std::string& name);

private:
  explicit RemoteStore(process::Owned<RemoteStoreProcess> process);

  RemoteStore(const RemoteStore&); // Not copyable.
  RemoteStore& operator=(const RemoteStore&); // Not assignable.

  process::Owned<RemoteStoreProcess> process;
};

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __MESOS_DOCKER_REMOTE_STORE__
