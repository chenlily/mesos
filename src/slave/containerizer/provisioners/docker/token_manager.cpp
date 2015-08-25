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

#include <process/defer.hpp>
#include <process/dispatch.hpp>

#include "slave/containerizer/provisioners/docker/token_manager.hpp"

using namespace process;
using namespace process::http;

using std::hash;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace slave {
namespace provisioners {
namespace docker {
namespace registry {

const string TokenManager::TOKEN_PATH_PREFIX = "/v2/token/";

const Duration TokenManagerProcess::RESPONSE_TIMEOUT = Seconds(10);


Token::Token(const string& _raw,
    const JSON::Object& _header,
    const JSON::Object& _claims,
    const Option<Time>& _expiration,
    const Option<Time>& _notBefore)
  : raw(_raw),
    header(_header),
    claims(_claims),
    expiration(_expiration),
    notBefore(_notBefore) {}


Try<Token> Token::create(const string& raw)
{
  auto decode = [](
      const string& segment) -> Try<JSON::Object> {
    const auto length = segment.length();
    const auto padding = length % 4;
    string paddedSegment(segment);

    if (padding) {
      paddedSegment.append(padding, '=');
    }

    Try<string> decoded = base64::decode(paddedSegment);
    if (decoded.isError()) {
      return Error(decoded.error());
    }

    return JSON::parse<JSON::Object>(decoded.get());
  };

  const vector<string> tokens = strings::tokenize(raw, ".");

  if (tokens.size() != 3) {
    return Error("invalid raw token string");
  }

  Try<JSON::Object> header = decode(tokens[0]);
  if (header.isError()) {
    return Error("failed to decode 'header' segment: " + header.error());
  }

  Try<JSON::Object> claims = decode(tokens[1]);
  if (claims.isError()) {
    return Error("failed to decode 'claims' segment: " + claims.error());
  }

  Result<Time> expirationTime = getTimeValue(claims.get(), "exp");
  if (expirationTime.isError()) {
    return Error("failed to decode expiration time: " + expirationTime.error());
  }

  Option<Time> expiration;
  if (expirationTime.isSome()) {
    expiration = expirationTime.get();
  }

  Result<Time> notBeforeTime = getTimeValue(claims.get(), "nbf");
  if (notBeforeTime.isError()) {
    return Error("failed to decode not-before time: " + notBeforeTime.error());
  }

  Option<Time> notBefore;
  if (notBeforeTime.isSome()) {
    notBefore = notBeforeTime.get();
  }

  Token token(raw, header.get(), claims.get(), expiration, notBefore);

  if (token.isExpired()) {
      return Error("token has expired");
  }

  // TODO(jojy): Add signature validation.
  return token;
}


Result<Time> Token::getTimeValue(const JSON::Object& object, const string& key)
{
  Result<JSON::Number> jsonValue =
    object.find<JSON::Number>(key);

  Option<Time> timeValue;

  // If expiration is provided, we will process it for future validations.
  if (jsonValue.isSome()) {
    Try<Time> time = Time::create(jsonValue.get().value);
    if (time.isError()) {
      return Error( "failed to decode time: " + time.error());
    }

    timeValue = time.get();
  }

  return timeValue;
}


bool Token::isExpired() const
{
  if (expiration.isSome()) {
    return (Clock::now() >= expiration.get());
  }

  return false;
}


bool Token::isValid() const
{
  if (!isExpired()) {
    if (notBefore.isSome()) {
      return (Clock::now() >= notBefore.get());
    }

    return true;
  }

  // TODO(jojy): Add signature validation.

  return false;
}


size_t TokenManager::TokenCacheKeyHash::operator() (
    const TokenManager::TokenCacheKey& key) const
{
  hash<string> hashFn;

  return (hashFn(key.service) ^
    (hashFn(key.scope) << 1));
}


bool TokenManager::TokenCacheKeyEqual::operator() (
    const TokenManager::TokenCacheKey& left,
    const TokenManager::TokenCacheKey& right) const
{
  return ((left.service == right.service) &&
    (left.scope == right.scope));
}


Try<Owned<TokenManager>> TokenManager::create(
    const http::URL& realm)
{
  Try<Owned<TokenManagerProcess>> process = TokenManagerProcess::create();
  if (process.isError()) {
    return Error(process.error());
  }

  return Owned<TokenManager>(new TokenManager(realm, process.get()));
}


TokenManager::TokenManager(const URL& realm,
    Owned<TokenManagerProcess>& process)
  : realm_(realm),
    process_(process)
{
  spawn(CHECK_NOTNULL(process_.get()));
}


TokenManager::~TokenManager()
{
  terminate(process_.get());
  process::wait(process_.get());
}


Future<Token> TokenManager::getToken(
      const string& service,
      const string& scope,
      const Option<string>& account)
{
  const TokenCacheKey tokenKey = {service, scope};

  if (tokenCache_.contains(tokenKey)) {
    Token token = tokenCache_.at(tokenKey);

    if (token.isValid()) {
      return token;
    } else {
      LOG(WARNING) << "Cached token was invalid. Will fetch once again.";
    }
  }

  URL tokenUrl = realm_;
  tokenUrl.path = TOKEN_PATH_PREFIX;

  tokenUrl.query = {
    {"service", service},
    {"scope", scope},
  };

  if (account.isSome()) {
    tokenUrl.query.insert({"account", account.get()});
  }

  Owned<Promise<Token>> promise(new Promise<Token>());

  Future<Token> result =
    dispatch(
      process_.get(),
      &TokenManagerProcess::getToken,
      tokenUrl);

  result.onFailed([promise](const string& failure) {
      promise->fail(failure);
      return Failure(failure);
  });

  result
    .onAny([this, promise, tokenKey] (
        const Future<Token>& token) -> Future<Token> {
      if (token.isFailed()) {
        promise->fail(token.failure());
        return Failure(token.failure());
      }

      tokenCache_.insert({tokenKey, token.get()});

      promise->associate(token);
      return promise->future();
    });

  return promise->future();
}


Try<Owned<TokenManagerProcess>> TokenManagerProcess::create()
{
  return Owned<TokenManagerProcess>(new TokenManagerProcess());
}


TokenManagerProcess::TokenManagerProcess() {}


Try<Token> TokenManagerProcess::getTokenFromResponse(
    const Response& response) const
{
  Try<JSON::Object> tokenJSON = JSON::parse<JSON::Object>(response.body);
  if (tokenJSON.isError()) {
    return Error(tokenJSON.error());
  }

  Result<JSON::String> tokenString =
    tokenJSON.get().find<JSON::String>("token");

  if (tokenString.isError()) {
    return Error(tokenString.error());
  }

  Try<Token> result = Token::create(tokenString.get().value);

  if (result.isError()) {
    return Error(result.error());
  }

  return result.get();;
}


Future<Token> TokenManagerProcess::getToken(const http::URL& url)
{
  Owned<Promise<Token>> promise(new Promise<Token>());

  Future<Response> response = http::get(url, None());
  response
    .after(RESPONSE_TIMEOUT, [] (Future<Response> resp) -> Future<Response> {
      resp.discard();
      return Failure("timeout waiting for response to token request");
    })
    .onAny(defer(self(), [this, promise](
      const Future<Response>& response) {
      if (response.isFailed()) {
        promise->fail("failed to get response for token request: " +
          response.failure());
        return;
      }

      Try<Token> token = getTokenFromResponse(response.get());
      if (token.isError()) {
        promise->fail("failed to parse JSON Web Token object from response: " +
          token.error());
        return;
      }

      promise->associate(token.get());
    }));

  return promise->future();
}

// TODO(jojy): Add implementatio for basic authentication based getToken API

} // namespace registry {
} // namespace docker {
} // namespace provisioners {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
