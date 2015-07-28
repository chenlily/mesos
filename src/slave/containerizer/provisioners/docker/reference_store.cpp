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

#include <stout/foreach.hpp>
#include <stout/hashset.hpp>
#include <stout/os.hpp>
#include <stout/protobuf.hpp>

#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/owned.hpp>

#include <glog/logging.h>

#include "slave/containerizer/provisioners/docker/reference_store.hpp"

using namespace process;

using std::list;
using std::string;

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

Future<Nothing> ReferenceStore::put(
    const string& name,
    const string& path,
    const JSON::Object& manifest,
    const list<string>& layers)
{
  return dispatch(
      process.get(), &ReferenceStoreProcess::put, name, path, manifest, layers);
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

  Try<Nothing> restore = referenceStore->load();
  if (restore.isError()) {
    return Error("Failed to load existing images: " + restore.error());
  }

  return referenceStore;
}

Future<Nothing> ReferenceStoreProcess::put(
    const string& name,
    const string& path,
    const JSON::Object& manifest,
    const list<string>& layers)
{
  imageCache[name] = DockerImage(name, path, manifest, layers);
  dependentLayers[layers.back()] = layers;

  ImageName imgName(name);
  repositories[imgName.repo].put(imgName.tag, layers.back());

  return save();
}

Future<Option<DockerImage>> ReferenceStoreProcess::get(const string& name)
{
  ImageName imageName(name);
  if (!repositories.contains(imageName.repo) ||
      !repositories[imageName.repo].contains(imageName.tag) ||
      !imageCache.contains(repositories[imageName.repo][imageName.tag])) {
    return None();
  }

  return imageCache[repositories[imageName.repo][imageName.tag]];
}

Try<Nothing> ReferenceStoreProcess::garbageCollect()
{
  Try<list<string>> layerList = os::ls(flags.docker_store_dir);
  if (layerList.isError()) {
    return Error("Failed to obtain a list of layers in persistent store:" +
                    layerList.error());
  }
  hashset<string> layersPresent;
  layersPresent.insert(layerList.get().begin(), layerList.get().end());

  // TODO(chenlily): Fix to use foreach.
  hashset<string> referenced;
  list<hashmap<string, string>> repos = repositories.values();
  for (auto repo = repos.begin(); repo != repos.end(); repo++) {
    list<string> layers = repo->values();
    referenced.insert(layers.begin(), layers.end());
  }

  foreach (const string& layer, layersPresent) {
    if (!referenced.contains(layer)) {
      Try<Nothing> remove = os::rm(path::join(flags.docker_store_dir, layer));
      if (remove.isError()) {
        return Error("Failed to garbage collect unreferenced directory: " +
                        remove.error());
      }
    }
  }

  return Nothing();
}

// Thin wrapper to checkpoint data to disk and perform the necessary
// error checking. It checkpoints an instance of google::protobuf::Message
// at the given path.
//
// NOTE: We provide atomic (all-or-nothing) semantics here by always
// writing to a temporary file first then use os::rename to atomically
// move it to the desired path.
Try<Nothing> ReferenceStoreProcess::checkpoint(
    const string& path,
    const google::protobuf::Message& message)
{
  // NOTE: We create the temporary file at 'docker_store_dir/XXXXXX' to make
  // sure rename below does not cross devices (MESOS-2319).
  //
  // TODO(jieyu): It's possible that the temporary file becomes
  // dangling if slave crashes or restarts while checkpointing.
  // Consider adding a way to garbage collect them.
  Try<string> temp = os::mktemp(path::join(flags.docker_store_dir, "XXXXXX"));
  if (temp.isError()) {
    return Error("Failed to create temporary file: " + temp.error());
  }

  // Now checkpoint the instance of T to the temporary file.
  // TODO(chenlily): check if need to convert DockerRepositories to a
  // ::google::protobuf::Message& message
  Try<Nothing> checkpoint = ::protobuf::write(temp.get(), message);
  if (checkpoint.isError()) {
    // Try removing the temporary file on error.
    os::rm(temp.get());

    return Error("Failed to write temporary file '" + temp.get() +
                 "': " + checkpoint.error());
  }

  // Rename the temporary file to the path.
  Try<Nothing> rename = os::rename(temp.get(), path);
  if (rename.isError()) {
    // Try removing the temporary file on error.
    os::rm(temp.get());

    return Error("Failed to rename '" + temp.get() + "' to '" +
                 path + "': " + rename.error());
  }

  return Nothing();
}

