<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://s3.diaway.com/files/leil/banner-leilfs-dark.png">
  <img alt="LeilFS" src="https://s3.diaway.com/files/leil/banner-leilfs-light.png" width="100%">
</picture>

**A free and open-source, distributed POSIX file system inspired by the Google File System.**

<p>
  <a href="https://github.com/leil-io/leilfs/actions/workflows/run-unit-and-ganesha-tests.yml"><img src="https://img.shields.io/github/actions/workflow/status/leil-io/leilfs/run-unit-and-ganesha-tests.yml?style=flat-square&labelColor=141929&color=0FD4A2&label=build" alt="Build"></a>
  <a href="https://github.com/leil-io/leilfs/releases"><img src="https://img.shields.io/github/v/release/leil-io/leilfs?style=flat-square&labelColor=141929&color=479DFF&label=release" alt="Release"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-GPL_3.0-6F8297?style=flat-square&labelColor=141929" alt="License"></a>
  <img src="https://img.shields.io/badge/C++-23-479DFF?style=flat-square&labelColor=141929&logo=cplusplus&logoColor=white" alt="C++">
  <img src="https://img.shields.io/badge/POSIX-compatible-0FD4A2?style=flat-square&labelColor=141929" alt="POSIX">
  <a href="https://github.com/leil-io/leilfs/stargazers"><img src="https://img.shields.io/github/stars/leil-io/leilfs?style=flat-square&labelColor=141929&color=95A3B2&label=stars" alt="Stars"></a>
</p>

<p>
  <a href="https://docs.leil.io"><b>Documentation</b></a> ·
  <a href="#quick-start">Quick start</a> ·
  <a href="#use-cases">Use cases</a> ·
  <a href="https://leil.io">Website</a> ·
  <a href="#contact-us">Contact</a>
</p>

</div>

<img src="https://s3.diaway.com/files/leil/divider.svg" width="100%">

## About

<strong><a href="https://leil.io/products/leil-fs/">LeilFS</a></strong> is a <strong>free and open source, distributed POSIX file system</strong>
inspired by <a href="https://en.wikipedia.org/wiki/Google_File_System">Google File System</a>.
LeilFS is being developed and maintained by the team from
<a href="https://leil.io">Leil</a>. Designed to run on commodity hardware, LeilFS is a
<strong>high-performance, scalable, and reliable file system</strong> that provides
<strong>high availability, data integrity, fault tolerance</strong>, and performance on par with local
file systems. It is easy to deploy and manage, and it is designed to be used in
a wide range of applications, from small clusters to large data centers.

<img src="https://s3.diaway.com/files/leil/divider.svg" width="100%">

## Use cases

