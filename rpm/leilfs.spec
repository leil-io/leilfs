Name:           leilfs
Summary:        Distributed, fault tolerant POSIX file system
Version:        5.10.0
Release:        %autorelease

# Most of the software is licensed under GPL-3.0, except:
# `cmake/FindSocket.cmake`, which is licensed GPL-3.0+;
# `utils/wireshark/plugins/epan/saunafs/CMakeLists.txt`, which is licensed GPL-2.0+;
# `tests/llvm.sh` which is licensed Apache-2.0 WITH LLVM-exception;
# `src/nfs-ganesha/*`, which is licensed LGPL-3.0+;
# `src/nfs-ganesha/CMakeLists.txt`, which is licensed GPL-3.0;
# `external/crcutil-1.0/*`, which is licensed Apache-2.0;
# `utils/ping_pong.cc`, which is licensed GPL-3.0+;
# `src/common/galois_field_isal.cc`, which is licensed BSD-3-Clause~Intel;
# `src/common/coroutine.h`, which is Boost license.
License:        GPL-3.0-only AND GPL-3.0-or-later AND GPL-2.0-or-later AND Apache-2.0 WITH LLVM-exception AND LGPL-3.0-or-later AND Apache-2.0 AND BSD-3-Clause AND BSL-1.0 AND MIT

URL:            https://github.com/leil-io/leilfs
Source0:        https://github.com/leil-io/%{name}/archive/refs/tags/v%{version}.tar.gz
Source1:        10-leilfs-uraft-arp.conf
Source2:        leilfs-uraft.sudoers
Source3:        10-leilfs.conf
Source4:        leilfs.conf

BuildRequires:  asciidoc
BuildRequires:  cmake
BuildRequires:  gcc
BuildRequires:  gcc-c++
BuildRequires:  make
BuildRequires:  pkgconfig
BuildRequires:  rubygem-asciidoctor
BuildRequires:  systemd
BuildRequires:  systemd-rpm-macros

BuildRequires:  boost-devel
BuildRequires:  fmt-devel
BuildRequires:  fuse3-devel
BuildRequires:  isa-l-devel
BuildRequires:  Judy-devel
BuildRequires:  libzstd-devel
BuildRequires:  openssl-devel
BuildRequires:  pam-devel
BuildRequires:  spdlog-devel
BuildRequires:  systemd-devel
BuildRequires:  yaml-cpp-devel
BuildRequires:  zlib-devel

%global         leil_project        saunafs
%global         leil_group          %{leil_project}
%global         leil_user           %{leil_project}
%global         leil_datadir        %{_localstatedir}/lib/%{leil_project}
%global         leil_confdir        %{_sysconfdir}/%{leil_project}
%global         leil_limits_conf    10-leilfs.conf

%description
LeilFS is an Open Source, easy to deploy and maintain, distributed,
fault tolerant file system for POSIX compliant OSes.
http://leil.io

# Package - master
############################################################

%package master
Summary:        LeilFS master server
Requires:       user(saunafs)
Requires:       group(saunafs)
%{?systemd_requires}

%description master
LeilFS master (metadata) server together with metarestore utility.

# Package - metalogger
############################################################

%package metalogger
Summary:        LeilFS metalogger server
Requires:       user(saunafs)
Requires:       group(saunafs)
%{?systemd_requires}

%description metalogger
LeilFS metalogger (metadata replication) server.

# Package - chunkserver
############################################################

%package chunkserver
Summary:        LeilFS data server
Requires:       user(saunafs)
Requires:       group(saunafs)
%{?systemd_requires}

%description chunkserver
LeilFS data server.

# Package - client
############################################################

%package client
Summary:        LeilFS client
Requires:       fuse3
Requires:       bash-completion

%description client
LeilFS client: sfsmount and sfstools.

# Package - LeilFS client development files
############################################################

%package client-devel
Summary:        Development files for LeilFS client C/C++

%description client-devel
LeilFS development headers for C/C++ bindings.

# Package - LeilFS client development files
############################################################

%package client-static
Summary:        Static libraries for LeilFS client C/C++

%description client-static
LeilFS development static libraries for C/C++ bindings.

# Package - CGI
############################################################

%package cgi
Summary:        LeilFS CGI Monitor
BuildArch:      noarch
Requires:       python3

%description cgi
LeilFS CGI Monitor.

# Package - CGI server
############################################################

