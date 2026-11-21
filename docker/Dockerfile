# EasyEFI 构建/打包基座镜像
# 用法（仓库根为上下文，edk2/ 为已校验 SHA 的锁定源码）：
#   docker build -t easyefi-build -f docker/Dockerfile .
# 所有 agent 共享此镜像；worktree 以 -v 挂载到 /work。
# 网络适配（本机实测）：Docker Hub 受阻 → DaoCloud 镜像；apt → 阿里云（arm64=ubuntu-ports）。
FROM docker.m.daocloud.io/library/ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN sed -i 's|http://ports.ubuntu.com/ubuntu-ports|http://mirrors.aliyun.com/ubuntu-ports|g; s|http://archive.ubuntu.com/ubuntu|http://mirrors.aliyun.com/ubuntu|g; s|http://security.ubuntu.com/ubuntu|http://mirrors.aliyun.com/ubuntu|g' /etc/apt/sources.list \
    && apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    gcc-x86-64-linux-gnu binutils-x86-64-linux-gnu \
    gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu \
    nasm iasl acpica-tools \
    uuid-runtime uuid-dev mtools gdisk dosfstools ntfs-3g \
    git python3 python3-distutils curl xz-utils file cpio \
    && rm -rf /var/lib/apt/lists/*
COPY edk2 /edk2
RUN make -C /edk2/BaseTools
ENV GCC5_X64_PREFIX=x86_64-linux-gnu- \
    GCC5_AARCH64_PREFIX=aarch64-linux-gnu- \
    EDK2_PATH=/edk2
WORKDIR /work
