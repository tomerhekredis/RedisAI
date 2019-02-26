FROM redis AS builder

ENV DEPS "build-essential git ca-certificates curl unzip wget"

#install latest cmake
ADD https://cmake.org/files/v3.12/cmake-3.12.4-Linux-x86_64.sh /cmake-3.12.4-Linux-x86_64.sh
RUN mkdir /opt/cmake
RUN sh /cmake-3.12.4-Linux-x86_64.sh --prefix=/opt/cmake --skip-license
RUN ln -s /opt/cmake/bin/cmake /usr/local/bin/cmake
RUN cmake --version

# Set up a build environment
RUN set -ex;\
    deps="$DEPS";\
    apt-get update;\
    apt-get install -y --no-install-recommends $deps;

# Get the dependencies
WORKDIR /redisai
ADD ./ /redisai
RUN bash ./get_deps.sh cpu

# Build the source
RUN set -ex;\
    make;

# Package the runner
FROM redis
ENV LD_LIBRARY_PATH /usr/lib/redis/modules

RUN set -ex;\
    mkdir -p "$LD_LIBRARY_PATH";

COPY --from=builder /redisai/src/redisai.so "$LD_LIBRARY_PATH"
COPY --from=builder /redisai/deps/libtensorflow/lib/libtensorflow.so "$LD_LIBRARY_PATH"
COPY --from=builder /redisai/deps/libtensorflow/lib/libtensorflow_framework.so "$LD_LIBRARY_PATH"

WORKDIR /data
EXPOSE 6379
CMD ["--loadmodule", "/usr/lib/redis/modules/redisai.so"]
