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

# Build NFS-Ganesha from the single shared source of truth: the version and the
# build procedure live in ganesha.env / build-ganesha.sh, and the build deps in
# ganesha-build-deps.txt. ntirpc is resolved from the release's git submodule, so
# there is no ntirpc version to pin here.
COPY ganesha.env build-ganesha.sh ganesha-build-deps.txt /opt/ganesha/

RUN set -e ; \
    apt-get update ; \
    grep -vE '^[[:space:]]*(#|$)' /opt/ganesha/ganesha-build-deps.txt \
        | xargs apt-get install -y --no-install-recommends ; \
    rm -rf /var/lib/apt/lists/*

RUN set -e ; \
    bash /opt/ganesha/build-ganesha.sh / ; \
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
        ibverbs-providers \
        keyutils \
        krb5-admin-server \
        krb5-kdc \
        nfs-common ; \
    rm -rf /var/lib/apt/lists/* ; \
    true

COPY --chmod=0755 services/ganesha/entrypoint.sh /docker-entrypoint.d/99-ganesha.sh
COPY --chmod=0755 services/ganesha/healthcheck.sh /healthcheck.d/99-ganesha.sh
