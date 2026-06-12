ARG BASE_IMAGE=ubuntu:24.04
FROM ${BASE_IMAGE} as base

ARG DEBIAN_FRONTEND=noninteractive
ENV TZ=Europe/Madrid
ENV LC_ALL=C.UTF-8

RUN set -e ; \
    if ! type tini > /dev/null 2>&1; then \
        apt update; apt install -y --no-install-recommends tini ; \
        rm -rf /var/lib/apt/lists/* ; \
    fi ;

COPY --chmod=755 services/base/docker_entrypoint.sh /usr/local/bin/entrypoint.sh
ENTRYPOINT ["/usr/bin/tini", "--", "/usr/local/bin/entrypoint.sh"]

COPY --chmod=755 services/base/docker_healthcheck.sh /usr/local/bin/base-healthcheck.sh
HEALTHCHECK --interval=30s --timeout=3s \
    CMD /usr/local/bin/base-healthcheck.sh || exit 1

FROM base AS ganesha-builder

ARG GANESHA_VERSION=9.15
ENV GANESHA_VERSION=${GANESHA_VERSION}
ARG NTIRPC_VERSION=7.2
ENV NTIRPC_VERSION=${NTIRPC_VERSION}

# install ganesha from sources
RUN set -e ; \
    apt-get update; \
    apt-get install -y  --no-install-recommends \
          acl \
          asciidoc \
          bison \
          build-essential \
          byacc \
          cmake \
          dbus \
          docbook \
          docbook-xml \
          doxygen \
          fio \
          flex \
          krb5-user \
          libacl1-dev \
          libblkid-dev \
          libboost-filesystem-dev \
          libboost-iostreams-dev \
          libboost-program-options-dev \
          libboost-system-dev \
          libcap-dev \
          libdbus-1-dev \
          libgssapi-krb5-2 \
          libibverbs-dev \
          libjemalloc-dev \
          libjudy-dev \
          libkrb5-dev \
          librdmacm-dev \
          libkrb5support0 \
          libnfsidmap-dev \
          libnsl-dev \
          libsqlite3-dev \
          liburcu-dev \
          pkg-config \
          pv \
          wget \
          unzip \
          rpcbind \
    ; \
    rm -rf /var/lib/apt/lists/*

RUN set -e ; \
    (cd /usr/src ; \
        wget https://github.com/nfs-ganesha/nfs-ganesha/archive/V${GANESHA_VERSION}.zip ; \
        unzip V${GANESHA_VERSION}.zip ; \
        wget https://github.com/nfs-ganesha/ntirpc/archive/v${NTIRPC_VERSION}.zip ; \
        unzip v${NTIRPC_VERSION}.zip ; \
        rm -f V${GANESHA_VERSION}.zip v${NTIRPC_VERSION}.zip ; \
        rm -r nfs-ganesha-${GANESHA_VERSION}/src/libntirpc ; \
        ln -s /usr/src/ntirpc-${NTIRPC_VERSION} nfs-ganesha-${GANESHA_VERSION}/src/libntirpc ; \
        mkdir nfs-ganesha-${GANESHA_VERSION}/build ; \
        cmake -Bnfs-ganesha-${GANESHA_VERSION}/build \
            -DCMAKE_INSTALL_PREFIX=/ \
            -DUSE_9P=NO \
            -DUSE_FSAL_CEPH=NO \
            -DUSE_FSAL_GLUSTER=NO \
            -DUSE_FSAL_GPFS=NO \
            -DUSE_FSAL_KVSFS=NO \
            -DUSE_FSAL_LIZARDFS=NO \
            -DUSE_FSAL_LUSTRE=NO \
            -DUSE_FSAL_PROXY_V3=NO \
            -DUSE_FSAL_PROXY_V4=NO \
            -DUSE_FSAL_RGW=NO \
            -DUSE_FSAL_XFS=NO \
            -DUSE_GSS=YES \
            -DUSE_NFS_RDMA=ON \
            -Snfs-ganesha-${GANESHA_VERSION}/src; \
        make -Cnfs-ganesha-${GANESHA_VERSION}/build -j$(($(nproc)*3/4+1)) install ; \
        rm -rf nfs-ganesha-${GANESHA_VERSION} ntirpc-${NTIRPC_VERSION} ; \
    ); \
    mkdir -p /tmp/dependencies; \
    find /usr/ -name ganesha.nfsd -type f -exec ldd {} \; | \
        grep -oP '(/usr)?(/local)?/lib(64)?/.*\s' | sed 's/\s*$//' | sort -u | \
        xargs -I{} cp -v --parents {} /tmp/dependencies; \
    find /usr/ -name 'libboost_*.so*' -type f | grep -oP '(/usr)?(/local)?/lib(64)?/.*' | sort -u | \
        xargs -I{} cp -v --parents {} /tmp/dependencies;

FROM base AS dbus

RUN set -e ; \
    if ! type dbus-daemon > /dev/null 2>&1; then \
        apt update; apt install -y --no-install-recommends dbus ; \
        rm -rf /var/lib/apt/lists/* ; \
    fi ;
COPY --chmod=0755 services/dbus/entrypoint.sh /docker-entrypoint.d/10-dbus.sh
COPY --chmod=0755 services/dbus/healthcheck.sh /healthcheck.d/10-dbus.sh

FROM dbus AS rpcbind

RUN set -e ; \
    if ! type rpcbind > /dev/null 2>&1; then \
        apt update; apt install -y --no-install-recommends rpcbind ; \
        rm -rf /var/lib/apt/lists/* ; \
    fi ;
COPY --chmod=0755 services/rpcbind/entrypoint.sh /docker-entrypoint.d/20-rpcbind.sh
COPY --chmod=0755 services/rpcbind/healthcheck.sh /healthcheck.d/20-rpcbind.sh

FROM rpcbind AS rpc-statd

RUN set -e ; \
    if ! type rpc.statd > /dev/null 2>&1; then \
        apt update; apt install -y --no-install-recommends nfs-common ; \
        rm -rf /var/lib/apt/lists/* ; \
    fi ;

COPY --chmod=0755 services/rpc-statd/entrypoint.sh /docker-entrypoint.d/30-rpc-statd.sh
COPY --chmod=0755 services/rpc-statd/healthcheck.sh /healthcheck.d/30-rpc-statd.sh

FROM rpc-statd AS ganesha

COPY --from=ganesha-builder /usr/bin/ganesha.nfsd /usr/bin/ganesha.nfsd
COPY --from=ganesha-builder /etc/ganesha /etc/ganesha
COPY --from=ganesha-builder /usr/lib/ganesha /usr/lib/ganesha
COPY --from=ganesha-builder /tmp/dependencies/lib /lib
COPY --from=ganesha-builder /tmp/dependencies/lib64 /lib64
COPY --from=ganesha-builder /tmp/dependencies/usr/lib /usr/lib

RUN set -e ; \
    apt update; \
    apt install -y --no-install-recommends \
        keyutils \
        krb5-admin-server \
        krb5-kdc \
        nfs-common ; \
    rm -rf /var/lib/apt/lists/* ; \
    true

COPY --chmod=0755 services/ganesha/entrypoint.sh /docker-entrypoint.d/99-ganesha.sh
COPY --chmod=0755 services/ganesha/healthcheck.sh /healthcheck.d/99-ganesha.sh
