FROM espressif/idf:v6.0.2

# Gitpod 需要 gitpod 用户；espressif/idf 镜像默认用户是 esp
USER root
RUN apt-get update \
    && apt-get install -y --no-install-recommends sudo \
    && useradd -m -s /bin/bash gitpod \
    && echo "gitpod ALL=(ALL) NOPASSWD: ALL" >> /etc/sudoers \
    && rm -rf /var/lib/apt/lists/*

USER gitpod
ENV IDF_PATH=/opt/esp/idf
