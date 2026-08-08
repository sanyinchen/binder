# Self-contained build + runtime environment for the AOSP android-12 binder stack.
#
#   docker build -f docker/Dockerfile -t android-binder .
#   scripts/run.sh                  # runs the end-to-end demo
#
# Nothing outside this repository is needed at build time. At run time the
# container needs --privileged so it can load binder_linux and mount binderfs;
# the binder driver comes from the host kernel, which containers share.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        clang \
        lld \
        cmake \
        ninja-build \
        make \
        kmod \
        util-linux \
        procps \
        gdb \
        strace \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

ENV CC=clang CXX=clang++

WORKDIR /opt/binder
COPY . /opt/binder

RUN cmake -S /opt/binder -B /opt/binder/build-docker -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /opt/binder/build-docker -j "$(nproc)" \
    && cmake --install /opt/binder/build-docker --prefix /usr/local

RUN chmod +x /opt/binder/docker/entrypoint.sh

ENTRYPOINT ["/opt/binder/docker/entrypoint.sh"]
CMD ["demo"]
