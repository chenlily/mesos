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

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#ifdef USE_SSL_SOCKET
#include <openssl/rsa.h>
#include <openssl/bio.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#endif

#include <process/address.hpp>
#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/owned.hpp>
#include <process/socket.hpp>
#include <process/subprocess.hpp>

#include <stout/duration.hpp>

#include "slave/containerizer/provisioners/docker/token_manager.hpp"
#include "tests/mesos.hpp"

using std::map;
using std::string;
using std::vector;

using namespace mesos::internal::slave::provisioners::docker::registry;
using namespace process;
using namespace process::network;

#ifdef USE_SSL_SOCKET

// TODO(jojy): This makes libprocess ssl helper symbols available at compile
// time. It would be better to have a common code for SSL server creation. Once
// we have more need for SSL utilities, this code along with its implementation
// needs to move to a common place.
namespace process {
namespace network {
namespace openssl {

extern void reinitialize();

extern Try<X509*> generate_x509(
    EVP_PKEY* subject_key,
    EVP_PKEY* sign_key,
    const Option<X509*>& parent_certificate = None(),
    int serial = 1,
    int days = 365,
    Option<std::string> hostname = None());

extern Try<EVP_PKEY*> generate_private_rsa_key(
    int bits = 2048,
    unsigned long exponent = RSA_F4);

extern Try<Nothing> write_key_file(
    EVP_PKEY* private_key, const Path& path);

extern Try<Nothing> write_certificate_file(
    X509* x509, const Path& path);

} // namespace openssl {
} // namespace network {
} // namespace process {

#endif


namespace mesos {
namespace internal {
namespace tests {


class DockerRegistryTokenTest : public ::testing::Test
{
private:
  const string hdrBase64 = base64::encode(
    "{ \
      \"alg\":\"ES256\", \
      \"typ\":\"JWT\", \
      \"x5c\":[\"test\"] \
    }");

  const string signBase64 = base64::encode("{\"\"}");

protected:
  string claimsJsonString;

  string getClaimsBase64() const
  {
    return base64::encode(claimsJsonString);
  }

  string getTokenString() const
  {
    return  hdrBase64 + "." + getClaimsBase64() + "." + signBase64;
  }
};

// Tests JSON Web Token parsing for a valid token string.
TEST_F(DockerRegistryTokenTest, ValidToken)
{
  const double expirySecs = Clock::now().secs() + Days(365).secs();

  claimsJsonString =
    "{\"access\" \
      :[ \
        { \
          \"type\":\"repository\", \
          \"name\":\"library/busybox\", \
          \"actions\":[\"pull\"]}], \
          \"aud\":\"registry.docker.io\", \
          \"exp\":" + stringify(expirySecs) + ", \
          \"iat\":1438887168, \
          \"iss\":\"auth.docker.io\", \
          \"jti\":\"l2PJDFkzwvoL7-TajJF7\", \
          \"nbf\":1438887166, \
          \"sub\":\"\" \
         }";

  Try<Token> token = Token::create(getTokenString());

  ASSERT_SOME(token);
}


// Tests JSON Web Token parsing for a token string with expiration date in the
// past.
TEST_F(DockerRegistryTokenTest, ExpiredToken)
{
  const double expirySecs = Clock::now().secs() - Days(365).secs();

  claimsJsonString =
    "{\"access\" \
      :[ \
        { \
          \"type\":\"repository\", \
          \"name\":\"library/busybox\", \
          \"actions\":[\"pull\"]}], \
          \"aud\":\"registry.docker.io\", \
          \"exp\":" + stringify(expirySecs) + ", \
          \"iat\":1438887166, \
          \"iss\":\"auth.docker.io\", \
          \"jti\":\"l2PJDFkzwvoL7-TajJF7\", \
          \"nbf\":1438887166, \
          \"sub\":\"\" \
         }";

  Try<Token> token = Token::create(getTokenString());

  EXPECT_ERROR(token);
}


// Tests JSON Web Token parsing for a token string with no expiration date.
TEST_F(DockerRegistryTokenTest, NoExpiration)
{
  claimsJsonString =
    "{\"access\" \
      :[ \
        { \
          \"type\":\"repository\", \
          \"name\":\"library/busybox\", \
          \"actions\":[\"pull\"]}], \
          \"aud\":\"registry.docker.io\", \
          \"iat\":1438887166, \
          \"iss\":\"auth.docker.io\", \
          \"jti\":\"l2PJDFkzwvoL7-TajJF7\", \
          \"nbf\":1438887166, \
          \"sub\":\"\" \
      }";

  const Try<Token> token = Token::create(getTokenString());

  ASSERT_SOME(token);
}


// Tests JSON Web Token parsing for a token string with not-before date in the
// future.
TEST_F(DockerRegistryTokenTest, NotBeforeInFuture)
{
  const double expirySecs = Clock::now().secs() + Days(365).secs();
  const double nbfSecs = Clock::now().secs() + Days(7).secs();

  claimsJsonString =
    "{\"access\" \
      :[ \
        { \
          \"type\":\"repository\", \
          \"name\":\"library/busybox\", \
          \"actions\":[\"pull\"]}], \
          \"aud\":\"registry.docker.io\", \
          \"exp\":" + stringify(expirySecs) + ", \
          \"iat\":1438887166, \
          \"iss\":\"auth.docker.io\", \
          \"jti\":\"l2PJDFkzwvoL7-TajJF7\", \
          \"nbf\":" + stringify(nbfSecs) + ", \
          \"sub\":\"\" \
         }";

  const Try<Token> token = Token::create(getTokenString());

  ASSERT_SOME(token);
  ASSERT_EQ(token.get().isValid(), false);
}


#ifdef USE_SSL_SOCKET

// Test suite for docker registry tests.
// TODO(jojy): Create a common SSL server test suite accessible from mesos
// source.
class DockerRegistryClientTest : public DockerRegistryTokenTest
{
protected:
  DockerRegistryClientTest() {}

