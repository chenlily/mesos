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

#include <process/gtest.hpp>

#include <stout/gtest.hpp>
#include <stout/json.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>

#include "slave/containerizer/provisioners/docker/reference_store.hpp"
#include "slave/containerizer/provisioners/docker/store.hpp"
#include "slave/containerizer/fetcher.hpp"

#include "tests/utils.hpp"

using std::list;
using std::string;

using namespace process;
using namespace mesos::internal::slave;
using namespace mesos::internal::slave::docker;

namespace mesos {
namespace internal {
namespace tests {

class DockerProvisionerLocalStoreTest : public TemporaryDirectoryTest
{
public:
  void verifyLocalDockerImage(
      const slave::Flags& flags,
      const DockerImage& dockerImage)
  {
    // Verify contents of the image in store directory.
    EXPECT_TRUE(
        os::exists(path::join(flags.docker_store_dir, "123", "rootfs")));
    EXPECT_TRUE(
        os::exists(path::join(flags.docker_store_dir, "456", "rootfs")));
    EXPECT_SOME_EQ(
        "foo 123",
        os::read(path::join(flags.docker_store_dir, "123", "rootfs" , "temp")));
    EXPECT_SOME_EQ(
        "bar 456",
        os::read(path::join(flags.docker_store_dir, "456", "rootfs", "temp")));

    // Verify the docker Image provided.
    EXPECT_EQ(dockerImage.imageName, "abc");
    list<string> expectedLayers;
    expectedLayers.push_back("123");
    expectedLayers.push_back("456");
    EXPECT_EQ(dockerImage.layers, expectedLayers);
  }

protected:
  virtual void SetUp()
  {
    TemporaryDirectoryTest::SetUp();

    string imageDir = path::join(os::getcwd(), "images");
    string image = path::join(imageDir, "abc");
    ASSERT_SOME(os::mkdir(imageDir));
    ASSERT_SOME(os::mkdir(image));

    JSON::Value repositories = JSON::parse(
      "{"
      "  \"abc\": {"
      "    \"latest\": \"456\""
      "  }"
      "}").get();
    ASSERT_SOME(
        os::write(path::join(image, "repositories"), stringify(repositories)));

    ASSERT_SOME(os::mkdir(path::join(image, "123")));
    JSON::Value manifest123 = JSON::parse(
      "{"
      "  \"parent\": \"\""
      "}").get();
    ASSERT_SOME(os::write(
        path::join(image, "123", "json"), stringify(manifest123)));
    ASSERT_SOME(os::mkdir(path::join(image, "123", "layer")));
    ASSERT_SOME(
        os::write(path::join(image, "123", "layer", "temp"), "foo 123"));

    // Must change directory to avoid carrying over /path/to/archive during tar.
    const string cwd = os::getcwd();
    ASSERT_SOME(os::chdir(path::join(image, "123", "layer")));
    ASSERT_SOME(os::tar(".", "../layer.tar"));
    ASSERT_SOME(os::chdir(cwd));
    ASSERT_SOME(os::rmdir(path::join(image, "123", "layer")));

    ASSERT_SOME(os::mkdir(path::join(image, "456")));
    JSON::Value manifest456 = JSON::parse(
      "{"
      "  \"parent\": \"123\""
      "}").get();
    ASSERT_SOME(
        os::write(path::join(image, "456", "json"), stringify(manifest456)));
    ASSERT_SOME(os::mkdir(path::join(image, "456", "layer")));
    ASSERT_SOME(
        os::write(path::join(image, "456", "layer", "temp"), "bar 456"));

    ASSERT_SOME(os::chdir(path::join(image, "456", "layer")));
    ASSERT_SOME(os::tar(".", "../layer.tar"));
    ASSERT_SOME(os::chdir(cwd));
    ASSERT_SOME(os::rmdir(path::join(image, "456", "layer")));

    ASSERT_SOME(os::chdir(image));
    ASSERT_SOME(os::tar(".", "../abc.tar"));
    ASSERT_SOME(os::chdir(cwd));
    ASSERT_SOME(os::rmdir(image));
  }
};

// This test verifies that a locally stored Docker image in the form of a
// tar achive created from a 'docker save' command can be unpacked and
// stored in the proper locations accessible to the Docker provisioner.
TEST_F(DockerProvisionerLocalStoreTest, LocalStoreTestWithTar)
{
  string imageDir = path::join(os::getcwd(), "images");
  string image = path::join(imageDir, "abc");
  ASSERT_SOME(os::mkdir(imageDir));
  ASSERT_SOME(os::mkdir(image));

  slave::Flags flags;
  flags.docker_store = "local";
  flags.docker_store_dir = path::join(os::getcwd(), "store");
  flags.docker_discovery_local_dir = imageDir;

  // Fetcher is not relevant to local store. It is passed through from the
  // provisioner interface.
  Fetcher fetcher;
  Try<Owned<Store>> store = Store::create(flags, &fetcher);
  ASSERT_SOME(store);

  string sandbox = path::join(os::getcwd(), "sandbox");
  ASSERT_SOME(os::mkdir(sandbox));
  Future<DockerImage> dockerImage = store.get()->put("abc", sandbox);
  AWAIT_READY(dockerImage);

  verifyLocalDockerImage(flags, dockerImage.get());

  Future<Option<DockerImage>> dockerImageOption = store.get()->get("abc");
  AWAIT_READY(dockerImageOption);
  ASSERT_SOME(dockerImageOption.get());
  verifyLocalDockerImage(flags, dockerImageOption.get().get());
}

// This tests the ability of the reference store to recover the images it has
// already stored on disk when it is initialized.
TEST_F(DockerProvisionerLocalStoreTest, ReferenceStoreInitialization)
{
  slave::Flags flags;
  flags.docker_store = "local";
  flags.docker_store_dir = path::join(os::getcwd(), "store");
  flags.docker_discovery_local_dir = path::join(os::getcwd(), "images");

  // Fetcher is not relevant to local store. It is passed through from the
  // provisioner interface.
  Fetcher fetcher;
  Try<Owned<Store>> store = Store::create(flags, &fetcher);
  ASSERT_SOME(store);

  string sandbox = path::join(os::getcwd(), "sandbox");
  ASSERT_SOME(os::mkdir(sandbox));
  Future<DockerImage> dockerImage = store.get()->put("abc", sandbox);
  AWAIT_READY(dockerImage);

  // Store is deleted and recreated. Reference Store is initialized upon
  // creation of the store.
  store.get().reset();
  store = Store::create(flags, &fetcher);
  ASSERT_SOME(store);

  Future<Option<DockerImage>> dockerImageOption = store.get()->get("abc");
  AWAIT_READY(dockerImageOption);
  ASSERT_SOME(dockerImageOption.get());
  verifyLocalDockerImage(flags, dockerImageOption.get().get());
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
