# LeilFS RPM Packaging
This directory contains the necessary files to build, test, and review RPM packages for leilfs on Fedora.

> [!WARNING]
> A lot of binaries, folder names and such are currently in "renaming" phase, so with each version naming of certain binaries, users, and folders changes.

## Directory Structure
File/Folder | Description
----------- | -----------
leilfs.spec | The "recipe" for the RPM. Defines metadata, dependencies, build steps, file placements, etc.
build.sh | Runs **rpmlint** on the *leilfs.spec* file, downloads source tarball and builds packages in a clean environment. Runs **rpmlint** on resulting packages. Results are generated in *results_leilfs/*.
review.sh | Wrapper for fedora-review to ensure the package meets Fedora standards. Uses packages built using ***build.sh*** script. The report is generated in *review-leilfs/*.
*.conf and *.sudoers | Configuration files (sysctl, uraft) bundled with the package.
filters.toml | Configuration for rpmlint to ignore specific non-critical warnings.
service-files/ | Systemd unit files and service configurations.

## Local Build and Review (Fedora Only)
Builds package locally using Mock to ensure a clean build environment.

### Install dependencies
```bash
sudo dnf install fedora-packager python3-specfile
```

* fedora-packager - different utilities used for fedora package building, linting and maintenance;
* python3-specfile - used for license validation. 

### Run the build script (takes around 20 minutes in CI)
```bash
sudo ./build.sh
```

The script creates the RPM source tarball from the current Git checkout. In CI,
this means the `LEILFS_REF` used for checkout is also the source reference that
gets packaged. If Jenkins leaves the repository in detached `HEAD` state, pass
the same checked-out branch, tag, or commit explicitly so the script can resolve
and verify it:
```bash
sudo ./build.sh dev
sudo ./build.sh v5.10.0
sudo ./build.sh 1e9e026dc6fa536ebbc0708a58a9947741b8c2c0
```
Branch arguments may resolve through local refs or `origin/<branch>`, but the
resolved ref must match the current checkout. To build another branch, tag, or
commit, check it out first.

### Run the review script (takes around 25-30 minutes in CI)
```bash
sudo ./review.sh
```

> [!NOTE]
> Every push to **feat-fedora-rpm** branch triggers a build and a subsequent review in a privileged Fedora container ([view workflow file](../.github/workflows/fedora-rpm-build-review.yml)). You can download the resulting review reports from the Actions tab ([view generated report in the Artifacts at the bottom of any workflow run](https://github.com/leil-io/leilfs/actions/workflows/fedora-rpm-build-review.yml)).

## Testing & Installation
### Install the local build
```bash
cd results_leilfs/{VERSION}/{RELEASE}/
sudo dnf install leilfs-{PACKAGE}-{VERSION}-{RELEASE}.x86_64.rpm leilfs-cgi-{VERSION}-{RELEASE}.noarch.rpm leilfs-cgiserv-{VERSION}-{RELEASE}.noarch.rpm
```
Don't install any debuginfo packages. Replace **VERSION** and **RELEASE** with appropriate version installed. Replace **PACKAGE** with (every package must be installed from the list): *adm, chunkserver, client, master, metalogger, uraft, user*.

After installation, follow [this quick guide on setting up local manual test](https://docs.leil.io/quick-start).

> [!IMPORTANT]
> Also copy sfsexports to /etc/saunafs:
```
sudo cp /usr/share/doc/saunafs-master/examples/sfsexports.cfg /etc/saunafs/
```

Navigate to the */mnt/client* after starting services and try to create and delete files to confirm the binaries are working.

### Verify services
```bash
systemctl status saunafs-master
systemctl status saunafs-chunkserver
```
> [!WARNING]
> Names of the service files are a subject to change due to Leil rebranding.

## Spec File Maintenance
### Bumping versions
When a new version is released, update the **Version:** field in the *.spec* file.

### Adding patches
If the latest version has errors during building, it is possible to create a new branch with fixes and generate patches from the commits pushed to the branch:
```bash
git format-patch -X
```
where X is how many commits from the last one to include (i.e -2 is generating 2 patches from 2 latest commits).
After generating commits, place them under *rpm/* and include them in a *.spec* file:
```vim
PatchX: 000Y-latest-commit-text.patch
```
where X is a number, usually starting from 0 and increasing with each new patch, Y is a number starting at 1 and increasing with every new patch.

## Getting into the official Fedora repos
If you intend to submit leilfs to the official Fedora repositories, follow the steps provided in the [new package process for new contributors' guide.](https://docs.fedoraproject.org/en-US/package-maintainers/New_Package_Process_for_New_Contributors/)

### Summarised steps for contribution
> [!WARNING]
> Use the email for registrations that you would definitely not lose access to (better use a new or a private and not a company one).
1. Create a [Fedora Account](https://accounts.fedoraproject.org/).
2. Sign in to your account on [Copr](copr.fedorainfracloud.org) using your previously created Fedora account (use OIDC login, press cancel in the pop-up, then enter username and password).
3. Follow [this tutorial](https://docs.copr.fedorainfracloud.org/screenshots_tutorial.html) to upload the .spec and .src.rpm files. Create a new build and use the upload instead of URLs. It is possible to test the build on multiple architectures, which is part of the review process.
4. Create a [Red Hat Bugzilla account](https://bugzilla.redhat.com/).
5. Submit a package review on Red Hat Bugzilla - [example](https://bugzilla.redhat.com/show_bug.cgi?id=2443729).

> [!NOTE]
> The steps are not fully known because the review was not yet submitted and sponsorship seeking is not yet possible.

## Future Tasks
- [ ] Rename binaries, folders, man pages, user and other parts in the .spec file to the correct Leil branding,
- [ ] Rename *.service files to Leil rebranding. Correctly install the new renamed service files in .spec,
- [ ] Remove from .spec file (only when removal is approved):
```bash
# Remove this when saunafs-uraft.saunafs-ha-master.service is no longer in service-files
if [ "$(basename "$f")" = "saunafs-uraft.saunafs-ha-master.service" ]; then
    continue
fi
```
- [ ] Rename the saunafs to leil in files: 10-leilfs.conf, leilfs.conf, leilfs-uraft.sudoers.
