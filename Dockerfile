# Multi-stage build: Copy Kannel files from kannel image
FROM ssuda/kannel:latest AS kannel-source

# Build stage
FROM ubuntu:24.04 AS builder

ENV TZ=Asia/kolkata
ENV DEBIAN_FRONTEND=noninteractive
RUN ln -snf /usr/share/zoneinfo/$TZ /etc/localtime && echo $TZ > /etc/timezone

# Install build dependencies
RUN apt-get update && \
    apt-get install -y \
    build-essential \
    git \
    curl \
    wget \
    sudo \
    libevent-dev \
    libtool \
    autogen \
    autoconf \
    automake1.11 \
    pkg-config \
    m4 \
    gettext \
    bison \
    byacc \
    libxml2-dev \
    default-libmysqlclient-dev \
    libssl-dev \
    libhiredis-dev \
    gnulib \
    autopoint \
    texinfo \
    shtool \
    && rm -rf /var/lib/apt/lists/*

# Copy Kannel libraries, headers, and gw-config from kannel image
COPY --from=kannel-source /usr/local/kannel/lib /usr/local/kannel/lib
COPY --from=kannel-source /usr/local/kannel/include /usr/local/kannel/include
COPY --from=kannel-source /usr/local/kannel/gw-config /usr/local/kannel/gw-config

# Build ksmppd - Cache bust for fresh git clone
ARG CACHE_BUST=1
RUN echo "Cache bust: $CACHE_BUST"

COPY ./ /root/softwares/ksmppd

# RUN --mount=type=ssh --mount=type=secret,id=ssh_config,target=/root/.ssh/config mkdir -p -m 0600 ~/.ssh && \
#     ssh-keyscan github.com >> ~/.ssh/known_hosts && \
#     mkdir -p /root/softwares && \
#     cd /root/softwares && \
#     #git clone https://github.com/kneodev/ksmppd.git \
#     git clone https://github.com/diviky/ksmppd.git \
#     && cd ksmppd

RUN cd /root/softwares/ksmppd && \
    libtoolize --force --copy && \
    aclocal && \
    autoheader && \
    automake --force-missing --add-missing --copy && \
    autoconf && \
    export PATH=/usr/local/kannel:$PATH && \
    export LD_LIBRARY_PATH=/usr/local/kannel/lib:$LD_LIBRARY_PATH && \
    export LDFLAGS="-L/usr/local/kannel/lib" && \
    export CPPFLAGS="-I/usr/local/kannel/include" && \
    ./configure --prefix=/usr/local && \
    make -j$(nproc) && \
    make install && \
    cd / && \
    rm -rf /root/softwares

# Runtime stage - minimal image using Ubuntu (for library compatibility)
FROM ubuntu:24.04

ENV TZ=Asia/kolkata
ENV DEBIAN_FRONTEND=noninteractive
RUN ln -snf /usr/share/zoneinfo/$TZ /etc/localtime && echo $TZ > /etc/timezone

# Install only runtime dependencies (minimal set)
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
    libevent-2.1-7t64 \
    libxml2 \
    libmysqlclient21 \
    libssl3t64 \
    libhiredis1.1.0 \
    ca-certificates \
    jq \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/* /tmp/* /var/tmp/* /var/cache/apt/archives/*

# Copy Kannel libraries and gw-config from kannel image
COPY --from=kannel-source /usr/local/kannel/lib /usr/local/kannel/lib
COPY --from=kannel-source /usr/local/kannel/gw-config /usr/local/kannel/gw-config

# Copy ksmppd binary from builder stage
COPY --from=builder /usr/local/bin/ksmppd /usr/local/bin/ksmppd

# Create necessary directories
RUN mkdir -p /etc/ksmppd && \
    mkdir -p /var/log/ksmppd && \
    chmod -R 755 /var/log/ksmppd

# Set runtime library path for Kannel
ENV LD_LIBRARY_PATH=/usr/local/kannel/lib:/usr/local/lib
ENV PATH=/usr/local/kannel:/usr/local/bin:$PATH

COPY kannel/entrypoint.sh /usr/bin/entrypoint.sh
RUN chmod +x /usr/bin/entrypoint.sh

EXPOSE 14000 2345 14010

VOLUME ["/etc/ksmppd", "/var/log/ksmppd"]

ENTRYPOINT ["/usr/bin/entrypoint.sh"]
CMD ["/usr/local/bin/ksmppd", "/etc/ksmppd/ksmppd.conf"]
