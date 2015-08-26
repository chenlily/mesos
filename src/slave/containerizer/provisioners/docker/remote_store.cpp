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

#include "slave/containerizer/provisioners/docker/remote_store.hpp"

#include <list>

#include <glog/logging.h>

#include <stout/json.hpp>
#include <stout/os.hpp>
#include <stout/stringify.hpp>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/http.hpp>

#include "slave/containerizer/provisioners/docker/reference_store.hpp"
#include "slave/containerizer/provisioners/docker/registry_client.hpp"

using namespace process;

using std::list;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace slave {
namespace docker {

class RemoteStoreProcess : public Process<RemoteStoreProcess>
{
public:
  ~RemoteStoreProcess() {}

  static Try<Owned<RemoteStoreProcess>> create(
      const Flags& flags,
      Fetcher* fetcher);

  Future<Option<DockerImage>> get(
      const string& name);

  Future<DockerImage> put(
      const string& name,
      const string& sandbox);

private:
  RemoteStoreProcess(
      const Flags& flags,
      Fetcher* fetcher,
      const Owned<ReferenceStore>& refStore,
      const Owned<RegistryClient>& registryClient);

  Future<JSON::Object> fetchManifest(const ImageName& imageName);

  Future<list<string>> collectLayers(
      const string& repo,
      const JSON::Object& manifest);

  Future<string> putLayer(
      const string& repo,
      const string& digest,
      const string& layerId);

  Future<string> getLayer(
      const string& repo,
      const string& digest,
      const string& layerId);

  const Flags flags;
  Fetcher* fetcher;

  Owned<ReferenceStore> refStore;
  Owned<RegistryClient> registryClient;
};


Try<Owned<Store>> RemoteStore::create(
    const Flags& flags,
    Fetcher* fetcher)
{
  Try<Owned<RemoteStoreProcess>> process =
    RemoteStoreProcess::create(flags, fetcher);
  if (process.isError()) {
    return Error("Failed to create store: " + process.error());
  }

  return Owned<Store>(new RemoteStore(process.get()));
}


RemoteStore::RemoteStore(Owned<RemoteStoreProcess> process)
  : process(process)
{
  process::spawn(CHECK_NOTNULL(process.get()));
}


RemoteStore::~RemoteStore()
{
  process::terminate(process.get());
  process::wait(process.get());
}


Future<DockerImage> RemoteStore::put(
    const string& name,
    const string& sandbox)
{
  return dispatch(process.get(), &RemoteStoreProcess::put, name, sandbox);
}


Future<Option<DockerImage>> RemoteStore::get(const string& name)
{
  return dispatch(process.get(), &RemoteStoreProcess::get, name);
}


Try<Owned<RemoteStoreProcess>> RemoteStoreProcess::create(
    const Flags& flags,
    Fetcher* fetcher)
{
  Try<Owned<ReferenceStore>> refStore = ReferenceStore::create(flags);
  if (refStore.isError()) {
    return Error("Error in creating ReferenceStore: " + refStore.error());
  }

  // TODO(chenlily): Pass through credentials.
  // TODO(chenlily): Auth server should be determined by the remote registry
  //                 server. Change this when Jojy modifies registry client
  //                 to reflect this.
  // TODO(chenlily): Implement parsing of url strings passed in as flags
  //                 into actual URLs.
  // TODO(chenlily): Fix registry client create static, instantiation of
  //                 member variable issues.
  Try<Owned<RegistryClient>> registryClient;
  /*
   RegistryClient::create(
      http::URL(),
      http::URL(),
      None());
      //http::URL("https", flags.docker_remote_auth_server, 80, "/",
                  hashmap<string, string>(), None()),
      //http::URL("https", flags.docker_remote_registry_server),
      //None());
    */
  if (registryClient.isError()) {
    return Error("Error in creating registryClient: " + registryClient.error());
  }

  return Owned<RemoteStoreProcess>(new RemoteStoreProcess(
      flags,
      fetcher,
      refStore.get(),
      registryClient.get()));
}


RemoteStoreProcess::RemoteStoreProcess(
    const Flags& flags,
    Fetcher* fetcher,
    const Owned<ReferenceStore>& refStore,
    const Owned<RegistryClient>& registryClient)
  : flags(flags),
    fetcher(fetcher),
    refStore(refStore),
    registryClient(registryClient) {}


Future<DockerImage> RemoteStoreProcess::put(
    const string& name,
    const string& sandbox)
{
  ImageName imageName(name);
  return fetchManifest(imageName);
    .then(defer(self(), &Self::collectLayers, imageName.repo, lambda::_1));
    .then([name](const Future<list<string>>& layers){
      if (layers.isDiscarded()) {
        return Failure("Failed to retrieve image layers (future discarded).");
      }
      if (layers.isFailed()) {
        return Failure(layers.failure());
      }
      refStore->put(name, layers);
    });
}


Future<JSON::Object> RemoteStoreProcess::fetchManifest(
    const ImageName& imageName)
{
  return registryClient->getManifest(imageName.repo, imageName.tag)
    .then([=](
        const Future<ManifestResponse>& manifest) -> Future<JSON::Object> {
      if (manifest.isError()) {
        return Failed("Failed to fetch Docker image manifest: " +
                      manifest.failure());
      }
      return manifest.get().responseJSON;
    });
}

// TODO(chenlily): Add a protobuf serializing (version 2) of the Docker image
//                 manifest to validate the object returned. Protobuf portion
//                 may possible go inside RegistryClient.
Future<list<string>> RemoteStoreProcess::collectLayers(
    const string& repo,
    const JSON::Object& manifest)
{
  Result<JSON::Array> fsLayers = manifest.find<JSON::Array>("fsLayers");
  if (!fsLayers.isSome()) {
    return Failure("No fsLayers in manifest for image");
  }

  Result<JSON::Array> history = manifest.find<JSON::Array>("history");
  if (!history.isSome()) {
    return Failure("No history in manifest for image");
  }

  if (fsLayers.get().values.size() != history.get().values.size()) {
    return Failure("Length of history does not match number of image layers");
  }

  list<Future<string>> futures;
  for (size_t i = 0; i < fsLayers.get().values.size(); i++) {
    Result<JSON::String> tarsum =
      manifest.find<JSON::String>("fsLayers[" + stringify(i) + "].blobSum");
    if (!tarsum.isSome()) {
      return Failure("Failed to obtain blobsum for layer");
    }

    Result<JSON::String> v1Str =
      manifest.find<JSON::String>("history[" + stringify(i) + "]");
    if (!v1Str.isSome()) {
      return Failure("Failed to obtain layer v1 compatbility in manifest");
    }
    Try<JSON::Object> v1Compatibility =
      JSON::parse<JSON::Object>(v1Str.get().value);

    if (!v1Compatibility.isSome()) {
      return Failure("Failed to obtain layer v1 compability json in manifest");
    }
    Result<JSON::String> id = v1Compatibility.get().find<JSON::String>("id");

    // TODO(chenlily): Parallelize the fetching of layers.
    futures.push_back(putLayer(tarsum.get().value, id.get().value, repo));
  }

  return collect(futures);
}


Future<string> RemoteStoreProcess::putLayer(
    const string& repo,
    const string& digest,
    const string& layerId)
{
  // Check if layer already exists on disk.
  if(os::exists(paths::getImageLayerPath(flags.docker_store_dir, layerId))) {
    return layerId;
  }

  // TODO(chenlily): Check if there is already a download in progress.
  return getLayer(repo, digest, layerId);
}


Future<string> RemoteStoreProcess::getLayer(
    const string& repo,
    const string& digest,
    const string& layerId)
{
  Try<string> staging = os::mkdtemp();
  if (staging.isError()) {
    return Failure("Failed to create staging directory for layer download:" +
                    staging.Error());
  }

  // TODO(chenlily): Pass through timeout and maxsize (configurable through
  //                 flags?).
  return registryClient->getBlob(
      repo,
      digest,
      layerId,
      RegistryClient::DEFAULT_MANIFEST_TIMEOUT_SECS,
      RegistryClient::DEFAULT_MANIFEST_MAXSIZE_BYTES)
    .then([=](const Future<size_t>& blobSize) -> Future<string> {
      if (blobSize.isFailed()) {
        return Failure(blobSize.failure());
      }
      if (blobSize.isDiscarded()) {
        return Failure("Failed to download layer: " + layerId +
                      "(discarded future).");
      }
      // TODO(chenlily): verify returned blob size with downloaded object.
      return layerId;
    });
}


Future<Option<DockerImage>> RemoteStoreProcess::get(const string& name)
{
  return refStore->get(name);
}

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
