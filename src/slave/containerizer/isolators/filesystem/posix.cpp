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

#include <list>
#include <string>

#include <stout/fs.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>

#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>

#include "slave/paths.hpp"

#include "slave/containerizer/isolators/filesystem/posix.hpp"

using namespace process;

using std::list;
using std::string;

namespace mesos {
namespace internal {
namespace slave {

using mesos::slave::ExecutorRunState;
using mesos::slave::Isolator;
using mesos::slave::IsolatorProcess;
using mesos::slave::Limitation;

PosixFilesystemIsolatorProcess::PosixFilesystemIsolatorProcess(
    const Flags& _flags)
  : flags(_flags) {}


PosixFilesystemIsolatorProcess::~PosixFilesystemIsolatorProcess() {}


Try<Isolator*> PosixFilesystemIsolatorProcess::create(const Flags& flags)
{
  process::Owned<IsolatorProcess> process(
      new PosixFilesystemIsolatorProcess(flags));

  return new Isolator(process);
}


Future<Nothing> PosixFilesystemIsolatorProcess::recover(
    const list<ExecutorRunState>& states,
    const hashset<ContainerID>& orphans)
{
  foreach (const ExecutorRunState& state, states) {
    infos.put(state.id, Owned<Info>(new Info(state.directory)));
  }

  return Nothing();
}


Future<Option<CommandInfo>> PosixFilesystemIsolatorProcess::prepare(
    const ContainerID& containerId,
    const ExecutorInfo& executorInfo,
    const string& directory,
    const Option<string>& rootfs,
    const Option<string>& user)
{
  if (infos.contains(containerId)) {
    return Failure("Container has already been prepared");
  }

  if (rootfs.isSome()) {
    return Failure("Container root filesystems not supported");
  }

  if (executorInfo.has_container()) {
    return Failure("Containers not supported");
  }

  infos.put(containerId, Owned<Info>(new Info(directory)));

  return update(containerId, executorInfo.resources())
      .then([] () -> Future<Option<CommandInfo>> { return None(); });
}


Future<Nothing> PosixFilesystemIsolatorProcess::isolate(
    const ContainerID& containerId,
    pid_t pid)
{
  // No-op.

  return Nothing();
}


Future<Limitation> PosixFilesystemIsolatorProcess::watch(
    const ContainerID& containerId)
{
  // No-op.

  return Future<Limitation>();
}


Future<Nothing> PosixFilesystemIsolatorProcess::update(
    const ContainerID& containerId,
    const Resources& resources)
{
  if (!infos.contains(containerId)) {
    return Failure("Unknown container");
  }

  const Owned<Info>& info = infos[containerId];

  // TODO(jieyu): Currently, we only allow non-nested relative
  // container paths for volumes. This is enforced by the master. For
  // those volumes, we create symlinks in the executor directory.
  Resources current = info->resources;

  // We first remove unneeded persistent volumes.
  foreach (const Resource& resource, current.persistentVolumes()) {
    // This is enforced by the master.
    CHECK(resource.disk().has_volume());

    // Ignore absolute and nested paths.
    const string& containerPath = resource.disk().volume().container_path();
    if (strings::contains(containerPath, "/")) {
      LOG(WARNING) << "Skipping updating symlink for persistent volume "
                   << resource << " of container " << containerId
                   << " because the container path '" << containerPath
                   << "' contains slash";
      continue;
    }

    if (resources.contains(resource)) {
      continue;
    }

    string link = path::join(info->directory, containerPath);

    LOG(INFO) << "Removing symlink '" << link << "' for persistent volume "
              << resource << " of container " << containerId;

    Try<Nothing> rm = os::rm(link);
    if (rm.isError()) {
      return Failure(
          "Failed to remove the symlink for the unneeded "
          "persistent volume at '" + link + "'");
    }
  }

  // We then link additional persistent volumes.
  foreach (const Resource& resource, resources.persistentVolumes()) {
    // This is enforced by the master.
    CHECK(resource.disk().has_volume());

    // Ignore absolute and nested paths.
    const string& containerPath = resource.disk().volume().container_path();
    if (strings::contains(containerPath, "/")) {
      LOG(WARNING) << "Skipping updating symlink for persistent volume "
                   << resource << " of container " << containerId
                   << " because the container path '" << containerPath
                   << "' contains slash";
      continue;
    }

    if (current.contains(resource)) {
      continue;
    }

    string link = path::join(info->directory, containerPath);

    string original = paths::getPersistentVolumePath(
        flags.work_dir,
        resource.role(),
        resource.disk().persistence().id());

    LOG(INFO) << "Adding symlink from '" << original << "' to '"
              << link << "' for persistent volume " << resource
              << " of container " << containerId;

    Try<Nothing> symlink = ::fs::symlink(original, link);
    if (symlink.isError()) {
      return Failure(
          "Failed to symlink persistent volume from '" +
          original + "' to '" + link + "'");
    }
  }

  // Store the updated resources.
  info->resources = resources;

  return Nothing();
}


Future<ResourceStatistics> PosixFilesystemIsolatorProcess::usage(
    const ContainerID& containerId)
{
  // No-op, no usage gathered.

  return ResourceStatistics();
}


Future<Nothing> PosixFilesystemIsolatorProcess::cleanup(
    const ContainerID& containerId)
{
  // Symlinks for persistent resources will be removed when the work
  // directory is GC'ed.

  infos.erase(containerId);

  return Nothing();
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