%package cgiserv
Summary:        Simple CGI-capable HTTP server to run LeilFS CGI Monitor
BuildArch:      noarch
Requires:       %{name}-cgi = %{version}-%{release}
%{?systemd_requires}

%description cgiserv
Simple CGI-capable HTTP server to run LeilFS CGI Monitor.

# Package - Administration utility
############################################################

%package adm
Summary:        LeilFS administration utility

%description adm
LeilFS command line administration utility.

# Package - uraft
############################################################

%package uraft
Summary:        LeilFS cluster management tool
Requires:       %{name}-master = %{version}-%{release}
Requires:       %{name}-adm = %{version}-%{release}
Requires:       iproute

%description uraft
LeilFS cluster management tool.

# Package - user
############################################################

%package user
Summary:        LeilFS common user/group
%{?sysusers_requires_compat}

%description user
LeilFS common user/group.

# Scriptlets - master
############################################################

%post master
%systemd_post saunafs-master.service

%preun master
%systemd_preun saunafs-master.service

%postun master
%systemd_postun_with_restart saunafs-master.service

# Scriptlets - metalogger
############################################################

%post metalogger
%systemd_post saunafs-metalogger.service

%preun metalogger
%systemd_preun saunafs-metalogger.service

%postun metalogger
%systemd_postun_with_restart saunafs-metalogger.service

# Scriptlets - chunkserver
############################################################

%post chunkserver
%systemd_post saunafs-chunkserver.service

%preun chunkserver
%systemd_preun saunafs-chunkserver.service

%postun chunkserver
%systemd_postun_with_restart saunafs-chunkserver.service

# Scriptlets - CGI server
############################################################

%post cgiserv
%systemd_post saunafs-cgiserv.service

%preun cgiserv
%systemd_preun saunafs-cgiserv.service

%postun cgiserv
%systemd_postun_with_restart saunafs-cgiserv.service

# Scriptlets - uraft
############################################################

%post uraft
%systemd_post saunafs-uraft.service saunafs-ha-master.service

%preun uraft
%systemd_preun saunafs-uraft.service saunafs-ha-master.service

%postun uraft
%systemd_postun_with_restart saunafs-uraft.service saunafs-ha-master.service

# Scriptlets - user
############################################################

%pre user
%sysusers_create_compat %{SOURCE4}

# Prep
############################################################

%prep
%autosetup -p1

# Remove /usr/bin/env from bash scripts
find . -type f -name "*.sh" -exec sed -i 's@#!/usr/bin/env bash@#!/bin/bash@' {} +
for i in src/data/postinst.in \
         src/master/sfsrestoremaster.in \
         src/unittests/unittests.in tests/ci_build/docker_entrypoint.test; do
    sed -i 's@#!/usr/bin/env bash@#!/bin/bash@' $i
done

# Remove /usr/bin/env from python3 scripts
for i in src/cgi/chart.cgi.in \
         src/cgi/leil-cgiserver.py.in \
         src/cgi/leil.cgi.in tests/data/extract_tests_durations.py \
         tests/test_utils/sqlite_stress_test.py \
         utils/wireshark/plugins/epan/saunafs/make_dissector.py; do
    sed -i 's@#!/usr/bin/env python3@#!/usr/bin/python3@' $i
done

# Build
############################################################

%build
%cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF \
    -DENABLE_CLIENT_LIB=ON \
    -DENABLE_COMPILE_COMMANDS=OFF \
    -DENABLE_PROMETHEUS=OFF \
    -DASCIIDOCTOR_AUTO_SETUP=OFF \
    -DGENERATE_GIT_INFO=OFF

%cmake_build

# Install
############################################################

%install
%cmake_install

mkdir -p %{buildroot}%{_datadir}/bash-completion/completions/

mv %{buildroot}%{_sysconfdir}/bash_completion.d/leil \
    %{buildroot}%{_sysconfdir}/bash_completion.d/saunafs \
    %{buildroot}%{_datadir}/bash-completion/completions/

rm -rf %{buildroot}%{_sysconfdir}/bash_completion.d/

install -p -m 0644 -D %{SOURCE1} %{buildroot}%{_sysconfdir}/sysctl.d/10-leilfs-uraft-arp.conf

install -p -m 0440 -D %{SOURCE2} %{buildroot}%{_sysconfdir}/sudoers.d/leilfs-uraft

