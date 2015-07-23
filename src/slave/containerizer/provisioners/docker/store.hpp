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

#ifndef __MESOS_DOCKER_STORE__
#define __MESOS_DOCKER_STORE__

#include <string>
#include <vector>

#include <stout/hashmap.hpp>
#include <stout/option.hpp>
#include <stout/result.hpp>
#include <stout/try.hpp>

#include <process/future.hpp>
#include <process/process.hpp>
#include <process/shared.hpp>

#include "slave/containerizer/provisioners/docker.hpp"
#include "slave/flags.hpp"

namespace mesos {
namespace internal {
namespace slave {
namespace docker {

class Store
{
public:
  static Try<process::Owned<Store>> create(
      const Flags& flags,
      Fetcher* fetcher);

  virtual ~Store() {}

  // Put an image into to the store. Returns the DockerImage containing
  // the manifest, hash of the image, and the path to the extracted
  // image.
  virtual process::Future<DockerImage> put(
      const std::string& name,
      const std::string& sandbox) = 0;

  // Get image by name.
  virtual process::Future<Option<DockerImage>> get(const std::string& name) = 0;
};


// Forward declaration.
class LocalStoreProcess;

class LocalStore : public Store
{
public:
  virtual ~LocalStore();

  static Try<process::Owned<Store>> create(
      const Flags& flags,
      Fetcher* fetcher);

  // Put assumes the image tar archive is located in the directory specified in
  // the slave flag docker_discovery_local_dir and is named with <name>.tar .
  virtual process::Future<DockerImage> put(
      const std::string& name,
      const std::string& sandbox);

  virtual process::Future<Option<DockerImage>> get(const std::string& name);

private:
  explicit LocalStore(process::Owned<LocalStoreProcess> process);

  LocalStore(const LocalStore&); // Not copyable.
  LocalStore& operator=(const LocalStore&); // Not assignable.

  process::Owned<LocalStoreProcess> process;
};

class LocalStoreProcess : public process::Process<LocalStoreProcess>
{
public:
  ~LocalStoreProcess() {}

  static Try<process::Owned<LocalStoreProcess>> create(
      const Flags& flags,
      Fetcher* fetcher);

  process::Future<DockerImage> put(
      const std::string& name,
      const std::string& sandbox);

  process::Future<Option<DockerImage>> get(const std::string& name);

private:
  LocalStoreProcess(
      const Flags& flags,
      Fetcher* fetcher);

  Try<Nothing> restore();

  process::Future<Nothing> untarImage(
      const std::string& tarPath,
      const std::string& imagePath);

  process::Future<DockerImage> putImage(
      const std::string& name,
      const std::string& imagePath,
      const std::string& sandbox);

  Result<std::string> getParentId(
      const std::string& imagePath,
      const std::string& layerId);

  process::Future<Nothing> putLayers(
      const std::string& imagePath,
      const std::list<std::string>& layers,
      const std::string& sandbox);

  process::Future<Nothing> untarLayer(
      const std::string& imagePath,
      const std::string& id,
      const std::string& sandbox);

  process::Future<Nothing> copyLayer(
      const std::string& imagePath,
      const std::string& id,
      const std::string& sandbox);

  const Flags flags;

  // This map is keyed by the image name and its value is the corresponding
  // DockerImage.
  hashmap<std::string, DockerImage> images;

  Fetcher* fetcher;
};

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __MESOS_DOCKER_STORE__
