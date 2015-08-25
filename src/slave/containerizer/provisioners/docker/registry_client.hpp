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

#ifndef __PROVISIONERS_DOCKER_REGISTRY_CLIENT_HPP__
#define __PROVISIONERS_DOCKER_REGISTRY_CLIENT_HPP__

#include <process/http.hpp>
#include <process/future.hpp>
#include <process/process.hpp>

#include <stout/hashmap.hpp>
#include <stout/duration.hpp>
#include <stout/json.hpp>
#include <stout/path.hpp>

namespace mesos {
namespace internal {
namespace slave {
namespace provisioners {
namespace docker {
namespace registry {


// Forward declaration.
class RegistryClientProcess;
class TokenManager;

// TODO(jojy): Move this somewhere common.
template <typename T>
T getOptionalValue(const Option<T>& option, const T& defaultValue)
{
  if (option.isNone()) {
    return defaultValue;
  }

  return option.get();
}


class RegistryClient
{
public:
  /**
   * Encapsulates response of "GET Mannifest" request.
   *
   * Reference: https://docs.docker.com/registry/spec/api
   */
  struct ManifestResponse {
    const std::string digest;
    const JSON::Object responseJSON;
  };

  /**
   * Encapsulates auth credentials for the client sessions.
   * TODO(jojy): Secure heap to protect the credentials.
   */
  struct Credentials {
    const Option<std::string> userid; /* userid for basic authentication.  */
    const Option<std::string> password; /* password for basic authentication. */
    const Option<std::string> account; /* account for fetching data from
                                          registry. */
  };

  /**
   * Factory method for creating RegistryClient objects.
   *
   * @param authServer URL of authorization server.
   * @param registryServer URL of docker registry server.
   * @param creds credentials for client session (optional).
   * @return RegistryClient on Sucess.
   *         Error on failure.
   */
  static Try<process::Owned<RegistryClient>> create(
      const process::http::URL& authServer,
      const process::http::URL& registryServer,
      const Option<Credentials>& creds);

  /**
   * Fetches manifest for a repository from the client's remote registry server.
   *
   * @param path path of the repository on the registry.
   * @param tag unique tag that identifies the repository. Will default to
   *    latest.
   * @param timeOut Maximum time ater which the request will timeout and return
   *    a failure. Will default to RESPONSE_TIMEOUT.
   * @return JSON object on success.
   *         Failure on process failure.
   */
  process::Future<ManifestResponse> getManifest(
      const std::string& path,
      const Option<std::string>& tag,
      const Option<Duration>& timeOut);

  /**
   * Fetches blob for a repository from the client's remote registry server.
   *
   * @param path path of the repository on the registry.
   * @param digest digest of the blob (from manifest).
   * @param filePath file path to store the fetched blobs.
   * @param timeOut Maximum time ater which the request will timeout and return
   *    a failure. Will default to RESPONSE_TIMEOUT.
   * @param maxSize Maximum size of the response thats acceptable. Will default
   *    to MAX_RESPONSE_SIZE.
   * @return size of downloaded blob on success.
   *         Failure in case of any errors.
   */
  process::Future<size_t> getBlob(
      const std::string& path,
      const Option<std::string>& digest,
      const Path& filePath,
      const Option<Duration>& timeOut,
      const Option<size_t>& maxSize);

  ~RegistryClient();

private:
  const static Duration DEFAULT_MANIFEST_TIMEOUT_SECS;
  const static size_t DEFAULT_MANIFEST_MAXSIZE_BYTES;

  const process::http::URL authServer_;
  const process::http::URL registryServer_;
  const Option<Credentials> credentials_;
  process::Owned<RegistryClientProcess> process_;

  RegistryClient(
    const process::http::URL& authServer,
    const process::http::URL& registryServer,
    const Option<Credentials>& creds,
    const process::Owned<RegistryClientProcess>& process);

  RegistryClient(const RegistryClient&) = delete;
  RegistryClient& operator = (const RegistryClient&) = delete;
};


class RegistryClientProcess :
  public process::Process<RegistryClientProcess>
{
public:
  static Try<process::Owned<RegistryClientProcess>> create(
      const process::http::URL& authServer,
      const process::http::URL& regiistry,
      const Option<RegistryClient::Credentials>& creds);

  process::Future<RegistryClient::ManifestResponse> getManifest(
      const std::string& path,
      const Option<std::string>& tag,
      const Duration& timeOut);

  process::Future<size_t> getBlob(
      const std::string& path,
      const Option<std::string>& digest,
      const Path& filePath,
      const Duration& timeOut,
      const size_t& maxSize);

private:
  process::Owned<TokenManager> tokenManager_;
  const process::http::URL registryServer_;
  const Option<RegistryClient::Credentials> credentials_;

  RegistryClientProcess(
    const process::Owned<TokenManager>& tokenMgr,
    const process::http::URL& registryServer,
    const Option<RegistryClient::Credentials>& creds);

  process::Future<process::http::Response> doHttpGet(
      const process::http::URL& url,
      const Option<hashmap<std::string, std::string>>& headers,
      const Duration& timeout,
      bool resend) const;

  Try<hashmap<std::string, std::string>> getAuthenticationAttributes(
      const process::http::Response& httpResponse) const;

  RegistryClientProcess(const RegistryClientProcess&) = delete;
  RegistryClientProcess& operator = (const RegistryClientProcess&) = delete;
};

} // namespace registry {
} // namespace docker {
} // namespace provisioners {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __PROVISIONERS_DOCKER_REGISTRY_CLIENT_HPP__