  static const Path& key_path()
  {
    static Path path(path::join(os::getcwd(), "key.pem"));
    return path;
  }

  static const Path& certificate_path()
  {
    static Path path(path::join(os::getcwd(), "cert.pem"));
    return path;
  }

  static const Path& scrap_key_path()
  {
    static Path path(path::join(os::getcwd(), "scrap_key.pem"));
    return path;
  }

  static const Path& scrap_certificate_path()
  {
    static Path path(path::join(os::getcwd(), "scrap_cert.pem"));
    return path;
  }

  static void SetUpTestCase()
  {
    // We store the allocated objects in these results so that we can
    // have a consolidated 'cleanup()' function. This makes all the
    // 'EXIT()' calls more readable and less error prone.
    Result<EVP_PKEY*> private_key = None();
    Result<X509*> certificate = None();
    Result<EVP_PKEY*> scrap_key = None();
    Result<X509*> scrap_certificate = None();

    auto cleanup = [&private_key, &certificate, &scrap_key, &scrap_certificate](
        bool failure = true) {
      if (private_key.isSome()) { EVP_PKEY_free(private_key.get()); }
      if (certificate.isSome()) { X509_free(certificate.get()); }
      if (scrap_key.isSome()) { EVP_PKEY_free(scrap_key.get()); }
      if (scrap_certificate.isSome()) { X509_free(scrap_certificate.get()); }

      // If we are under a failure condition, clean up any files we
      // already generated. The expected behavior is that they will be
      // cleaned up in 'TearDownTestCase()'; however, we call ABORT
      // during 'SetUpTestCase()' failures.
      if (failure) {
        os::rm(key_path().value);
        os::rm(certificate_path().value);
        os::rm(scrap_key_path().value);
        os::rm(scrap_certificate_path().value);
      }
    };

    // Generate the authority key.
    private_key = openssl::generate_private_rsa_key();
    if (private_key.isError()) {
      ABORT("Could not generate private key: " + private_key.error());
    }

    // Figure out the hostname that 'INADDR_LOOPBACK' will bind to.
    // Set the hostname of the certificate to this hostname so that
    // hostname verification of the certificate will pass.
    Try<string> hostname = net::getHostname(net::IP(INADDR_LOOPBACK));
    if (hostname.isError()) {
      cleanup();
      ABORT("Could not determine hostname of 'INADDR_LOOPBACK': " +
            hostname.error());
    }

    // Generate an authorized certificate.
    certificate = openssl::generate_x509(
        private_key.get(),
        private_key.get(),
        None(),
        1,
        365,
        hostname.get());

    if (certificate.isError()) {
      cleanup();
      ABORT("Could not generate certificate: " + certificate.error());
    }

    // Write the authority key to disk.
    Try<Nothing> key_write =
      openssl::write_key_file(private_key.get(), key_path());

    if (key_write.isError()) {
      cleanup();
      ABORT("Could not write private key to disk: " + key_write.error());
    }

    // Write the authorized certificate to disk.
    Try<Nothing> certificate_write =
      openssl::write_certificate_file(certificate.get(), certificate_path());

    if (certificate_write.isError()) {
      cleanup();
      ABORT("Could not write certificate to disk: " +
            certificate_write.error());
    }

    // Generate a scrap key.
    scrap_key = openssl::generate_private_rsa_key();
    if (scrap_key.isError()) {
      cleanup();
      ABORT("Could not generate a scrap private key: " + scrap_key.error());
    }

    // Write the scrap key to disk.
    key_write = openssl::write_key_file(scrap_key.get(), scrap_key_path());

    if (key_write.isError()) {
      cleanup();
      ABORT("Could not write scrap key to disk: " + key_write.error());
    }

    // Generate a scrap certificate.
    scrap_certificate =
      openssl::generate_x509(scrap_key.get(), scrap_key.get());

    if (scrap_certificate.isError()) {
      cleanup();
      ABORT("Could not generate a scrap certificate: " +
            scrap_certificate.error());
    }

    // Write the scrap certificate to disk.
    certificate_write = openssl::write_certificate_file(
        scrap_certificate.get(),
        scrap_certificate_path());

    if (certificate_write.isError()) {
      cleanup();
      ABORT("Could not write scrap certificate to disk: " +
            certificate_write.error());
    }

    // Since we successfully set up all our state, we call cleanup
    // with failure set to 'false'.
    cleanup(false);
  }

