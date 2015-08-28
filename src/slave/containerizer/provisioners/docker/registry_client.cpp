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

#include <process/defer.hpp>
#include <process/dispatch.hpp>

#include "slave/containerizer/provisioners/docker/registry_client.hpp"
#include "slave/containerizer/provisioners/docker/token_manager.hpp"

using namespace process;
using namespace process::http;

using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace slave {
namespace docker {

using ManifestResponse = RegistryClient::ManifestResponse;


const Duration RegistryClient::DEFAULT_MANIFEST_TIMEOUT_SECS = Seconds(10);

const size_t RegistryClient::DEFAULT_MANIFEST_MAXSIZE_BYTES = 4096;


Try<Owned<RegistryClient>> RegistryClient::create(
    const URL& authServer,
    const URL& registryServer,
    const Option<Credentials>& creds)
{
  Try<Owned<RegistryClientProcess>> process =
    RegistryClientProcess::create(authServer, registryServer, creds);

  if (process.isError()) {
    return Error(process.error());
  }

  return Owned<RegistryClient>(
      new RegistryClient(authServer, registryServer, creds, process.get()));
}


RegistryClient::RegistryClient(
    const URL& authServer,
    const URL& registryServer,
    const Option<Credentials>& creds,
    const Owned<RegistryClientProcess>& process)
  : authServer_(authServer),
    registryServer_(registryServer),
    credentials_(creds),
    process_(process)
{
   spawn(CHECK_NOTNULL(process_.get()));
}


RegistryClient::~RegistryClient()
{
  terminate(process_.get());
  process::wait(process_.get());
}


Future<ManifestResponse> RegistryClient::getManifest(
    const string& _path,
    const Option<string>& _tag,
    const Option<Duration>& _timeOut)
{
  Duration timeOut = getOptionalValue(_timeOut, DEFAULT_MANIFEST_TIMEOUT_SECS);

  Owned<Promise<ManifestResponse>> promise(new Promise<ManifestResponse>());

  Future<ManifestResponse> result =
    dispatch(
        process_.get(),
        &RegistryClientProcess::getManifest,
        _path,
        _tag,
        timeOut);

  result.onFailed([promise](const string& failure) {
      promise->fail(failure);
      return Failure(failure);
  });

  result
    .onAny([this, promise] (
        const Future<ManifestResponse>& response) -> Future<ManifestResponse> {
      if (response.isFailed()) {
        promise->fail(response.failure());
        LOG(WARNING) << "Failed to get manifest: " << response.failure();

        return Failure(response.failure());
      }

      promise->associate(response);
      return promise->future();
    });

  return promise->future();
}


Future<size_t> RegistryClient::getBlob(
    const std::string& _path,
    const Option<std::string>& _digest,
    const Path& _filePath,
    const Option<Duration>& _timeOut,
    const Option<size_t>& _maxSize)
{
  Duration timeOut = getOptionalValue(_timeOut, DEFAULT_MANIFEST_TIMEOUT_SECS);
  size_t maxSize = getOptionalValue(_maxSize, DEFAULT_MANIFEST_MAXSIZE_BYTES);

  Owned<Promise<size_t>> promise(new Promise<size_t>());

  auto prepare = ([&_filePath, promise]() -> Try<Nothing> {
      const string dirName = _filePath.dirname();

      //TODO (jojy): Return more state, for example - if the directory is new.
      Try<Nothing> dirResult = os::mkdir(dirName, true);
      if (dirResult.isError()) {
        return Error("failed to create directory to download blob: " +
           dirResult.error());
      }

      return dirResult;
  })();

  // TODO(jojy): This currently leaves a residue in failure cases. Would be
  // ideal if we can completely rollback.
  if (prepare.isError()) {
     return Failure(prepare.error());
  }

  Future<size_t> result =
    dispatch(
        process_.get(),
        &RegistryClientProcess::getBlob,
        _path,
        _digest,
        _filePath,
        timeOut,
        maxSize);

  result.onFailed([promise](const string& failure) {
      promise->fail(failure);
      return Failure(failure);
  });

  result
    .onAny([this, promise] (
        const Future<size_t>& response) -> Future<size_t> {
      if (response.isFailed()) {
        promise->fail(response.failure());
        LOG(WARNING) << "Failed to get blob: " << response.failure();

        return Failure(response.failure());
      }

      promise->associate(response);
      return promise->future();
    });

  return promise->future();
}


Try<Owned<RegistryClientProcess>> RegistryClientProcess::create(
    const URL& authServer,
    const URL& registryServer,
    const Option<RegistryClient::Credentials>& creds)
{
  Try<Owned<TokenManager>> tokenMgr = TokenManager::create(authServer);
  if (tokenMgr.isError()) {
    return Error("failed to create token manager: " + tokenMgr.error());
  }

  return Owned<RegistryClientProcess>(
      new RegistryClientProcess(tokenMgr.get(), registryServer, creds));
}


RegistryClientProcess::RegistryClientProcess(
    const Owned<TokenManager>& tokenMgr,
    const URL& registryServer,
    const Option<RegistryClient::Credentials>& creds)
  : tokenManager_(tokenMgr),
    registryServer_(registryServer),
    credentials_(creds) {}


Try<hashmap<string, string>>
RegistryClientProcess::getAuthenticationAttributes(
    const Response& httpResponse) const
{
  if (httpResponse.headers.find("WWW-Authenticate") ==
      httpResponse.headers.end()) {
    return Error("failed to find WWW-Authenticate header value");
  }

  const string& authString = httpResponse.headers.at("WWW-Authenticate");

  const vector<string> authStringTokens = strings::tokenize(authString, " ");
  if ((authStringTokens.size() != 2) || (authStringTokens[0] != "Bearer")) {
    // TODO(jojy): Look at various possibilities of auth response. We currently
    // assume that the string will have realm information.
    return Error("invalid authentication header value: " + authString);
  }

  const vector<string> authParams = strings::tokenize(authStringTokens[1], ",");

  hashmap<string, string> authAttributes;
  auto addAttribute = [&authAttributes] (
      const string& param) -> Try<Nothing> {
    const vector<string> paramTokens =
      strings::tokenize(param, "=\"");

    if (paramTokens.size() != 2) {
      return Error(
          "failed to get authentication attribute from response parameter" +
              param);
    }

    authAttributes.insert({paramTokens[0], paramTokens[1]});

    return Nothing();
  };

  for (const auto i : authParams) {
    Try<Nothing> addRes = addAttribute(i);
    if (addRes.isError()) {
      return Error(addRes.error());
    }
  }

  return authAttributes;
}


Future<Response>
RegistryClientProcess::doHttpGet(
    const URL& url,
    const Option<hashmap<string, string>>& headers,
    const Duration& timeout,
    bool resend) const
{
  Owned<Promise<Response>> promise(new Promise<Response>());

  http::get(url, headers)
    .after(timeout, [promise] (
        const Future<Response>& httpResponseFuture) -> Future<Response> {
      return Failure("response timeout");
    })
    .onAny(defer(self(), [this, promise, url, headers, timeout, resend] (
        const Future<Response>& httpResponseFuture) {
      if (!httpResponseFuture.isReady()) {
        promise->fail("failed to get response to http request: " +
            httpResponseFuture.failure());
        return;
      }

      if (httpResponseFuture.get().status == "200 OK") {
        promise->associate(httpResponseFuture);
        return;
      }

      if (!resend) {
        promise->fail("bad response: " + httpResponseFuture.get().status);
        return;
      }

      if (httpResponseFuture.get().status == "401 Unauthorized") {
        Try<hashmap<string, string>> authAttributes =
          getAuthenticationAttributes(httpResponseFuture.get());

        if (authAttributes.isError()) {
          promise->fail("failed to get authentication attributes: " +
              authAttributes.error());
          return;
        }

        // TODO(jojy): Currently only handling TLS/cert authentication.
        Future<Token> tokenResponse = tokenManager_->getToken(
          authAttributes.get().at("service"),
          authAttributes.get().at("scope"),
          None());

        tokenResponse
          .after(timeout, [] (
              Future<Token> tokenResponse) -> Future<Token> {
            tokenResponse.discard();
            return Failure("token response timeout");
          })
        .onAny(defer(
              self(), [this, promise, timeout, url] (
                const Future<Token>& tokenResponse) {
            if (tokenResponse.isFailed()) {
              promise->fail("failed to get token response: " +
                  tokenResponse.failure());

              return;
            }

            // Send request with acquired token.
            hashmap<string, string> authHeaders = {
              {"Authorization", "Bearer " + tokenResponse.get().raw}
            };

            promise->associate(doHttpGet(url, authHeaders, timeout, true));
        }));
      } else if (httpResponseFuture.get().status == "307 Temporary Redirect") {
        auto toURL = [] (
            const string& urlString) -> Try<URL> {
          // TODO(jojy): Need to add functionality to URL class that parses a
          // string to its URL components. For now, assuming:
          //  - scheme is https
          //  - path always ends with /

          static const string schemePrefix = string("https") + "://";

          if (urlString.find_first_of(schemePrefix) == string::npos) {
            return Error("failed to find expected token '" +
                schemePrefix + "' in redirect url");
          }

          const string schemeSuffix = urlString.substr(schemePrefix.length());

          const vector<string> components =
            strings::tokenize(schemeSuffix, "/");

          const string path = schemeSuffix.substr(components[0].length());

          const vector<string> addrComponents =
            strings::tokenize(components[0], ":");

          uint16_t port = 80;
          string domain = components[0];

          if (addrComponents.size() == 2) {
            domain = addrComponents[0];

            Try<uint16_t> tryPort = numify<uint16_t>(addrComponents[1]);
            if (tryPort.isError()) {
              return Error("failed to parse location: " +
                  urlString + " for port.");
            }

            port = tryPort.get();
          }

          return URL("https", domain, port, path);
        };

        if (httpResponseFuture.get().headers.find("Location") ==
            httpResponseFuture.get().headers.end()) {
          promise->fail(
              "invalid redirect response: 'Location' not found in headers.");

          return;
        }

        const string& location =
          httpResponseFuture.get().headers.at("Location");

        Try<URL> tryUrl = toURL(location);
        if (tryUrl.isError()) {
          promise->fail("failed to parse '" + location + "': " + tryUrl.error());
          return;
        }

        promise->associate(doHttpGet(tryUrl.get(), headers, timeout, false));
      } else {
        promise->fail("invalid response: " + httpResponseFuture.get().status);
      }
    }));

  return promise->future();
}


Future<ManifestResponse> RegistryClientProcess::getManifest(
    const std::string& path,
    const Option<std::string>& tag,
    const Duration& timeOut)
{
  URL manifestURL(registryServer_);
  manifestURL.path =
    "v2/" + path + "/manifests/" + getOptionalValue(tag, string(""));

  Owned<Promise<ManifestResponse>> promise(new Promise<ManifestResponse>());

  auto getManifestResponse = [] (
      const Response& httpResponse) -> Try<ManifestResponse> {

    Try<JSON::Object> responseJSON =
      JSON::parse<JSON::Object>(httpResponse.body);

    if (responseJSON.isError()) {
      return Error(responseJSON.error());
    }

    if (!httpResponse.headers.contains("Docker-Content-Digest")) {
      return Error("Docker-Content-Digest header missing in response");
    }

    return ManifestResponse {
      httpResponse.headers.at("Docker-Content-Digest"), responseJSON.get(),
    };
  };

  doHttpGet(manifestURL, None(), timeOut, true)
    .onAny([promise, getManifestResponse] (
        const Future<Response>&  httpResponseFuture) {
      if (!httpResponseFuture.isReady()) {
        promise->fail("failed to get response to manifest request: " +
            httpResponseFuture.failure());

        return;
      }

      Try<ManifestResponse> manifestResponse =
        getManifestResponse(httpResponseFuture.get());

      if (manifestResponse.isError()) {
        promise->fail("failed to parse manifest response: " +
            manifestResponse.error());
        return;
      }

      promise->associate(manifestResponse.get());
    });


  return promise->future();
}


Future<size_t> RegistryClientProcess::getBlob(
    const std::string& path,
    const Option<std::string>& digest,
    const Path& filePath,
    const Duration& timeOut,
    const size_t& maxSize)
{
  URL blobURL(registryServer_);
  blobURL.path =
    "v2/" + path + "/blobs/" + getOptionalValue(digest, string(""));

  Owned<Promise<size_t>> promise(new Promise<size_t>());

  auto saveBlob = [filePath] (
      const Response& httpResponse) -> Try<size_t> {
    Try<Nothing> writeResult =
      os::write(filePath, httpResponse.body);

    // TODO(jojy): Add verification step.

    if (writeResult.isError()) {
      return Error(writeResult.error());
    }

    return httpResponse.body.length();
  };

  doHttpGet(blobURL, None(), timeOut, true)
    .onAny([promise, saveBlob] (
        const Future<Response>&  httpResponseFuture) {
      if (!httpResponseFuture.isReady()) {
        promise->fail("failed to get response to blob request: " +
            httpResponseFuture.failure());

        return;
      }

      Try<size_t> blobSaved = saveBlob(httpResponseFuture.get());
      if (blobSaved.isError()) {
        const string errMsg("failed to save blob: " + blobSaved.error());
        promise->fail(errMsg);
        return;
      }

      promise->associate(blobSaved.get());
    });

  return promise->future();
}

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
