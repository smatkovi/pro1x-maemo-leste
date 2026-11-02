FROM debian:bullseye
RUN rm -f /etc/apt/sources.list.d/* && printf '%s\n' 'deb http://archive.debian.org/debian bullseye main' 'deb http://archive.debian.org/debian-security bullseye-security main' > /etc/apt/sources.list && \
    apt-get -o Acquire::Check-Valid-Until=false update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential python2 clang-11 lld-11 llvm-11 gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu \
      uuid-dev make bc ca-certificates && rm -rf /var/lib/apt/lists/* && ln -s /usr/bin/python2 /usr/local/bin/python