  static void TearDownTestCase()
  {
    // Clean up all the pem files we generated.
    os::rm(key_path().value);
    os::rm(certificate_path().value);
    os::rm(scrap_key_path().value);
    os::rm(scrap_certificate_path().value);
  }

  virtual void SetUp()
  {
    // This unsets all the SSL environment variables. Necessary for
    // ensuring a clean starting slate between tests.
    os::unsetenv("SSL_ENABLED");
    os::unsetenv("SSL_SUPPORT_DOWNGRADE");
    os::unsetenv("SSL_CERT_FILE");
    os::unsetenv("SSL_KEY_FILE");
    os::unsetenv("SSL_VERIFY_CERT");
    os::unsetenv("SSL_REQUIRE_CERT");
    os::unsetenv("SSL_VERIFY_DEPTH");
    os::unsetenv("SSL_CA_DIR");
    os::unsetenv("SSL_CA_FILE");
    os::unsetenv("SSL_CIPHERS");
    os::unsetenv("SSL_ENABLE_SSL_V3");
    os::unsetenv("SSL_ENABLE_TLS_V1_0");
    os::unsetenv("SSL_ENABLE_TLS_V1_1");
    os::unsetenv("SSL_ENABLE_TLS_V1_2");
  }

  Try<Socket> setup_server(const map<string, string>& environment)
  {
    foreachpair (const string& name, const string& value, environment) {
      os::setenv(name, value);
    }
    openssl::reinitialize();

    const Try<Socket> create = Socket::create(Socket::SSL);
    if (create.isError()) {
      return Error(create.error());
    }

    Socket server = create.get();

    // We need to explicitly bind to INADDR_LOOPBACK so the
    // certificate we create in this test fixture can be verified.
    Try<network::Address> bind =
      server.bind(network::Address(net::IP(INADDR_LOOPBACK), 0));

    if (bind.isError()) {
      return Error(bind.error());
    }

    const Try<Nothing> listen = server.listen(BACKLOG);
    if (listen.isError()) {
      return Error(listen.error());
    }

    return server;
  }

