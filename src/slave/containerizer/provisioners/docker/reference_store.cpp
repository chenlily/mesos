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

#include <vector>

#include <glog/logging.h>

#include <stout/foreach.hpp>
#include <stout/hashset.hpp>
#include <stout/os.hpp>
#include <stout/protobuf.hpp>

#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/owned.hpp>

#include "messages/docker_provisioner.hpp"

#include "slave/containerizer/provisioners/docker/paths.hpp"
#include "slave/containerizer/provisioners/docker/reference_store.hpp"
#include "slave/state.hpp"

using namespace process;

using std::list;
using std::string;
using std::vector;
using namespace mesos::internal::slave::state;

namespace mesos {
namespace internal {
namespace slave {
namespace docker {

Try<Owned<ReferenceStore>> ReferenceStore::create(const Flags& flags)
{
  Try<Owned<ReferenceStoreProcess>> process =
    ReferenceStoreProcess::create(flags);

  if (process.isError()) {
    return Error("Failed to create reference store: " + process.error());
  }
  return Owned<ReferenceStore>(new ReferenceStore(process.get()));
}

ReferenceStore::ReferenceStore(Owned<ReferenceStoreProcess> process)
  : process(process)
{
  process::spawn(CHECK_NOTNULL(process.get()));
}

ReferenceStore::~ReferenceStore()
{
  process::terminate(process.get());
  process::wait(process.get());
}

void ReferenceStore::initialize()
{
  process::dispatch(process.get(), &ReferenceStoreProcess::initialize);
}

Future<DockerImage> ReferenceStore::put(
    const string& name,
    const list<string>& layers)
{
  return dispatch(
      process.get(), &ReferenceStoreProcess::put, name, layers);
}

Future<Option<DockerImage>> ReferenceStore::get(const string& name)
{
  return dispatch(process.get(), &ReferenceStoreProcess::get, name);
}


ReferenceStoreProcess::ReferenceStoreProcess(const Flags& flags)
  : flags(flags) {}

Try<Owned<ReferenceStoreProcess>> ReferenceStoreProcess::create(
    const Flags& flags)
{
  Owned<ReferenceStoreProcess> referenceStore =
    Owned<ReferenceStoreProcess>(new ReferenceStoreProcess(flags));

  return referenceStore;
}


Future<DockerImage> ReferenceStoreProcess::put(
    const string& name,
    const list<string>& layers)
{
  storedImages[name] = DockerImage(name, layers);

  Try<Nothing> status = persist();
  if (status.isError()) {
    return Failure("Failed to save state of Docker images" + status.error());
  }

  return storedImages[name];
}


Future<Option<DockerImage>> ReferenceStoreProcess::get(const string& name)
{
  if (!storedImages.contains(name)) {
    return None();
  }

  return storedImages[name];
}


Try<Nothing> ReferenceStoreProcess::persist()
{
  DockerProvisionerImages images;

  foreachpair(
      const string& name, const DockerImage& dockerImage, storedImages) {
    DockerProvisionerImages::Image* image = images.add_images();

    image->set_name(name);

    foreach (const string& layer, dockerImage.layers) {
      image->add_layer_ids(layer);
    }
  }

  Try<Nothing> status =
    checkpoint(paths::getStoredImagesPath(flags.docker_store_dir), images);
  if (status.isError()) {
    return Error("Failed to perform checkpoint: " + status.error());
  }

  return Nothing();
}


void ReferenceStoreProcess::initialize()
{
  string storedImagesPath = paths::getStoredImagesPath(flags.docker_store_dir);

  storedImages.clear();
  if (!os::exists(storedImagesPath)) {
    LOG(INFO) << "No images to load from disk. Docker provisioner image "
              << "storage path: " << storedImagesPath << " does not exist.";
    return;
  }

  Result<DockerProvisionerImages> images =
    ::protobuf::read<DockerProvisionerImages>(storedImagesPath);

  for (int i = 0; i < images.get().images_size(); i++) {
    string imageName = images.get().images(i).name();

    list<string> layers;
    vector<string> missingLayers;
    for (int j = 0; j < images.get().images(i).layer_ids_size(); j++) {
      string layerId = images.get().images(i).layer_ids(j);

      layers.push_back(layerId);

      if (!os::exists(
              paths::getImageLayerPath(flags.docker_store_dir, layerId))) {
        missingLayers.push_back(layerId);
      }
    }

    if (!missingLayers.empty()) {
      foreach (const string& layer, missingLayers) {
        LOG(WARNING) << "Image layer: " << layer << " required for Docker "
                     << "image: " << imageName << " is not on disk.";
      }
      LOG(WARNING) << "Did not load image: " << imageName << " from disk.";
      continue;
    }

    VLOG(1) << "Obtained Docker image: " << imageName << " from disk.";
    storedImages[imageName] = DockerImage(imageName, layers);
  }
}

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
