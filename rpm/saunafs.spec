%define distro @DISTRO@

Summary:        SaunaFS - distributed, fault tolerant file system
Name:           saunafs
Version:        3.13.0
Release:        0%{?distro}
License:        GPL v3
Group:          System Environment/Daemons
URL:            http://www.saunafs.org/
Source:         saunafs-%{version}.tar.gz
BuildRequires:  fuse-devel
BuildRequires:  cmake
BuildRequires:  pkgconfig
BuildRequires:  zlib-devel
BuildRequires:  asciidoc
BuildRequires:  systemd
BuildRoot:      %{_tmppath}/%{name}-%{version}-%{release}-root-%(%{__id_u} -n)

%define         sau_project        saunafs
%define         sau_group          %{sau_project}
%define         sau_user           %{sau_project}
%define         sau_datadir        %{_localstatedir}/lib/%{sau_project}
%define         sau_confdir        %{_sysconfdir}/%{sau_project}
%define         sau_limits_conf    /etc/security/limits.d/10-saunafs.conf
%define         sau_pam_d          /etc/pam.d/saunafs
%define         _unpackaged_files_terminate_build 0
%define         debug_package      %{nil}

%description
SaunaFS is an Open Source, easy to deploy and maintain, distributed,
fault tolerant file system for POSIX compliant OSes.
http://saunafs.com

# Packages
############################################################

%package master
Summary:        SaunaFS master server
Group:          System Environment/Daemons
Requires(post): systemd-units
Requires(preun): systemd-units
Requires(postun): systemd-units

%description master
SaunaFS master (metadata) server together with metarestore utility.

%package metalogger
Summary:        SaunaFS metalogger server
Group:          System Environment/Daemons
Requires(post): systemd-units
Requires(preun): systemd-units
Requires(postun): systemd-units

%description metalogger
SaunaFS metalogger (metadata replication) server.

%package chunkserver
Summary:        SaunaFS data server
Group:          System Environment/Daemons
Requires(post): systemd-units
Requires(preun): systemd-units
Requires(postun): systemd-units

%description chunkserver
SaunaFS data server.

%package client
Summary:        SaunaFS client
Group:          System Environment/Daemons
Requires:       fuse
Requires:       fuse-libs
Requires:       bash-completion

%description client
SaunaFS client: sfsmount and sfstools.

%package lib-client
Summary:        SaunaFS client C/C++ library
Group:          Development/Libraries

%description lib-client
SaunaFS client library for C/C++ bindings.

### Uncomment lines below to re-enable ganesha build.
# %package nfs-ganesha
# Summary:        SaunaFS plugin for nfs-ganesha
# Group:          System Environment/Libraries
# Requires:       saunafs-lib-client
#
# %description nfs-ganesha
# SaunaFS fsal plugin for nfs-ganesha.

%package cgi
Summary:        SaunaFS CGI Monitor
Group:          System Environment/Daemons
Requires:       python3

%description cgi
SaunaFS CGI Monitor.

%package cgiserv
Summary:        Simple CGI-capable HTTP server to run SaunaFS CGI Monitor
Group:          System Environment/Daemons
Requires:       %{name}-cgi = %{version}-%{release}
Requires(post): systemd-units
Requires(preun): systemd-units
Requires(postun): systemd-units

%description cgiserv
Simple CGI-capable HTTP server to run SaunaFS CGI Monitor.

%package adm
Summary:        SaunaFS administration utility
Group:          System Environment/Daemons

%description adm
SaunaFS command line administration utility.

%package uraft
Summary:        SaunaFS cluster management tool
Group:          System Environment/Daemons
Requires:       saunafs-master
Requires:       saunafs-adm
Requires:       boost-system
Requires:       boost-program-options

%description uraft
SaunaFS cluster management tool.

# Scriptlets - master
############################################################

%pre master
if ! getent group %{sau_group} > /dev/null 2>&1 ; then
	groupadd --system %{sau_group}
