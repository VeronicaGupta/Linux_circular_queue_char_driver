FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    linux-image-generic \
    linux-headers-generic \
    kmod \
    qemu-system-x86 \
    busybox-static \
    cpio \
    gzip \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
COPY . /workspace
RUN chmod +x /workspace/docker/lab.sh \
    /workspace/docker/init-test.sh \
    /workspace/docker/init-shell.sh

ENTRYPOINT ["/workspace/docker/lab.sh"]
CMD ["help"]