// Consider moving save to a try.
Future<Nothing> ReferenceStoreProcess::save()
{
  // Save repositories.
  Owned<DockerRepositories> dockerRepositories(new DockerRepositories);

  // Directly using 'hashmap<string,string>' in loop breaks BOOST_FOREACH macro.
  hashmap<string, string> m;
  foreachpair (string repo, m, repositories) {
    DockerRepositories_Repository* repository =
      dockerRepositories->add_repositories();
    repository->set_name(repo);

    foreachpair (string ref, string id, m) {
      DockerRepositories_Reference* reference= repository->add_references();
      reference->set_reference(ref);
      reference->set_id(id);
    }
  }

  Try<string> path = path::join(flags.docker_store_dir, "repositories");
  if (path.isError()) {
    return Failure("Failure to construct path to repositories lookup: " +
                    path.error());
  }
  Try<Nothing> status = checkpoint(path.get(), *dockerRepositories);

  if (status.isError()) {
    return Failure("Failed to perform checkpoint: " + status.error());
  }

  // Save dependencies.
  Owned<DockerLayerDependencies> dependencies(new DockerLayerDependencies);
  foreachpair(string name, list<string> layers, dependentLayers) {
    DockerLayerDependencies_Image* image = dependencies->add_images();
    image->set_name(name);
    foreach(const string& id, layers) {
      string* layer = image->add_layers();
      *layer = id;
    }
  }
  path = path::join(flags.docker_store_dir, "dependencies");
  if (path.isError()) {
    return Failure("Failure to construct path to dependencies lookup: " +
                    path.error());
  }
  status = checkpoint(path.get(), *dependencies);

  if (status.isError()) {
    return Failure("Failed to perform checkpoint: " + status.error());
  }

  return Nothing();
}


Try<Nothing> ReferenceStoreProcess::load()
{
  // Load repositories lookup.
  Try<string> path = path::join(flags.docker_store_dir, "repositories");
  if (path.isError()) {
    return Error("Failed to construct path to repositories: " + path.error());
  }

  repositories.clear();
  if (os::exists(path.get())) {
    Result<DockerRepositories> repos =
      ::protobuf::read<DockerRepositories>(path.get());
    if (repos.isError()) {
      return Error("Failed to read repositories from protobuf file: " +
                    repos.error());
    }
    for (int i = 0; i < repos.get().repositories_size(); i++) {
      hashmap<string, string> refs;
      const DockerRepositories_Repository& repo = repos.get().repositories(i);
      for (int j = 0; j < repo.references_size(); j++) {
        refs[repo.references(j).reference()] = repo.references(j).id();
      }
      repositories[repo.name()] = refs;
    }
  }

  // Load layer dependencies.
  path = path::join(flags.docker_store_dir, "dependencies");
  if (path.isError()) {
    return Error("Failed to construct path to dependencies: " + path.error());
  }

  dependentLayers.clear();
  if (os::exists(path.get())) {
    Result<DockerLayerDependencies> dependencies =
      ::protobuf::read<DockerLayerDependencies>(path.get());
    if (dependencies.isError()) {
      return Error("Failed to obtain docker image layer dependencies:" +
                    dependencies.error());
    }

    for (int i = 0; i < dependencies.get().images_size(); i++) {
      list<string> layers;
      for (int j = 0; dependencies.get().images(i).layers_size(); j++) {
        layers.push_back(dependencies.get().images(i).layers(j));
      }
      dependentLayers[layers.back()] = layers;
    }
  }

  // Restore imageCache.
  foreachpair (const string& name, const list<string> layers, dependentLayers) {
    bool missingLayer = false;
    foreach (const string& layer, layers) {
      if (!os::exists(path::join(flags.docker_store_dir, layer, "rootfs"))) {
        missingLayer = true;
      }
    }
    if (!missingLayer) {
      // TODO(chenlily): If manifest is available, restore manifest to
      // cached image.
      imageCache[name] = DockerImage(
          name,
          path::join(flags.docker_store_dir, layers.back()),
          None(),
          layers);
    }
  }

  return garbageCollect();
}

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