  static constexpr size_t BACKLOG = 5;
};


// Tests TokenManager for a simple token request.
TEST_F(DockerRegistryClientTest, SimpleGetToken)
{
  Try<Socket> server = setup_server({
      {"SSL_ENABLED", "true"},
      {"SSL_KEY_FILE", key_path().value},
      {"SSL_CERT_FILE", certificate_path().value}});

  ASSERT_SOME(server);
  ASSERT_SOME(server.get().address());
  ASSERT_SOME(server.get().address().get().hostname());

  Future<Socket> socket = server.get().accept();

  // Create URL from server hostname and port.
  const http::URL url(
      "https",
      server.get().address().get().hostname().get(),
      server.get().address().get().port);

  Try<Owned<TokenManager>> tokenMgr = TokenManager::create(url);
  ASSERT_SOME(tokenMgr);

  Future<Token> token =
    tokenMgr.get()->getToken(
        "registry.docker.io",
        "repository:library/busybox:pull",
        None());

  AWAIT_ASSERT_READY(socket);

  // Construct response and send(server side).
  const double expirySecs = Clock::now().secs() + Days(365).secs();

  claimsJsonString =
    "{\"access\" \
      :[ \
        { \
          \"type\":\"repository\", \
          \"name\":\"library/busybox\", \
          \"actions\":[\"pull\"]}], \
          \"aud\":\"registry.docker.io\", \
          \"exp\":" + stringify(expirySecs) + ", \
          \"iat\":1438887168, \
          \"iss\":\"auth.docker.io\", \
          \"jti\":\"l2PJDFkzwvoL7-TajJF7\", \
          \"nbf\":1438887166, \
          \"sub\":\"\" \
         }";

  const string tokenString(getTokenString());
  const string tokenResponse = "{\"token\":\"" + tokenString + "\"}";

  const string buffer =
    string("HTTP/1.1 200 OK\r\n") +
    "Content-Length : " +
    stringify(tokenResponse.length()) + "\r\n" +
    "\r\n" +
    tokenResponse;

  AWAIT_ASSERT_READY(Socket(socket.get()).send(buffer));

  AWAIT_ASSERT_READY(token);
  ASSERT_EQ(token.get().raw, tokenString);
}


// Tests TokenManager for bad token response from server.
TEST_F(DockerRegistryClientTest, BadTokenResponse)
{
  Try<Socket> server = setup_server({
      {"SSL_ENABLED", "true"},
      {"SSL_KEY_FILE", key_path().value},
      {"SSL_CERT_FILE", certificate_path().value}});

  ASSERT_SOME(server);
  ASSERT_SOME(server.get().address());
  ASSERT_SOME(server.get().address().get().hostname());

  Future<Socket> socket = server.get().accept();

  // Create URL from server hostname and port.
  const http::URL url(
      "https",
      server.get().address().get().hostname().get(),
      server.get().address().get().port);

  Try<Owned<TokenManager>> tokenMgr = TokenManager::create(url);
  ASSERT_SOME(tokenMgr);

  Future<Token> token =
    tokenMgr.get()->getToken(
        "registry.docker.io",
        "repository:library/busybox:pull",
        None());

  AWAIT_ASSERT_READY(socket);

  const string tokenString("bad token");
  const string tokenResponse = "{\"token\":\"" + tokenString + "\"}";

  const string buffer =
    string("HTTP/1.1 200 OK\r\n") +
    "Content-Length : " +
    stringify(tokenResponse.length()) + "\r\n" +
    "\r\n" +
    tokenResponse;

  AWAIT_ASSERT_READY(Socket(socket.get()).send(buffer));

  AWAIT_FAILED(token);
}


// Tests TokenManager for request to invalid server.
TEST_F(DockerRegistryClientTest, BadTokenServerAddress)
{
  // Create URL from server hostname and port.
  const http::URL url( "https", stringify(Clock::now().secs()), 0);

  Try<Owned<TokenManager>> tokenMgr = TokenManager::create(url);
  ASSERT_SOME(tokenMgr);

  Future<Token> token =
    tokenMgr.get()->getToken(
        "registry.docker.io",
        "repository:library/busybox:pull",
        None());

  AWAIT_FAILED(token);
}

#endif // USE_SSL_SOCKET


} // namespace tests {
} // namespace internal {
} // namespace mesos {
