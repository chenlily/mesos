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

#include <stout/os.hpp>
#include <stout/json.hpp>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/dispatch.hpp>

#include <glog/logging.h>

#include "slave/containerizer/fetcher.hpp"

#include "slave/containerizer/provisioners/docker/store.hpp"

using namespace process;

using std::list;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace slave {
namespace docker {


Try<Owned<Store>> Store::create(
    const Flags& flags,
    Fetcher* fetcher)
{
  hashmap<string, Try<Owned<Store>>(*)(const Flags&, Fetcher*)> creators{
    {"local", &LocalStore::create}
  };

  if (!creators.contains(flags.docker_store)) {
    return Error("Unknown or unsupported image retrieval");
  }

  return creators[flags.docker_store](flags, fetcher);
}

Try<Owned<Store>> LocalStore::create(
    const Flags& flags,
    Fetcher* fetcher)
{
  Try<Owned<LocalStoreProcess>> process =
    LocalStoreProcess::create(flags, fetcher);
  if (process.isError()) {
    return Error("Failed to create store: " + process.error());
  }

  return Owned<Store>(new LocalStore(process.get()));
}


LocalStore::LocalStore(Owned<LocalStoreProcess> process)
  : process(process)
{
  process::spawn(CHECK_NOTNULL(process.get()));
}


LocalStore::~LocalStore()
{
  process::terminate(process.get());
  process::wait(process.get());
}


Future<DockerImage> LocalStore::put(
    const string& name,
    const string& sandbox)
{
  return dispatch(process.get(), &LocalStoreProcess::put, name, sandbox);
}


Future<Option<DockerImage>> LocalStore::get(const string& name)
{
  return dispatch(process.get(), &LocalStoreProcess::get, name);
}

Try<Owned<LocalStoreProcess>> LocalStoreProcess::create(
    const Flags& flags,
    Fetcher* fetcher)
{
  Owned<LocalStoreProcess> store =
    Owned<LocalStoreProcess>(new LocalStoreProcess(flags, fetcher));

  Try<Nothing> restore = store->restore();
  if (restore.isError()) {
    return Error("Failed to restore store: " + restore.error());
  }

  return store;
}

LocalStoreProcess::LocalStoreProcess(
    const Flags& flags,
    Fetcher* fetcher)
  : flags(flags),
    fetcher(fetcher) {}

Future<DockerImage> LocalStoreProcess::put(
    const string& name,
    const string& sandbox)
{
  // TODO(chenlily): Check in reference store if image manifest and layers
  // are already present.

  Try<string> path = path::join(flags.docker_discovery_local_dir, name);
  if (path.isError()) {
    return Failure(path.error());
  }
  string imagePath = path.get();

  if(!os::exists(imagePath) && os::stat::isfile(imagePath)) {
    os::rm(imagePath);
  }

  if (!os::exists(imagePath)) {
    os::mkdir(imagePath);
  }

  string tarPath = imagePath + ".tar";
  if (!os::exists(tarPath)) {
    return Failure("No Docker image directory or tar archive");
  }

  LOG(INFO) << "Untarring image at " + tarPath;

  return untarImage(tarPath, imagePath)
    .then(defer(self(), &Self::putImage, name, imagePath, sandbox));
}

Future<Nothing> LocalStoreProcess::untarImage(
    const string& tarPath,
    const string& imagePath)
{
  if (!os::exists(imagePath)) {
    os::mkdir(imagePath);
  }

  // Untar imagePath/imgName.tar into imagePath/image/.
  vector<string> argv = {
    "tar",
    "-C",
    imagePath,
    "-x",
    "-f",
    tarPath
  };

  Try<Subprocess> s = subprocess(
      "tar",
      argv,
      Subprocess::PATH("/dev/null"),
      Subprocess::PATH("/dev/null"),
      Subprocess::PATH("/dev/null"));

  if (s.isError()) {
    return Failure("Failed to create tar subprocess: " + s.error());
  }

  return s.get().status()
    .then([=](const Option<int>& status) -> Future<Nothing> {
      if (status.isNone()) {
        return Failure("Failed to reap status for tar subprocess in " +
                        imagePath);
      }
      if (status.isSome() && status.get() != 0) {
        return Failure("Non-zero exit for tar subprocess: " +
                        stringify(status.get()) + " in " + imagePath);
      }
      return Nothing();
    });
}


Future<DockerImage> LocalStoreProcess::putImage(
    const string& name,
    const string& imagePath,
    const string& sandbox)
{
  ImageName imageName(name);
  string repository = imageName.repo;
  string tag = imageName.tag;

  // Read repository json
  Try<string> repoPath = path::join(imagePath, "repositories");
  if (repoPath.isError()) {
    return Failure("Failed to create path to repository: " + repoPath.error());
  }

  Try<string> value = os::read(repoPath.get());
  if (value.isError()) {
    return Failure("Failed to read repository JSON: " + value.error());
  }

  Try<JSON::Object> json = JSON::parse<JSON::Object>(value.get());
  if (json.isError()) {
    return Failure("Failed to parse JSON: " + json.error());
  }

  Result<JSON::Object> repositoryValue =
    json.get().find<JSON::Object>(imageName.repo);
  if (repositoryValue.isError()) {
    return Failure("Failed to find repository: " + repositoryValue.error());
  } else if (repositoryValue.isNone()) {
    return Failure("Repository '" + imageName.repo + "' is not found");
  }

  JSON::Object repositoryJson = repositoryValue.get();

  // We don't use JSON find here because a tag might contain a '.'.
  std::map<string, JSON::Value>::const_iterator entry =
    repositoryJson.values.find(imageName.tag);
  if (entry == repositoryJson.values.end()) {
    return Failure("Tag '" + imageName.tag + "' is not found");
  } else if (!entry->second.is<JSON::String>()) {
    return Failure("Tag json value expected to be JSON::String");
  }

  Try<string> layerPath = path::join(
      imagePath,
      entry->second.as<JSON::String>().value);

  string layerId = entry->second.as<JSON::String>().value;

  if (layerPath.isError()) {
    return Failure("Failed to create path to image layer: " +
                    layerPath.error());
  }

  Try<string> manifest = os::read(path::join(imagePath, layerId, "json"));
  if (manifest.isError()) {
    return Failure("Failed to read manifest: " + manifest.error());
  }

  Try<JSON::Object> manifestJson = JSON::parse<JSON::Object>(manifest.get());
  if (manifestJson.isError()) {
    return Failure("Failed to parse manifest: " + manifestJson.error());
  }

  list<string> layers;
  layers.push_back(layerId);

  Result<string> parentId = getParentId(imagePath, layerId);
  while(parentId.isSome()) {
    layers.push_front(parentId.get());
    parentId = getParentId(imagePath, parentId.get());
  }
  if (parentId.isError()) {
    return Failure("Failed to obtain parent layer id: " + parentId.error());
  }

  return putLayers(imagePath, layers, sandbox)
    .then([=]() -> Future<DockerImage> {
      images[name] = DockerImage(
          name,
          flags.docker_store_dir,
          manifestJson.get(),
          layers);

      // TODO(chenlily): update reference store or replace with reference store
      return images[name];
    });
}

Result<string> LocalStoreProcess::getParentId(
    const string& imagePath,
    const string& layerId)
{
  Try<string> manifest = os::read(path::join(imagePath, layerId, "json"));
  if (manifest.isError()) {
    return Error("Failed to read manifest: " + manifest.error());
  }

  Try<JSON::Object> json = JSON::parse<JSON::Object>(manifest.get());
  if (json.isError()) {
    return Error("Failed to parse manifest: " + json.error());
  }

  Result<JSON::String> parentId = json.get().find<JSON::String>("parent");
  if (parentId.isNone()) {
    return None();
  } else if (parentId.isError()) {
    return Error("Failed to read parent of layer: " + parentId.error());
  }
  return parentId.get().value;
}


Future<Nothing> LocalStoreProcess::putLayers(
    const string& imagePath,
    const list<string>& layers,
    const string& sandbox)
{
  list<Future<Nothing>> futures{ Nothing() };
  foreach (const string& layer, layers) {
    futures.push_back(
        futures.back().then(
          defer(self(), &Self::untarLayer, imagePath, layer, sandbox)));
  }

  return collect(futures)
    .then([]() -> Future<Nothing> { return Nothing(); });
}

Future<Nothing> LocalStoreProcess::untarLayer(
    const string& imagePath,
    const string& id,
    const string& sandbox)
{
  if (!os::exists(path::join(imagePath, id, "rootfs"))) {
    os::mkdir(path::join(imagePath, id, "rootfs"));
  }
  // Untar imagePath/id/layer.tar into imagePath/id/rootFs.
  vector<string> argv = {
    "tar",
    "-C",
    path::join(imagePath, id, "rootfs"),
    "-x",
    "-f",
    path::join(imagePath, id, "layer.tar")
  };

  Try<Subprocess> s = subprocess(
      "tar",
      argv,
      Subprocess::PATH("/dev/null"),
      Subprocess::PATH("/dev/null"),
      Subprocess::PATH("/dev/null"));

  if (s.isError()) {
    return Failure("Failed to create tar subprocess: " + s.error());
  }

  return s.get().status()
    .then([=](const Option<int>& status) -> Future<Nothing> {
      Try<string> layerPath = path::join(imagePath, id, "rootfs");
      if (status.isNone()) {
        return Failure("Failed to reap status for tar subprocess in " +
                        layerPath.get());
      }
      if (status.isSome() && status.get() != 0) {
        return Failure("Non-zero exit for tar subprocess: " +
                        stringify(status.get()) + " in " + layerPath.get());
      }
      return copyLayer(imagePath, id, sandbox);
    });
}

Future<Nothing> LocalStoreProcess::copyLayer(
    const string& imagePath,
    const string& id,
    const string& sandbox)
{
  Try<int> out = os::open(
      path::join(sandbox, "stdout"),
      O_WRONLY | O_CREAT | O_TRUNC | O_NONBLOCK | O_CLOEXEC,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  if (out.isError()) {
    return Failure("Failed to create 'stdout' file: " + out.error());
  }

  // Repeat for stderr.
  Try<int> err = os::open(
      path::join(sandbox, "stderr"),
      O_WRONLY | O_CREAT | O_TRUNC | O_NONBLOCK | O_CLOEXEC,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  if (err.isError()) {
    os::close(out.get());
    return Failure("Failed to create 'stderr' file: " + err.error());
  }

  Try<string> storePath = path::join(flags.docker_store_dir, id);
  if (storePath.isError()) {
    return Failure("Failed to construct image storePath: " + storePath.error());
  }

  if (os::exists(storePath.get())) {
    os::rm(storePath.get());
  }
  os::mkdir(storePath.get());

  vector<string> argv = {
    "cp",
    "--archive",
    path::join(imagePath, id),
    flags.docker_store_dir
  };

  VLOG(1) << "Copying image with command: " << strings::join(" ", argv);

  Try<Subprocess> s = subprocess(
      "cp",
      argv,
      Subprocess::PATH("/dev/null"),
      Subprocess::FD(out.get()),
      Subprocess::FD(err.get()));

  if (s.isError()) {
    return Failure("Failed to create 'cp' subprocess: " + s.error());
  }

  return s.get().status()
    .then([=](const Option<int>& status) -> Future<Nothing> {
      if (status.isNone()) {
        return Failure("Failed to reap subprocess to copy image");
      } else if (status.get() != 0) {
        return Failure("Non-zero exit from subprocess to copy image: " +
                      stringify(status.get()));
      }

      // TODO(chenlily): Remove layer.tar from directory.
      return Nothing();
    });
}


Future<Option<DockerImage>> LocalStoreProcess::get(const string& name)
{
  if (!images.contains(name)) {
    return None();
  }

  return images[name];
}


// Recover stored image layers and update layers map.
// TODO(chenlily): Implement restore.
Try<Nothing> LocalStoreProcess::restore()
{
  return Nothing();
}

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
