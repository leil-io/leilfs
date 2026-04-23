<p align="center">
    <img alt="LeilFS" style="width: 50%; height: auto;" src="https://s3.diaway.com/files/leil/logo_png/LeilFS_positive.png"/>
</p>
<h3 align="center">A Distributed POSIX File System</h3>

[![Matrix](https://img.shields.io/badge/matrix-join_chat-0dbd8b?logo=matrix&style=flat)](https://matrix.to/#/#leil:matrix.org)

## About

[LeilFS](https://leil.io) is a free and open source, distributed POSIX file
system inspired by [Google File System](https://en.wikipedia.org/wiki/Google_File_System).
LeilFS is being developed and maintained by the team from [Leil
Storage](https://leil.io). Designed to run on commodity hardware, LeilFS is a
high-performance, scalable, and reliable file system that provides high
availability, data integrity, fault tolerance, and performance on par with local
file systems. It is easy to deploy and manage, and it is designed to be used in
a wide range of applications, from small clusters to large data centers.

### About Leil Storage

There are a few commercial products built on top of LeilFS, Leil Storage being
the flagship product. Leil Storage = LeilFS + [HM-SMR drives
support](https://leil.io/#whatishmsmr) + [ICE](https://leil.io/#green) + Arctic
Forest Concept.

#### Use Cases

We target the use cases below as primary and this is where we are proud to be a
great fit. Contact us to learn more.

* Active Archive
* AI & HPC
* Backup
* CCTV Storage
* Enterprise File Sharing
* Media and Video Post-Production (including proprietary HM-SMR, Windows Client, Connect
  and Navigator Apps, clustered performant Samba support)

### Feature List

* Resilient architecture: separated components for metadata servers (Master, Shadow, Metaloggers), data servers (Chunkservers), and clients. 
* High Availability (HA): uRaft-based metadata failover with coordinated floating IP management for seamless continuity. 
* Seamless hardware refresh and expansion: Nodes and drives can be added or replaced without interrupting client access. 
* Data integrity: End-to-end data integrity with CRC verification per chunk and periodic validation operations. 
* Robust redundancy: 
  * Erasure Coding (EC): Reed-Solomon `EC(d, p)` for high durability, supporting simultaneous loss of up to `d` servers without data loss or service disruption. 
  * Standard replication: Simple mirroring for improved locality and performance, especially for geographically distributed deployments. 
  * Instant Copy-on-Write Snapshots: Fast and immutable snapshots enabling historical state access and safe filesystem-level rollback. 
* Protocol interoperability: 
  * S3 compatibility: Supported through Versity gateway. 
  * NFS support: Full NFSv3/NFSv4 support through Ganesha plugin (FSAL). 
  * Samba/CIFS support: High performance settings for shares on top of Linux native mount points. 
* * Advanced ACL framework: Rich, NFSv4 and POSIX ACL support for precise access management. 
* POSIX & flock advisory locking: Includes byte-range locks for concurrent collaborative access. 
* Granular quota management: Limits by user, group, and directory with independent caps for size and inode count. 
* Fast recursive deletion: Efficient removal of large directory trees via asynchronous task-manager operations. 
* Flexible media strategy: HDD, SSD, and NVMe can coexist in the same cluster with labels and goal-based placement for tiering behavior. 
* Periodic scrubbing for durability: 
  * Metadata scans: Validates chunk availability and redundancy compliance. 
  * Data scrubbing: CRC-based block checking ensures ongoing data correctness. 
* Automatic data rebalancing: Reclaims space and redistributes chunks when disks or servers are added or removed. 

## Quick Start

### Installation

Please refer to the [Installation Guide](INSTALL.md) for detailed instructions
on how to install LeilFS.

### Setup

Check the [Quick Start guide](https://docs.leil.io/quick-start) for a
simple setup of LeilFS on a single machine.

After the Quick Start Guide, for an advanced setup, please refer to the
[Administration Guide](https://docs.leil.io/administration-guide) as a starting
place.

### Building from source

This section assumes you have the necessary dependencies installed. If not,
check the [Installation Guide](INSTALL.md) for a list of dependencies (at least
for Ubuntu) and a more complete guide for compiling from source.

We use `nice` to set the building process to a lower priority, so it doesn't
hog memory and CPU resources. We also set `-j` to the number of cores in your
system to speed up the build process. Note that setting `-j` without nice can
lead to the system running out of memory/hanging.

```bash
git clone https://github.com/leil-io/leilfs.git
cd leilfs
mkdir build
cd build
cmake ..
nice -n 16 make -j$(nproc)
```

## Documentation

There are 2 types of documentation available:

* [Online documentation for a general overview](https://docs.leil.io/)
* [Man pages for specific commands and service configuration](doc/)

## Contributing

See the [Contributing Guide](CONTRIBUTING.md) for detailed information on how
to contribute to LeilFS.

The [Developer Guide](https://docs.leil.io/dev-guide) is a good starting
point for how to setup a development environment and run tests.

## Contact us

Join our [community chat on Matrix](https://matrix.to/#/#leil:matrix.org) to connect with fellow
LeilFS enthusiasts, developers, and users. In the chat, you can:

- **Ask Questions**: Seek guidance, share your experiences, and ask questions
related to LeilFS.
- **Discuss Ideas**: Engage in discussions about new features, improvements,
and best practices.
- **Receive Updates**: Stay informed about LeilFS developments, releases, and
events.

Join us and be part of the discussion.

## Licensing

Most of the software is licensed under GPLv3, except the Ganesha FSAL, which is
licensed LGPLv3 and located under `src/nfs-ganesha/`. See the [FSAL LICENSE
file](src/nfs-ganesha/LICENSE) for more info.

### Other ways to contact us
| Method                     | Link                                                          |
|----------------------------|---------------------------------------------------------------|
| :email: Email              | [hello@leil.io](mailto:hello@leil.io?subject=RFI) |
| :globe_with_meridians: Web | <https://leil.io>                                         |

Thank you for your help.

The LeilFS Team.
