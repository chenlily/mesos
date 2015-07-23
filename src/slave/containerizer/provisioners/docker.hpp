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

#ifndef __MESOS_DOCKER__
#define __MESOS_DOCKER__

#include <list>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <stout/hashmap.hpp>
#include <stout/json.hpp>
#include <stout/try.hpp>

#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>
#include <process/shared.hpp>

#include <mesos/resources.hpp>

#include "slave/containerizer/provisioner.hpp"
#include "slave/flags.hpp"

namespace mesos {
namespace internal {
namespace slave {
namespace docker {

// Forward declarations.
class Backend;
class Store;

struct ImageName
{
  std::string repo;
  std::string tag;

  ImageName(const std::string& name)
  {
    std::size_t found = name.find_last_of(':');
    if (found == std::string::npos) {
      repo = name;
      tag = "latest";
    } else {
      repo = name.substr(0, found);
      tag = name.substr(found + 1);
    }
  }

  ImageName() {}
};

struct DockerImage
{
  DockerImage() {}

  DockerImage(
      const std::string& name,
      const std::string& path,
      const Option<JSON::Object>& manifest,
      const std::list<std::string>& layers)
  : name(name), path(path), manifest(manifest), layers(layers) {}

  std::string name;
  std::string path;
  Option<JSON::Object> manifest;
  std::list<std::string> layers;
};

// Forward declaration.
class DockerProvisionerProcess;

class DockerProvisioner : public Provisioner
{
public:
  static Try<process::Owned<Provisioner>> create(
      const Flags& flags,
      Fetcher* fetcher);

  ~DockerProvisioner();

  virtual process::Future<Nothing> recover(
      const std::list<mesos::slave::ExecutorRunState>& states,
      const hashset<ContainerID>& orphans);

  virtual process::Future<std::string> provision(
      const ContainerID& containerId,
      const ContainerInfo::Image& image,
      const std::string& directory);

  virtual process::Future<Nothing> destroy(const ContainerID& containerId);

private:
  DockerProvisioner(process::Owned<DockerProvisionerProcess> process);
  DockerProvisioner(const DockerProvisioner&); // Not copyable.
  DockerProvisioner& operator=(const DockerProvisioner&); // Not assignable.

  process::Owned<DockerProvisionerProcess> process;
};


class DockerProvisionerProcess :
  public process::Process<DockerProvisionerProcess>
{
public:
  static Try<process::Owned<DockerProvisionerProcess>> create(
      const Flags& flags,
      Fetcher* fetcher);

  process::Future<Nothing> recover(
      const std::list<mesos::slave::ExecutorRunState>& states,
      const hashset<ContainerID>& orphans);

  process::Future<std::string> provision(
      const ContainerID& containerId,
      const ContainerInfo::Image::Docker& image,
      const std::string& directory);

  process::Future<Nothing> destroy(const ContainerID& containerId);

private:
  DockerProvisionerProcess(
      const Flags& flags,
      const process::Owned<Store>& store,
      const process::Owned<Backend>& backend);

  process::Future<std::string> _provision(
      const ContainerID& containerId,
      const DockerImage& image);

  process::Future<DockerImage> fetch(
      const std::string& name,
      const std::string& directory);

  const Flags flags;

  process::Owned<Store> store;
  process::Owned<Backend> backend;
};

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __MESOS_DOCKER__
