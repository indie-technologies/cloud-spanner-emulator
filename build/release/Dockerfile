FROM ubuntu:24.04@sha256:224a1869083a311ef3f13648a154ba79832fbef6364d31493642ca03082da254
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    libstdc++6 ca-certificates tzdata && rm -rf /var/lib/apt/lists/*
WORKDIR /
COPY emulator_main gateway_main LICENSE licenses.txt.gz build-info.json ./
ARG SOURCE_REVISION
ARG RELEASE_VERSION
LABEL org.opencontainers.image.source="https://github.com/indie-technologies/cloud-spanner-emulator" \
      org.opencontainers.image.revision="$SOURCE_REVISION" \
      org.opencontainers.image.version="$RELEASE_VERSION" \
      org.opencontainers.image.licenses="Apache-2.0"
EXPOSE 9010 9020
CMD ["./gateway_main", "--hostname=0.0.0.0"]