install -p -d -m 0755 %{buildroot}%{_unitdir}
for f in rpm/service-files/*.service ; do
    # Remove this when saunafs-uraft.saunafs-ha-master.service is no longer in service-files
    if [ "$(basename "$f")" = "saunafs-uraft.saunafs-ha-master.service" ]; then
        continue
    fi
    install -p -m 0644 "$f" %{buildroot}%{_unitdir}/
done

install -p -m 0644 -D %{SOURCE3} %{buildroot}%{_sysconfdir}/security/limits.d/%{leil_limits_conf}

install -p -m 0644 -D %{SOURCE4} %{buildroot}%{_sysusersdir}/leilfs.conf

mkdir -p %{buildroot}%{leil_confdir}

# Not used anywhere, can be removed
rm -f %{buildroot}%{_libdir}/libsaunafs-client.so
rm -f %{buildroot}%{_libdir}/libsaunafsmount_shared.so

# Files - master
############################################################

%files master
%license COPYING
%doc NEWS README.md
%{_sbindir}/leil-master
%{_sbindir}/sfsmaster
%{_sbindir}/leil-restoremaster
%{_sbindir}/sfsrestoremaster
%{_sbindir}/leil-metadump
%{_sbindir}/sfsmetadump
%{_sbindir}/leil-metarestore
%{_sbindir}/sfsmetarestore
%{_unitdir}/saunafs-master.service
%dir %{leil_confdir}
%attr(-,%{leil_user},%{leil_group}) %dir %{leil_datadir}
%attr(-,%{leil_user},%{leil_group}) %{leil_datadir}/metadata.sfs.empty
%{_mandir}/man5/sfsexports.cfg.5*
%{_mandir}/man5/leil-exports.cfg.5*
%{_mandir}/man5/sfstopology.cfg.5*
%{_mandir}/man5/leil-topology.cfg.5*
%{_mandir}/man5/sfsgoals.cfg.5*
%{_mandir}/man5/leil-goals.cfg.5*
%{_mandir}/man5/sfsmaster.cfg.5*
%{_mandir}/man5/leil-master.cfg.5*
%{_mandir}/man5/sfsglobaliolimits.cfg.5*
%{_mandir}/man5/leil-globaliolimits.cfg.5*
%{_mandir}/man7/sfs.7*
%{_mandir}/man7/leilfs.7*
%{_mandir}/man7/leil.7*
%{_mandir}/man7/saunafs.7*
%{_mandir}/man8/sfsmaster.8*
%{_mandir}/man8/leil-master.8*
%{_mandir}/man8/sfsmetadump.8*
%{_mandir}/man8/leil-metadump.8*
%{_mandir}/man8/sfsmetarestore.8*
%{_mandir}/man8/leil-metarestore.8*
%{_mandir}/man8/sfsrestoremaster.8*
%{_mandir}/man8/leil-restoremaster.8*
%dir %{_docdir}/saunafs-master/
%dir %{_docdir}/saunafs-master/examples/
%{_docdir}/saunafs-master/examples/sfsexports.cfg
%{_docdir}/saunafs-master/examples/leil-exports.cfg
%{_docdir}/saunafs-master/examples/sfstopology.cfg
%{_docdir}/saunafs-master/examples/leil-topology.cfg
%{_docdir}/saunafs-master/examples/sfsgoals.cfg
%{_docdir}/saunafs-master/examples/leil-goals.cfg
%{_docdir}/saunafs-master/examples/sfsmaster.cfg
%{_docdir}/saunafs-master/examples/leil-master.cfg
%{_docdir}/saunafs-master/examples/sfsglobaliolimits.cfg
%{_docdir}/saunafs-master/examples/leil-globaliolimits.cfg
%config(noreplace) %{_sysconfdir}/pam.d/saunafs
%config(noreplace) %{_sysconfdir}/security/limits.d/%{leil_limits_conf}

# Files - metalogger
############################################################

%files metalogger
%license COPYING
%doc NEWS README.md
%{_sbindir}/leil-metalogger
%{_sbindir}/sfsmetalogger
%{_unitdir}/saunafs-metalogger.service
%dir %{leil_confdir}
%attr(-,%{leil_user},%{leil_group}) %dir %{leil_datadir}
%{_mandir}/man5/sfsmetalogger.cfg.5*
%{_mandir}/man5/leil-metalogger.cfg.5*
%{_mandir}/man8/sfsmetalogger.8*
%{_mandir}/man8/leil-metalogger.8*
%dir %{_docdir}/saunafs-metalogger/
%dir %{_docdir}/saunafs-metalogger/examples/
%{_docdir}/saunafs-metalogger/examples/sfsmetalogger.cfg
%{_docdir}/saunafs-metalogger/examples/leil-metalogger.cfg

# Files - chunkserver
############################################################

%files chunkserver
%license COPYING
%doc NEWS README.md
%{_sbindir}/leil-chunkserver
%{_sbindir}/sfschunkserver
%{_unitdir}/saunafs-chunkserver.service
%dir %{leil_confdir}
%attr(-,%{leil_user},%{leil_group}) %dir %{leil_datadir}
%{_mandir}/man5/sfschunkserver.cfg.5*
%{_mandir}/man5/leil-chunkserver.cfg.5*
%{_mandir}/man5/sfshdd.cfg.5*
%{_mandir}/man5/leil-hdd.cfg.5*
%{_mandir}/man8/sfschunkserver.8*
%{_mandir}/man8/leil-chunkserver.8*
%dir %{_docdir}/saunafs-chunkserver/
%dir %{_docdir}/saunafs-chunkserver/examples/
%{_docdir}/saunafs-chunkserver/examples/sfschunkserver.cfg
%{_docdir}/saunafs-chunkserver/examples/leil-chunkserver.cfg
%{_docdir}/saunafs-chunkserver/examples/sfshdd.cfg
%{_docdir}/saunafs-chunkserver/examples/leil-hdd.cfg
%config(noreplace) %{_sysconfdir}/pam.d/saunafs
%config(noreplace) %{_sysconfdir}/security/limits.d/%{leil_limits_conf}

# Files - client
############################################################

%files client
%license COPYING
%doc NEWS README.md
%{_bindir}/leil
%{_bindir}/saunafs
%{_bindir}/leil-mount
%{_bindir}/sfsmount
%dir %{leil_confdir}
%{_mandir}/man1/leil-appendchunks.1*
%{_mandir}/man1/saunafs-appendchunks.1*
%{_mandir}/man1/leil-checkfile.1*
%{_mandir}/man1/saunafs-checkfile.1*
%{_mandir}/man1/leil-deleattr.1*
%{_mandir}/man1/saunafs-deleattr.1*
%{_mandir}/man1/leil-dirinfo.1*
%{_mandir}/man1/saunafs-dirinfo.1*
%{_mandir}/man1/leil-fileinfo.1*
%{_mandir}/man1/saunafs-fileinfo.1*
%{_mandir}/man1/leil-filerepair.1*
%{_mandir}/man1/saunafs-filerepair.1*
%{_mandir}/man1/leil-geteattr.1*
%{_mandir}/man1/saunafs-geteattr.1*
%{_mandir}/man1/leil-getgoal.1*
%{_mandir}/man1/saunafs-getgoal.1*
%{_mandir}/man1/leil-gettrashtime.1*
%{_mandir}/man1/saunafs-gettrashtime.1*
%{_mandir}/man1/leil-makesnapshot.1*
%{_mandir}/man1/saunafs-makesnapshot.1*
%{_mandir}/man1/leil-repquota.1*
%{_mandir}/man1/saunafs-repquota.1*
%{_mandir}/man1/leil-seteattr.1*
%{_mandir}/man1/saunafs-seteattr.1*
%{_mandir}/man1/leil-setgoal.1*
%{_mandir}/man1/saunafs-setgoal.1*
%{_mandir}/man1/leil-setquota.1*
%{_mandir}/man1/saunafs-setquota.1*
%{_mandir}/man1/leil-settrashtime.1*
%{_mandir}/man1/saunafs-settrashtime.1*
%{_mandir}/man1/leil-rremove.1*
%{_mandir}/man1/saunafs-rremove.1*
%{_mandir}/man1/leil.1*
%{_mandir}/man1/saunafs.1*
%{_mandir}/man1/leil-mount.1*
%{_mandir}/man1/sfsmount.1*
%{_mandir}/man5/sfsiolimits.cfg.5*
%{_mandir}/man5/leil-iolimits.cfg.5*
%{_mandir}/man5/sfsmount.cfg.5*
%{_mandir}/man5/leil-mount.cfg.5*
%{_mandir}/man7/sfs.7*
%{_mandir}/man7/leilfs.7*
%{_mandir}/man7/leil.7*
%{_mandir}/man7/saunafs-migrations.7*
%{_mandir}/man7/leil-migrations.7*
%dir %{_docdir}/saunafs-client/
%dir %{_docdir}/saunafs-client/examples/
%{_docdir}/saunafs-client/examples/sfstls.cfg
%{_docdir}/saunafs-client/examples/leil-tls.cfg
%{_docdir}/saunafs-client/examples/sfsiolimits.cfg
%{_docdir}/saunafs-client/examples/leil-iolimits.cfg
%{_docdir}/saunafs-client/examples/sfsmount.cfg
%{_docdir}/saunafs-client/examples/leil-mount.cfg
%{_datadir}/bash-completion/completions/leil
%{_datadir}/bash-completion/completions/saunafs

# Files - client-devel
############################################################

%files client-devel
%doc NEWS README.md
%license COPYING
%dir %{_includedir}/saunafs/
%{_includedir}/saunafs/saunafs_c_api.h
%{_includedir}/saunafs/saunafs_error_codes.h

# Files - client-static
############################################################

%files client-static
%doc NEWS README.md
%license COPYING
%dir %{_includedir}/saunafs/
%{_libdir}/libsaunafs-client-cpp.a
%{_libdir}/libsaunafs-client-cpp_pic.a
%{_libdir}/libsaunafs-client.a
%{_libdir}/libsaunafs-client_pic.a

# Files - CGI
############################################################

%files cgi
%license COPYING
%doc NEWS README.md
%dir %{_datadir}/leil-cgi
%{_datadir}/leil-cgi/err.gif
%{_datadir}/leil-cgi/favicon.ico
%{_datadir}/leil-cgi/favicon.svg
%{_datadir}/leil-cgi/index.html
%{_datadir}/leil-cgi/logomini.svg
%{_datadir}/leil-cgi/logomini.png
%{_datadir}/leil-cgi/leil.css
%{_datadir}/leil-cgi/leil.cgi
%{_datadir}/leil-cgi/chart.cgi
%{_datadir}/leil-cgi/sfs.css
%{_datadir}/leil-cgi/sfs.cgi
%{_datadir}/sfscgi

# Files - CGI server
############################################################

%files cgiserv
%license COPYING
%doc NEWS README.md
%{_sbindir}/leil-cgiserver
%{_sbindir}/saunafs-cgiserver
%{_unitdir}/saunafs-cgiserv.service
%{_mandir}/man8/leil-cgiserver.8*
%{_mandir}/man8/saunafs-cgiserver.8*

# Files - Administration utility
############################################################

%files adm
%license COPYING
%doc NEWS README.md
%{_bindir}/leil-admin
%{_bindir}/saunafs-admin
%{_mandir}/man8/leil-admin.8*
%{_mandir}/man8/saunafs-admin.8*
%{_bindir}/leil-probe
%{_bindir}/saunafs-probe
%{_mandir}/man8/leil-probe.8*
%{_mandir}/man8/saunafs-probe.8*

# Files - uraft
############################################################

%files uraft
%license COPYING
%doc NEWS README.md
%{_sbindir}/leil-uraft
%{_sbindir}/saunafs-uraft
%{_sbindir}/leil-uraft-helper
%{_sbindir}/saunafs-uraft-helper
%{_unitdir}/saunafs-uraft.service
%{_unitdir}/saunafs-ha-master.service
%{_mandir}/man8/leil-uraft.8*
%{_mandir}/man8/saunafs-uraft.8*
%{_mandir}/man8/leil-uraft-helper.8*
%{_mandir}/man8/saunafs-uraft-helper.8*
%{_mandir}/man5/leil-uraft.cfg.5*
%{_mandir}/man5/saunafs-uraft.cfg.5*
%dir %{_docdir}/saunafs-uraft/
%dir %{_docdir}/saunafs-uraft/examples/
%{_docdir}/saunafs-uraft/examples/leil-uraft.cfg
%{_docdir}/saunafs-uraft/examples/saunafs-uraft.cfg
%config(noreplace) %{_sysconfdir}/sysctl.d/10-leilfs-uraft-arp.conf
%attr(0440, root, root) %config(noreplace) %{_sysconfdir}/sudoers.d/leilfs-uraft

# Files - user
############################################################

%files user
%license COPYING
%doc NEWS README.md
%{_sysusersdir}/leilfs.conf

%changelog
%autochangelog