fi
if ! getent passwd %{sau_user} > /dev/null 2>&1 ; then
	adduser --system -g %{sau_group} --no-create-home --home-dir %{sau_datadir} %{sau_user}
fi
if [ ! -f %{sau_limits_conf} ]; then
	echo "%{sau_user} soft nofile 131072" > %{sau_limits_conf}
	echo "%{sau_user} hard nofile 131072" >> %{sau_limits_conf}
	chmod 0644 %{sau_limits_conf}
fi
if [ ! -f %{sau_pam_d} ]; then
	echo "session	required	pam_limits.so" > %{sau_pam_d}
fi
exit 0

%post master
%systemd_post saunafs-master.service

%preun master
%systemd_preun saunafs-master.service

%postun master
%systemd_postun_with_restart saunafs-master.service

# Scriptlets - metalogger
############################################################

%pre metalogger
if ! getent group %{sau_group} > /dev/null 2>&1 ; then
	groupadd --system %{sau_group}
fi
if ! getent passwd %{sau_user} > /dev/null 2>&1 ; then
	adduser --system -g %{sau_group} --no-create-home --home-dir %{sau_datadir} %{sau_user}
fi
exit 0

%post metalogger
%systemd_post saunafs-metalogger.service

%preun metalogger
%systemd_preun saunafs-metalogger.service

%postun metalogger
%systemd_postun_with_restart saunafs-metalogger.service

# Scriptlets - chunkserver
############################################################

%pre chunkserver
if ! getent group %{sau_group} > /dev/null 2>&1 ; then
	groupadd --system %{sau_group}
fi
if ! getent passwd %{sau_user} > /dev/null 2>&1 ; then
	adduser --system -g %{sau_group} --no-create-home --home-dir %{sau_datadir} %{sau_user}
fi
if [ ! -f %{sau_limits_conf} ]; then
	echo "%{sau_user} soft nofile 131072" > %{sau_limits_conf}
	echo "%{sau_user} hard nofile 131072" >> %{sau_limits_conf}
	chmod 0644 %{sau_limits_conf}
fi
if [ ! -f %{sau_pam_d} ]; then
	echo "session	required	pam_limits.so" > %{sau_pam_d}
fi
exit 0

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
echo "net.ipv4.conf.all.arp_accept = 1" > /etc/sysctl.d/10-saunafs-uraft-arp.conf
chmod 0664 /etc/sysctl.d/10-saunafs-uraft-arp.conf
sysctl -p /etc/sysctl.d/10-saunafs-uraft-arp.conf
echo "# Allow saunafs user to set floating ip" > /etc/sudoers.d/saunafs-uraft
echo "saunafs    ALL=NOPASSWD:/sbin/ip" >> /etc/sudoers.d/saunafs-uraft
echo 'Defaults !requiretty' >> /etc/sudoers

# Prep, build, install, files...
############################################################

%prep
%setup

%build
./configure --with-doc
make %{?_smp_mflags}

