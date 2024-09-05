FROM rockylinux:8

RUN yum -y update && \
  yum -y install \
    gcc-c++ \
    git \
    python3

RUN dnf --enablerepo=powertools install -y libstdc++-static