We target the use cases below as primary and this is where we are proud to be a great fit. [Contact us](#contact-us) to learn more.

<table>
<tr>
<td width="50%" valign="top">
  <img src="https://s3.diaway.com/files/leil/icon-media.svg" width="56" align="left" hspace="12">
  <h3>Media &amp; video post-production</h3>
  <p>Sustained sequential throughput for 4K/8K editorial workflows. Proprietary HM-SMR support, Windows client, Connect and Navigator apps, and clustered Samba.</p>
</td>
<td width="50%" valign="top">
  <img src="https://s3.diaway.com/files/leil/icon-datalake.svg" width="56" align="left" hspace="12">
  <h3>Data lakes &amp; AI pipelines</h3>
  <p>A single POSIX namespace across nodes and JBODs. Training and analytics read directly from the file system — no object gateway in the path.</p>
</td>
</tr>
<tr>
<td width="50%" valign="top">
  <img src="https://s3.diaway.com/files/leil/icon-backup.svg" width="56" align="left" hspace="12">
  <h3>Backup &amp; long-term retention</h3>
  <p>Erasure coding keeps usable capacity high at low overhead. Predictable cost per TB on high-density commodity drives.</p>
</td>
<td width="50%" valign="top">
  <img src="https://s3.diaway.com/files/leil/icon-hpc.svg" width="56" align="left" hspace="12">
  <h3>Research &amp; HPC clusters</h3>
  <p>Parallel access from many clients with per-directory redundancy goals, so hot and cold datasets share one deployment.</p>
</td>
</tr>
</table>

<img src="https://s3.diaway.com/files/leil/divider.svg" width="100%">

## Feature list

<table>
<tr>
  <td width="33%" valign="top">
    <img src="https://s3.diaway.com/files/leil/icon-shield.svg" width="48">
    <h3>Resilient by architecture</h3>
    <p>Separated components for metadata servers (<strong>Master, Shadow, Metaloggers</strong>), data servers (<strong>Chunkservers</strong>), and <strong>Clients</strong>.</p>
  </td>
  <td width="33%" valign="top">
    <img src="https://s3.diaway.com/files/leil/icon-scale.svg" width="48">
    <h3>Scales linearly</h3>
    <p>Add chunkservers to add capacity and throughput. Rebalancing is automatic and online.</p>
  </td>
  <td width="33%" valign="top">
    <img src="https://s3.diaway.com/files/leil/icon-posix.svg" width="48">
    <h3>POSIX, not almost-POSIX</h3>
    <p>Real POSIX semantics, so existing applications run unmodified.</p>
  </td>
</tr>
</table>

| | Capability | Detail |
|:--|:--|:--|
| ![](https://img.shields.io/badge/-✓-0FD4A2?style=flat-square) | **High availability** | **uRaft-based** metadata failover with coordinated floating IP management for seamless continuity. |
| ![](https://img.shields.io/badge/-✓-0FD4A2?style=flat-square) | **Seamless hardware refresh and expansion** | Nodes and drives can be added or replaced without interrupting client access. |
| ![](https://img.shields.io/badge/-✓-0FD4A2?style=flat-square) | **Data integrity** | **End-to-end data** integrity with **CRC verification** per chunk and periodic validation operations. |
| ![](https://img.shields.io/badge/-✓-0FD4A2?style=flat-square) | **Robust redundancy** | <ul><li><strong>Erasure Coding (EC):</strong> Reed-Solomon <code>EC(d, p)</code> for high durability, supporting simultaneous server loss without data loss or service disruption.</li><li><strong>Standard replication:</strong> Simple mirroring for improved locality and performance, especially for geographically distributed deployments.</li><li><strong>Instant Copy-on-Write Snapshots:</strong> Fast and immutable snapshots enabling historical state access and safe filesystem-level rollback.</li></ul> |
| ![](https://img.shields.io/badge/-✓-0FD4A2?style=flat-square) | **Protocol interoperability** | <ul><li><strong>S3 compatibility:</strong> Supported through Versity gateway.</li><li><strong>NFS support:</strong> Full NFSv3/NFSv4 support through Ganesha plugin (FSAL).</li><li><strong>Samba/CIFS support:</strong> High performance settings for shares on top of Linux native mount points.</li></ul> |
| ![](https://img.shields.io/badge/-✓-0FD4A2?style=flat-square) | **Advanced ACL framework** | Rich **NFSv4** and **POSIX ACL** support for precise access management. |
| ![](https://img.shields.io/badge/-✓-0FD4A2?style=flat-square) | **POSIX and flock advisory locking** | Includes byte-range locks for concurrent collaborative access. |
| ![](https://img.shields.io/badge/-✓-0FD4A2?style=flat-square) | **Granular quota management** | Limits by user, group, and directory with independent caps for size and inode count. |
| ![](https://img.shields.io/badge/-✓-0FD4A2?style=flat-square) | **Fast recursive deletion** | Efficient removal of large directory trees via asynchronous task-manager operations. |
| ![](https://img.shields.io/badge/-✓-0FD4A2?style=flat-square) | **Flexible media strategy** | **HDD**, **SSD**, and **NVMe** can coexist in the same cluster with labels and goal-based placement for tiering behavior. |
| ![](https://img.shields.io/badge/-✓-0FD4A2?style=flat-square) | **Periodic scrubbing for durability** | <ul><li><strong>Metadata scans:</strong> Validates chunk availability and redundancy compliance.</li><li><strong>Data scrubbing:</strong> **CRC-based** block checking ensures ongoing data correctness.</li></ul> |
| ![](https://img.shields.io/badge/-✓-0FD4A2?style=flat-square) | **Automatic data rebalancing** | Reclaims space and redistributes chunks when disks or servers are added or removed. |

<img src="https://s3.diaway.com/files/leil/divider.svg" width="100%">

## Quick start

<table>
<tr>
<td width="33%" valign="top">
  <h3><img src="https://img.shields.io/badge/-✓-0FD4A2?style=flat-square" alt="" height="14"> Install</h3>
  <p>Use the <a href="INSTALL.md">Installation Guide</a> for packages, dependencies, and full installation steps.</p>
</td>
<td width="33%" valign="top">
  <h3><img src="https://img.shields.io/badge/-✓-0FD4A2?style=flat-square" alt="" height="14"> Set up a cluster</h3>
  <p>Follow the <a href="https://docs.leil.io/quick-start">Quick Start Guide</a> for a simple single-machine <strong>LeilFS</strong> setup.</p>
</td>
<td width="33%" valign="top">
  <h3><img src="https://img.shields.io/badge/-✓-0FD4A2?style=flat-square" alt="" height="14"> Go further</h3>
  <p>Continue with the <a href="https://docs.leil.io/administration-guide">Administration Guide</a> for production topology and advanced configuration.</p>
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

<img src="https://s3.diaway.com/files/leil/divider.svg" width="100%">

## Documentation

<table>
<tr>
  <td width="33%" valign="top"><h3><a href="https://docs.leil.io/quick-start">Quick start →</a></h3><p>A working <strong>single-machine</strong> cluster in a few minutes.</p></td>
  <td width="33%" valign="top"><h3><a href="https://docs.leil.io/administration-guide/installation">Installation →</a></h3><p>Dependencies, packages and compiling from source.</p></td>
  <td width="33%" valign="top"><h3><a href="https://docs.leil.io/administration-guide">Administration →</a></h3><p>Topology, goals, quotas, upgrades and day-two operations.</p></td>
</tr>
<tr>
  <td width="33%" valign="top"><h3><a href="https://docs.leil.io/#architectural-overview-of-leilfs">Architecture →</a></h3><p><strong>Masters</strong>, <strong>Shadows</strong>, <strong>Metaloggers</strong>, <strong>Chunkservers</strong> and <strong>Clients</strong>.</p></td>
  <td width="33%" valign="top"><h3><a href="https://docs.leil.io/nfs-client">NFS &amp; clients →</a></h3><p><strong>Ganesha FSAL</strong>, <strong>Windows</strong> and <strong>macOS</strong> clients.</p></td>
  <td width="33%" valign="top"><h3><a href="doc/">Man pages →</a></h3><p>Reference documentation for commands, services, and configuration files.</p></td>
</tr>
</table>

<p align="center"><a href="https://docs.leil.io"><img src="https://s3.diaway.com/files/leil/btn-docs.svg" alt="Read the docs" height="40"></a></p>

<img src="https://s3.diaway.com/files/leil/divider.svg" width="100%">

## Contributing

**LeilFS** is built in the open and we review every contribution. Start with the [Contributing Guide](CONTRIBUTING.md) for the workflow, coding standards and commit conventions.

The [Developer Guide](https://docs.leil.io/dev-guide) is a good starting
point for how to setup a development environment and run tests.

<table>
<tr>
  <td width="33%" valign="top"><h3>1 - Find something</h3><p>Browse <a href="https://github.com/leil-io/leilfs/labels/good%20first%20issue">good first issues</a>, or open one to describe what you have in mind.</p></td>
  <td width="33%" valign="top"><h3>2 - Build and test</h3><p>Follow the Quick Start to build locally, then run the test suite before opening a pull request.</p></td>
  <td width="33%" valign="top"><h3>3 - Open a PR</h3><p>Reference the issue, describe the change and what you tested. We respond on every PR.</p></td>
</tr>
</table>

<p>
  <a href="https://github.com/leil-io/leilfs/labels/good%20first%20issue"><img src="https://img.shields.io/github/issues/leil-io/leilfs/good%20first%20issue?style=flat-square&labelColor=141929&color=0FD4A2&label=good%20first%20issues" alt="Good first issues"></a>
  <a href="https://github.com/leil-io/leilfs/pulls"><img src="https://img.shields.io/github/issues-pr/leil-io/leilfs?style=flat-square&labelColor=141929&color=479DFF&label=open%20PRs" alt="Open PRs"></a>
  <a href="https://github.com/leil-io/leilfs/graphs/contributors"><img src="https://img.shields.io/github/contributors/leil-io/leilfs?style=flat-square&labelColor=141929&color=6F8297&label=contributors" alt="Contributors"></a>
</p>

<img src="https://s3.diaway.com/files/leil/divider.svg" width="100%">

## Contact us

Join our community chat on [Matrix](https://matrix.to/#/#leil:matrix.org) to connect with fellow **LeilFS** enthusiasts, developers and users.

<table>
<tr>
<td width="60%" valign="top">

<p><img src="https://img.shields.io/badge/-✓-0FD4A2?style=flat-square" alt="" height="10"> <strong>Ask questions:</strong> Get help, share experiences, and discuss <strong>LeilFS</strong> usage.</p>
<p><img src="https://img.shields.io/badge/-✓-0FD4A2?style=flat-square" alt="" height="10"> <strong>Discuss ideas:</strong> Propose features, improvements, and best practices.</p>
<p><img src="https://img.shields.io/badge/-✓-0FD4A2?style=flat-square" alt="" height="10"> <strong>Receive updates:</strong> Follow LeilFS development, releases, and community news.</p>

</td>
<td width="40%" valign="top">

<table>
<tr>
  <td><img src="https://img.shields.io/badge/Matrix-141929?style=flat-square&logo=matrix&logoColor=white" alt="Matrix" height="20"></td>
  <td><a href="https://matrix.to/#/#leil:matrix.org">#leil:matrix.org</a></td>
</tr>
<tr>
  <td><img src="https://img.shields.io/badge/Email-6F8297?style=flat-square&logo=maildotru&logoColor=white" alt="Email" height="20"></td>
  <td><a href="mailto:hello@leil.io">hello@leil.io</a></td>
</tr>
<tr>
  <td><img src="https://img.shields.io/badge/Website-479DFF?style=flat-square&logo=googleearth&logoColor=white" alt="Website" height="20"></td>
  <td><a href="https://leil.io">leil.io</a></td>
</tr>
</table>

</td>
</tr>
</table>

<img src="https://s3.diaway.com/files/leil/divider.svg" width="100%">

## Licensing

Most of the software is licensed under **GPLv3**, except the **Ganesha FSAL**, which is
licensed **LGPLv3** and located under `src/nfs-ganesha/`. See the [FSAL LICENSE
file](src/nfs-ganesha/LICENSE) for more info.

<div align="center">
  <img src="https://s3.diaway.com/files/leil/divider.svg" width="100%">
  <p>
    <picture>
      <source media="(prefers-color-scheme: dark)" srcset="https://s3.diaway.com/files/leil/logo-leil-negative.svg">
      <img alt="Leil" src="https://s3.diaway.com/files/leil/logo-leil-positive.svg" height="26">
    </picture>
  </p>
  <p><strong>Thank you from the LeilFS team.</strong></p>
  <p><sub>Engineered in Estonia · © 2026 Leil Storage OÜ</sub></p>
</div>
