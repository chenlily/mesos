# Mesos Provisioner

## What is Mesos Provisioner?
Mesos provisioner allows Mesos containers created and managed through Mesos containerizer to be provisioned using common image specification formats such as AppContainer and Docker. Both AppContainer and Docker images are currently supported through Mesos containerizer's provisioner.

## What does Mesos Provisioner do?
Mesos provisioner implements image discovery, storage, and also the provisioning of the image's filesystem changeset. Currently, Mesos provisioner supports local and simple discovery of AppC images as well as local discovery of Docker images. For local discovery, images are found in the provisioner_local_dir specified to the slave. Mesos provisioner then stores the configurations and files provided in the image and also provisions each image and its dependencies and mounts the filesystem into a rootfs.

## Setup and Slave Flags

To run the slave to enable Mesos containerizer, you must launch the slave with mesos as a containerizer option. this is the default containerizer for the mesos slave. The provisioner type (must?) also be specified through the provisioners flag, a comma separated list of image rootfs provisioners.

* Example: `mesos-slave`
* Example: `mesos-slave --containerizers=mesos --provisioners=docker`
* Example: `mesos-salve --containerizers=docker,mesos --provisioners=appc,docker`

Additional configurations can be set with the following flags:

* `--provisioner_rootfs_dir` Directory the provisioner will store container root filesystems in, default value: "/tmp/mesos/containers"
* `--provisioner_store_dir`, Directory the provisioner will store images in, default value: "tmp/mesos/store"
* `--provisioner_discovery`, Strategy for provisioner to discover images, default value: "simple"
* `--provisioner_local_dir`, Directory to look in for local images, default value: "tmp/mesos/images"
* `--provisioner_backend`, Strategy for provisioning container rootfs from images, default value: "copy"


Miscellaneous Flags which may be helpful:

* Isolator: `--isolation`, default value: "posix/cpu,posix/mem" (Configure with `--with-network-isolator` to use a custom isolator.)
* `--switch_user`, default value: true


## How to use Mesos Provisioner

### The URI protobuf structure
```
message ContainerInfo {
  // All container implementation types.
  enum Type {
    DOCKER = 1;
    MESOS = 2;
  }
  ...
  message Image {
    enum Type {
      APPC = 1;
      DOCKER = 2;
    }

    message AppC {
      // The name of the image.
      required string name = 1;

      // Image hash. Form is type-value, where type is sha512 and value is
      // the hex encoded string value of the hash of the uncompressed tar.
      required string id = 2;

      // Optional labels. Suggested labels: "version", "os", and "arch".
      optional Labels labels = 3;
    }

    message Docker {
      // The name of the image.
      required string name = 1;
    }

    required Type type = 1;
    optional AppC appc = 2;
    optional Docker docker = 3;
  }

  message MesosInfo {
    optional Image image = 1;
  }

  required Type type = 1;
  repeated Volume volumes = 2;
  optional string hostname = 4;

  optional DockerInfo docker = 3;
  optional MesosInfo mesos = 5;
}
```
**Note:** DockerInfo refers to the information required to launch a Docker container through the docker containerizer which calls Docker Daemon to create and manage Docker containers. To launch a Mesos container using a Docker image, ContainerInfo will have type MESOS and have a MesosInfo message but no DockerInfo; however, Image type will be DOCKER and specify a Docker message.


## Recovery in Mesos Containerizer Provisioner


## Future Features
* Enable caching for container images
* Support image discovery and pulling Docker images from Docker registry


## Related Docs
For more information on the Mesos containerizer filesystem, namespace, and isolator features, visit [here](https://github.com/apache/mesos/blob/master/docs/mesos-containerizer.md).

For more information on launching Docker containers through the Docker
containerizer, visit [here](https://github.com/apache/mesos/blob/master/docs/docker-containerizer.md).
