<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://s3.diaway.com/files/leil/banner-leilfs-dark.png">
  <img alt="LeilFS" src="https://s3.diaway.com/files/leil/banner-leilfs-light.png" width="100%">
</picture>

**A free and open-source, distributed POSIX file system inspired by the Google File System.**

[![Release](https://img.shields.io/github/v/release/leil-io/leilfs?style=flat-square&labelColor=141929&color=479DFF&label=release)](https://github.com/leil-io/leilfs/releases)
[![License](https://img.shields.io/badge/license-GPL_3.0-6F8297?style=flat-square&labelColor=141929)](COPYING)
![C++](https://img.shields.io/badge/C++-23-479DFF?style=flat-square&labelColor=141929&logo=cplusplus&logoColor=white)
![POSIX](https://img.shields.io/badge/POSIX-compatible-479DFF?style=flat-square&labelColor=141929)
[![Stars](https://img.shields.io/github/stars/leil-io/leilfs?style=flat-square&labelColor=141929&color=95A3B2&label=stars)](https://github.com/leil-io/leilfs/stargazers)

[**Documentation**](https://docs.leil.io) ·
[Quick start](#quick-start) ·
[Use cases](#use-cases) ·
[Website](https://leil.io) ·
[Contact](#contact-us)

</div>

<img src="https://s3.diaway.com/files/leil/blue/divider.svg" width="100%">

## About

[**LeilFS**](https://leil.io/products/leil-fs/) is a **free and open source, distributed POSIX file system**
inspired by [Google File System](https://en.wikipedia.org/wiki/Google_File_System).
LeilFS is being developed and maintained by the team from
[Leil](https://leil.io). Designed to run on commodity hardware, LeilFS is a
**high-performance, scalable, and reliable file system** that provides
**high availability, data integrity, fault tolerance**, and performance on par with local
file systems. It is easy to deploy and manage, and it is designed to be used in
a wide range of applications, from small clusters to large data centers.

<img src="https://s3.diaway.com/files/leil/blue/divider.svg" width="100%">

## Use cases

We target the use cases below as primary and this is where we are proud to be a great fit. [Contact us](#contact-us) to learn more.

<table>
<tr>
<td width="50%" valign="top">

<img src="https://s3.diaway.com/files/leil/blue/icon-media.svg" width="56">

**Media & video post-production**

Sustained sequential throughput for 4K/8K editorial workflows. Proprietary HM-SMR support, Windows client, Connect and Navigator apps, and clustered Samba.

</td>
<td width="50%" valign="top">

<img src="https://s3.diaway.com/files/leil/blue/icon-datalake.svg" width="56">

**Data lakes & AI pipelines**

A single POSIX namespace across nodes and JBODs. Training and analytics read directly from the file system — no object gateway in the path.

</td>
</tr>
<tr>
<td width="50%" valign="top">

<img src="https://s3.diaway.com/files/leil/blue/icon-backup.svg" width="56">

**Backup & long-term retention**

Erasure coding keeps usable capacity high at low overhead. Predictable cost per TB on high-density commodity drives.

</td>
<td width="50%" valign="top">

<img src="https://s3.diaway.com/files/leil/blue/icon-hpc.svg" width="56">

**Research & HPC clusters**

Parallel access from many clients with per-directory redundancy goals, so hot and cold datasets share one deployment.

</td>
</tr>
</table>

<img src="https://s3.diaway.com/files/leil/blue/divider.svg" width="100%">

## Feature list

<table>
<tr>
<td width="33%" valign="top">

<img src="https://s3.diaway.com/files/leil/blue/icon-shield.svg" width="48">

**Resilient by architecture**

Separated components for metadata servers (**Master, Shadow, Metaloggers**), data servers (**Chunkservers**), and **Clients**.

</td>
<td width="33%" valign="top">

<img src="https://s3.diaway.com/files/leil/blue/icon-scale.svg" width="48">

**Scales linearly**

Add chunkservers to add capacity and throughput. Rebalancing is automatic and online.

</td>
<td width="33%" valign="top">

<img src="https://s3.diaway.com/files/leil/blue/icon-posix.svg" width="48">

**POSIX, not almost-POSIX**

Real POSIX semantics, so existing applications run unmodified.

</td>
</tr>
</table>

| | Capability | Detail |
|:--|:--|:--|
| ![](https://img.shields.io/badge/-✓-479DFF?style=flat-square) | **High availability** | **uRaft-based** metadata failover with coordinated floating IP management for seamless continuity. |
| ![](https://img.shields.io/badge/-✓-479DFF?style=flat-square) | **Seamless hardware refresh and expansion** | Nodes and drives can be added or replaced without interrupting client access. |
| ![](https://img.shields.io/badge/-✓-479DFF?style=flat-square) | **Data integrity** | **End-to-end data** integrity with **CRC verification** per chunk and periodic validation operations. |
| ![](https://img.shields.io/badge/-✓-479DFF?style=flat-square) | **Robust redundancy** | <ul><li>**Erasure Coding (EC):** Reed-Solomon `EC(d, p)` for high durability, supporting simultaneous server loss without data loss or service disruption.</li><li>**Standard replication:** Simple mirroring for improved locality and performance, especially for geographically distributed deployments.</li><li>**Instant Copy-on-Write Snapshots:** Fast and immutable snapshots enabling historical state access and safe filesystem-level rollback.</li></ul> |
| ![](https://img.shields.io/badge/-✓-479DFF?style=flat-square) | **Protocol interoperability** | <ul><li>**S3 compatibility:** Supported through Versity gateway.</li><li>**NFS support:** Full NFSv3/NFSv4 support through Ganesha plugin (FSAL).</li><li>**Samba/CIFS support:** High performance settings for shares on top of Linux native mount points.</li></ul> |
| ![](https://img.shields.io/badge/-✓-479DFF?style=flat-square) | **Advanced ACL framework** | Rich **NFSv4** and **POSIX ACL** support for precise access management. |
| ![](https://img.shields.io/badge/-✓-479DFF?style=flat-square) | **POSIX and flock advisory locking** | Includes byte-range locks for concurrent collaborative access. |
| ![](https://img.shields.io/badge/-✓-479DFF?style=flat-square) | **Granular quota management** | Limits by user, group, and directory with independent caps for size and inode count. |
| ![](https://img.shields.io/badge/-✓-479DFF?style=flat-square) | **Fast recursive deletion** | Efficient removal of large directory trees via asynchronous task-manager operations. |
| ![](https://img.shields.io/badge/-✓-479DFF?style=flat-square) | **Flexible media strategy** | **HDD**, **SSD**, and **NVMe** can coexist in the same cluster with labels and goal-based placement for tiering behavior. |
| ![](https://img.shields.io/badge/-✓-479DFF?style=flat-square) | **Periodic scrubbing for durability** | <ul><li>**Metadata scans:** Validates chunk availability and redundancy compliance.</li><li>**Data scrubbing:** **CRC-based** block checking ensures ongoing data correctness.</li></ul> |
| ![](https://img.shields.io/badge/-✓-479DFF?style=flat-square) | **Automatic data rebalancing** | Reclaims space and redistributes chunks when disks or servers are added or removed. |

<img src="https://s3.diaway.com/files/leil/blue/divider.svg" width="100%">

## Quick start

<table>
<tr>
<td width="33%" valign="top">

<img src="https://img.shields.io/badge/-✓-479DFF?style=flat-square" alt="" height="14"> **Install**

Use the [Installation Guide](INSTALL.md) for packages, dependencies, and full installation steps.

</td>
<td width="33%" valign="top">

<img src="https://img.shields.io/badge/-✓-479DFF?style=flat-square" alt="" height="14"> **Set up a cluster**

Follow the [Quick Start Guide](https://docs.leil.io/quick-start) for a simple single-machine **LeilFS** setup.

</td>
<td width="33%" valign="top">

<img src="https://img.shields.io/badge/-✓-479DFF?style=flat-square" alt="" height="14"> **Go further**

Continue with the [Administration Guide](https://docs.leil.io/administration-guide) for production topology and advanced configuration.

</td>
</tr>
</table>

### Build from source

This section assumes the required dependencies are already installed. If not,
see the [Installation Guide](INSTALL.md) for platform-specific dependency and
source-build instructions.

```bash
git clone https://github.com/leil-io/leilfs.git
cd leilfs
mkdir build
cd build
cmake ..
nice -n 16 make -j$(nproc)
```

We run `make` under `nice` so the build uses a lower CPU priority. The
`-j$(nproc)` option matches the number of build jobs to your CPU core count.

> [!WARNING]
 > A parallel build with `-j$(nproc)` can exhaust memory on smaller systems. If
 > that happens, reduce the job count (for example, use `-j2`).

<img src="https://s3.diaway.com/files/leil/blue/divider.svg" width="100%">

## Documentation

<table>
<tr>
<td width="33%" valign="top">

**[Quick start →](https://docs.leil.io/quick-start)**

A working **single-machine** cluster in a few minutes.

</td>
<td width="33%" valign="top">

**[Installation →](https://docs.leil.io/administration-guide/installation)**

Dependencies, packages and compiling from source.

</td>
<td width="33%" valign="top">

**[Administration →](https://docs.leil.io/administration-guide)**

Topology, goals, quotas, upgrades and day-two operations.

</td>
</tr>
<tr>
<td width="33%" valign="top">

**[Architecture →](https://docs.leil.io/#architectural-overview-of-leilfs)**

**Masters**, **Shadows**, **Metaloggers**, **Chunkservers** and **Clients**.

</td>
<td width="33%" valign="top">

**[NFS & clients →](https://docs.leil.io/nfs-client)**

**Ganesha FSAL**, **Windows** and **macOS** clients.

</td>
<td width="33%" valign="top">

**[Man pages →](doc/)**

Reference documentation for commands, services, and configuration files.

</td>
</tr>
</table>

<p align="center"><a href="https://docs.leil.io"><img src="https://s3.diaway.com/files/leil/green/btn-docs.svg" alt="Read the docs" height="40"></a></p>

<img src="https://s3.diaway.com/files/leil/blue/divider.svg" width="100%">

## Contributing

**LeilFS** is built in the open and we review every contribution. Start with the [Contributing Guide](CONTRIBUTING.md) for the workflow, coding standards and commit conventions.

The [Developer Guide](https://docs.leil.io/dev-guide) is a good starting
point for how to setup a development environment and run tests.

<table>
<tr>
<td width="33%" valign="top">

**1 - Find something**

Browse [good first issues](https://github.com/leil-io/leilfs/labels/good%20first%20issue), or open one to describe what you have in mind.

</td>
<td width="33%" valign="top">

**2 - Build and test**

Follow the Quick Start to build locally, then run the test suite before opening a pull request.

</td>
<td width="33%" valign="top">

**3 - Open a PR**

Reference the issue, describe the change and what you tested. We respond on every PR.

</td>
</tr>
</table>

[![Good first issues](https://img.shields.io/github/issues/leil-io/leilfs/good%20first%20issue?style=flat-square&labelColor=141929&color=479DFF&label=good%20first%20issues)](https://github.com/leil-io/leilfs/labels/good%20first%20issue)
[![Open PRs](https://img.shields.io/github/issues-pr/leil-io/leilfs?style=flat-square&labelColor=141929&color=479DFF&label=open%20PRs)](https://github.com/leil-io/leilfs/pulls)
[![Contributors](https://img.shields.io/github/contributors/leil-io/leilfs?style=flat-square&labelColor=141929&color=6F8297&label=contributors)](https://github.com/leil-io/leilfs/graphs/contributors)

<img src="https://s3.diaway.com/files/leil/blue/divider.svg" width="100%">

## Contact us

Join our community chat on [Matrix](https://matrix.to/#/#leil:matrix.org) to connect with fellow **LeilFS** enthusiasts, developers and users.

<table>
<tr>
<td width="60%" valign="top">

<img src="https://img.shields.io/badge/-✓-479DFF?style=flat-square" alt="" height="10"> **Ask questions:** Get help, share experiences, and discuss **LeilFS** usage.

<img src="https://img.shields.io/badge/-✓-479DFF?style=flat-square" alt="" height="10"> **Discuss ideas:** Propose features, improvements, and best practices.

<img src="https://img.shields.io/badge/-✓-479DFF?style=flat-square" alt="" height="10"> **Receive updates:** Follow LeilFS development, releases, and community news.

</td>
<td width="40%" valign="top">

| | |
|:--|:--|
| ![Matrix](https://img.shields.io/badge/Matrix-141929?style=flat-square&logo=matrix&logoColor=white) | [#leil:matrix.org](https://matrix.to/#/#leil:matrix.org) |
| ![Email](https://img.shields.io/badge/Email-6F8297?style=flat-square&logo=maildotru&logoColor=white) | [hello@leil.io](mailto:hello@leil.io) |
| ![Website](https://img.shields.io/badge/Website-479DFF?style=flat-square&logo=googleearth&logoColor=white) | [leil.io](https://leil.io) |

</td>
</tr>
</table>

<img src="https://s3.diaway.com/files/leil/blue/divider.svg" width="100%">

## Licensing

Most of the software is licensed under **GPLv3**, except the **Ganesha FSAL**, which is
licensed **LGPLv3** and located under `src/nfs-ganesha/`. See the [FSAL LICENSE
file](src/nfs-ganesha/LICENSE) for more info.

<div align="center">

<img src="https://s3.diaway.com/files/leil/blue/divider.svg" width="100%">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://s3.diaway.com/files/leil/blue/logo-leil-negative.svg">
  <img alt="Leil" src="https://s3.diaway.com/files/leil/blue/logo-leil-positive.svg" height="26">
</picture>

**Thank you from the LeilFS team.**

<sub>Engineered in Estonia · © 2026 Leil Storage OÜ</sub>

</div>