%install
rm -rf $RPM_BUILD_ROOT
make install DESTDIR=$RPM_BUILD_ROOT
install -d -m755 $RPM_BUILD_ROOT/%{sau_confdir}
install -d -m755 $RPM_BUILD_ROOT/%{_unitdir}
for f in rpm/service-files/*.service ; do
	install -m644 "$f" $RPM_BUILD_ROOT/%{_unitdir}/$(basename "$f")
done

%clean
rm -rf $RPM_BUILD_ROOT

%files master
%define sau_master_examples %{_docdir}/saunafs-master/examples
%defattr(644,root,root,755)
%doc NEWS README.md UPGRADE
%attr(755,root,root) %{_sbindir}/sfsmaster
%attr(755,root,root) %{_sbindir}/sfsrestoremaster
%attr(755,root,root) %{_sbindir}/sfsmetadump
%attr(755,root,root) %{_sbindir}/sfsmetarestore
%dir %{sau_confdir}
%attr(755,%{sau_user},%{sau_group}) %dir %{sau_confdir}
%attr(755,%{sau_user},%{sau_group}) %dir %{sau_datadir}
%{_mandir}/man5/sfsexports.cfg.5*
%{_mandir}/man5/sfstopology.cfg.5*
%{_mandir}/man5/sfsgoals.cfg.5*
%{_mandir}/man5/sfsmaster.cfg.5*
%{_mandir}/man5/sfsglobaliolimits.cfg.5*
%{_mandir}/man7/sfs.7*
%{_mandir}/man7/saunafs.7*
%{_mandir}/man8/sfsmaster.8*
%{_mandir}/man8/sfsmetadump.8*
%{_mandir}/man8/sfsmetarestore.8*
%{_mandir}/man8/sfsrestoremaster.8*
%{sau_master_examples}/sfsexports.cfg
%{sau_master_examples}/sfstopology.cfg
%{sau_master_examples}/sfsgoals.cfg
%{sau_master_examples}/sfsmaster.cfg
%{sau_master_examples}/sfsglobaliolimits.cfg
%attr(644,root,root) %{sau_datadir}/metadata.sfs.empty
%attr(644,root,root) %{_unitdir}/saunafs-master.service

%files metalogger
%define sau_metalogger_examples %{_docdir}/saunafs-metalogger/examples
%defattr(644,root,root,755)
%doc NEWS README.md UPGRADE
%attr(755,root,root) %{_sbindir}/sfsmetalogger
%attr(755,%{sau_user},%{sau_group}) %dir %{sau_datadir}
%{_mandir}/man5/sfsmetalogger.cfg.5*
%{_mandir}/man8/sfsmetalogger.8*
%{sau_metalogger_examples}/sfsmetalogger.cfg
%attr(644,root,root) %{_unitdir}/saunafs-metalogger.service

%files chunkserver
%define sau_chunkserver_examples %{_docdir}/saunafs-chunkserver/examples
%defattr(644,root,root,755)
%doc NEWS README.md UPGRADE
%attr(755,root,root) %{_sbindir}/sfschunkserver
%dir %{sau_confdir}
%attr(755,%{sau_user},%{sau_group}) %dir %{sau_confdir}
%attr(755,%{sau_user},%{sau_group}) %dir %{sau_datadir}
%{_mandir}/man5/sfschunkserver.cfg.5*
%{_mandir}/man5/sfshdd.cfg.5*
%{_mandir}/man8/sfschunkserver.8*
%{sau_chunkserver_examples}/sfschunkserver.cfg
%{sau_chunkserver_examples}/sfshdd.cfg
%attr(644,root,root) %{_unitdir}/saunafs-chunkserver.service

%files client
%define sau_client_examples %{_docdir}/saunafs-client/examples
%defattr(644,root,root,755)
%doc NEWS README.md UPGRADE
%attr(755,root,root) %{_bindir}/saunafs
%attr(755,root,root) %{_bindir}/sfsmount
%attr(755,root,root) %{_bindir}/sfstools.sh
%{_bindir}/sfsappendchunks
%{_bindir}/sfscheckfile
%{_bindir}/sfsdeleattr
%{_bindir}/sfsdirinfo
%{_bindir}/sfsfileinfo
%{_bindir}/sfsfilerepair
%{_bindir}/sfsgeteattr
%{_bindir}/sfsgetgoal
%{_bindir}/sfsgettrashtime
%{_bindir}/sfsmakesnapshot
%{_bindir}/sfsrepquota
%{_bindir}/sfsrgetgoal
%{_bindir}/sfsrgettrashtime
%{_bindir}/sfsrsetgoal
%{_bindir}/sfsrsettrashtime
%{_bindir}/sfsseteattr
%{_bindir}/sfssetgoal
%{_bindir}/sfssetquota
%{_bindir}/sfssettrashtime
%{_mandir}/man1/saunafs-appendchunks.1*
%{_mandir}/man1/saunafs-checkfile.1*
%{_mandir}/man1/saunafs-deleattr.1*
%{_mandir}/man1/saunafs-dirinfo.1*
%{_mandir}/man1/saunafs-fileinfo.1*
%{_mandir}/man1/saunafs-filerepair.1*
%{_mandir}/man1/saunafs-geteattr.1*
%{_mandir}/man1/saunafs-getgoal.1*
%{_mandir}/man1/saunafs-gettrashtime.1*
%{_mandir}/man1/saunafs-makesnapshot.1*
%{_mandir}/man1/saunafs-repquota.1*
%{_mandir}/man1/saunafs-rgetgoal.1*
%{_mandir}/man1/saunafs-rgettrashtime.1*
%{_mandir}/man1/saunafs-rsetgoal.1*
%{_mandir}/man1/saunafs-rsettrashtime.1*
%{_mandir}/man1/saunafs-seteattr.1*
%{_mandir}/man1/saunafs-setgoal.1*
%{_mandir}/man1/saunafs-setquota.1*
%{_mandir}/man1/saunafs-settrashtime.1*
%{_mandir}/man1/saunafs-rremove.1*
%{_mandir}/man1/saunafs.1*
%{_mandir}/man5/sfsiolimits.cfg.5*
%{_mandir}/man7/sfs.7*
%{_mandir}/man1/sfsmount.1*
%{_mandir}/man5/sfsmount.cfg.5*
%{sau_client_examples}/sfsiolimits.cfg
%{sau_client_examples}/sfsmount.cfg
%{_sysconfdir}/bash_completion.d/saunafs

%files lib-client
%{_libdir}/libsaunafsmount_shared.so
%{_libdir}/libsaunafs-client.so
%{_libdir}/libsaunafs-client-cpp.a
%{_libdir}/libsaunafs-client-cpp_pic.a
%{_libdir}/libsaunafs-client.a
%{_libdir}/libsaunafs-client_pic.a
%{_includedir}/saunafs/saunafs_c_api.h
%{_includedir}/saunafs/saunafs_error_codes.h

### Uncomment lines below to re-enable ganesha build.
# %files nfs-ganesha
# %{_libdir}/ganesha/libfsalsaunafs.so

%files cgi
%defattr(644,root,root,755)
%doc NEWS README.md UPGRADE
%dir %{_datadir}/sfscgi
%{_datadir}/sfscgi/err.gif
%{_datadir}/sfscgi/favicon.ico
%{_datadir}/sfscgi/favicon.svg
%{_datadir}/sfscgi/index.html
%{_datadir}/sfscgi/logomini.svg
%{_datadir}/sfscgi/logomini.png
%{_datadir}/sfscgi/sfs.css
%attr(755,root,root) %{_datadir}/sfscgi/sfs.cgi
%attr(755,root,root) %{_datadir}/sfscgi/chart.cgi

%files cgiserv
%defattr(644,root,root,755)
%attr(755,root,root) %{_sbindir}/saunafs-cgiserver
%attr(755,root,root) %{_sbindir}/sfscgiserv
%{_mandir}/man8/saunafs-cgiserver.8*
%{_mandir}/man8/sfscgiserv.8*
%attr(644,root,root) %{_unitdir}/saunafs-cgiserv.service

%files adm
%defattr(644,root,root,755)
%doc NEWS README.md UPGRADE
%attr(755,root,root) %{_bindir}/saunafs-admin
%{_mandir}/man8/saunafs-admin.8*
%{_bindir}/saunafs-probe
%{_mandir}/man8/saunafs-probe.8*

%files uraft
%define sau_uraft_examples %{_docdir}/saunafs-uraft/examples
%defattr(644,root,root,755)
%attr(755,root,root) %{_sbindir}/saunafs-uraft
%attr(755,root,root) %{_sbindir}/saunafs-uraft-helper
%doc NEWS README.md UPGRADE
%{_mandir}/man8/saunafs-uraft.8*
%{_mandir}/man8/saunafs-uraft-helper.8*
%{_mandir}/man5/saunafs-uraft.cfg.5*
%{sau_uraft_examples}/saunafs-uraft.cfg
%attr(644,root,root) %{_unitdir}/saunafs-uraft.service
%attr(644,root,root) %{_unitdir}/saunafs-ha-master.service

%changelog
