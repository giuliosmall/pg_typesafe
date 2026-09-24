# PostgreSQL with pg_typesafe preinstalled and enabled.
#
#   docker run -e POSTGRES_PASSWORD=pw -e TYPESAFE_API_KEY=tsk_... \
#     -p 5432:5432 ghcr.io/giuliosmall/pg_typesafe:17
#
# Build another major:  docker build --build-arg PG_MAJOR=16 .
ARG PG_MAJOR=17
ARG BASE_IMAGE=postgres

FROM ${BASE_IMAGE}:${PG_MAJOR} AS build
ARG PG_MAJOR
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      postgresql-server-dev-${PG_MAJOR} libcurl4-openssl-dev pkg-config gcc make \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY Makefile typesafe.c typesafe.control typesafe--*.sql ./
RUN make with_llvm=no \
 && make with_llvm=no DESTDIR=/out install

FROM ${BASE_IMAGE}:${PG_MAJOR}
# libcurl4 was renamed libcurl4t64 in Debian trixie
RUN apt-get update \
 && (apt-get install -y --no-install-recommends libcurl4t64 \
     || apt-get install -y --no-install-recommends libcurl4) \
 && rm -rf /var/lib/apt/lists/*
COPY --from=build /out/ /
COPY docker/initdb-typesafe.sh /docker-entrypoint-initdb.d/10-typesafe.sh
LABEL org.opencontainers.image.source="https://github.com/giuliosmall/pg_typesafe" \
      org.opencontainers.image.description="PostgreSQL with the pg_typesafe extension (TypeSafe AI from SQL)" \
      org.opencontainers.image.licenses="MIT"
