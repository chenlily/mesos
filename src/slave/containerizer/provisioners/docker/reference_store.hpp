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

#ifndef __MESOS_DOCKER_REFERENCE_STORE__
#define __MESOS_DOCKER_REFERENCE_STORE__

#include <list>
#include <string>

#include <process/future.hpp>

#include <process/owned.hpp>

#include <stout/hashmap.hpp>
#include <stout/json.hpp>
#include <stout/protobuf.hpp>

#include "slave/containerizer/provisioners/docker.hpp"
#include "slave/flags.hpp"

namespace mesos {
namespace internal {
namespace slave {
namespace docker {

// Forward Declaration.
class ReferenceStoreProcess;

class ReferenceStore
{
public:
  ~ReferenceStore();

  static Try<process::Owned<ReferenceStore>> create(const Flags& flags);

  process::Future<Nothing> put(
      const std::string& name,
      const std::string& path,
      const JSON::Object& manifest,
      const std::list<std::string>& layers);

  // Retrieve Docker Image based on image name.
  process::Future<Option<DockerImage>> get(const std::string& name);
private:
  explicit ReferenceStore(process::Owned<ReferenceStoreProcess> process);

  ReferenceStore(const ReferenceStore&); // Not copyable.
  ReferenceStore& operator=(const ReferenceStore&); // Not assignable.

  process::Owned<ReferenceStoreProcess> process;
};

class ReferenceStoreProcess : public process::Process<ReferenceStoreProcess>
{
public:
  ~ReferenceStoreProcess() {}

  static Try<process::Owned<ReferenceStoreProcess>> create(const Flags& flags);

  process::Future<Nothing> put(
      const std::string& name,
      const std::string& path,
      const JSON::Object& manifest,
      const std::list<std::string>& layers);

  process::Future<Option<DockerImage>> get(const std::string& name);

private:
  ReferenceStoreProcess(const Flags& flags);
  // Write out repositories to persistent store.
  process::Future<Nothing> save();

  Try<Nothing> garbageCollect();
  // Update repositories based on state in persistent store.
  Try<Nothing> load();
  Try<Nothing> checkpoint(
      const std::string& path,
      const google::protobuf::Message& message);

  const Flags flags;

  // This is keyed by the repository name and its value will be another map that
  // is then keyed by the reference (a tag or blobsum digest) to a layer's id.
  hashmap<std::string, hashmap<std::string, std::string>> repositories;

  // This is keyed by a layer id and maps to a list of dependent layer Ids.
  // This may not be necessary if you only support recovery with images that
  // have version 2 of the image manifest.
  hashmap<std::string, std::list<std::string>> dependentLayers;

  // This is keyed by leaf layer id and maps to a corresponding Docker Image.
  hashmap<std::string, DockerImage> imageCache;
};

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __MESOS_DOCKER_REFERENCE_STORE__
