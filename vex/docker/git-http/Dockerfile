FROM debian:bookworm AS vex-git-build

# The MySQL client is an explicit build feature: this image never delegates
# server-side protocol work to the distro Git binary.  Keep the source build
# in a separate stage so the runtime contains only the custom Git artifacts
# and their shared-library dependencies.
RUN apt-get update \
	&& apt-get install -y --no-install-recommends \
		build-essential \
		ca-certificates \
		gettext \
		libcurl4-openssl-dev \
		libexpat1-dev \
		libssl-dev \
		default-libmysqlclient-dev \
		pkg-config \
		zlib1g-dev \
	&& rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN make -j"$(nproc)" VEX_MYSQL=YesPlease NO_RUST=YesPlease NO_TCLTK=YesPlease NO_PERL=YesPlease \
	git git-http-backend vex-sweep-plan vex-epoch-lease

# Keep the source-built Vex Git runtime reusable by non-HTTP services.  The
# pack worker deliberately inherits this stage so it executes the same Git
# binary and shared-library set as receive-pack and upload-pack.
FROM debian:bookworm-slim AS vex-git-runtime

RUN apt-get update \
	&& apt-get install -y --no-install-recommends \
		ca-certificates \
		curl \
		libcurl4 \
		libexpat1 \
		libmariadb3 \
		libssl3 \
		python3 \
		zlib1g \
	&& rm -rf /var/lib/apt/lists/*

COPY --from=vex-git-build /src/git /opt/vex-git/git
COPY --from=vex-git-build /src/git-http-backend /opt/vex-git/git-http-backend
COPY --from=vex-git-build /src/t/vex/bin/vex-sweep-plan /opt/vex-git/vex-sweep-plan
COPY --from=vex-git-build /src/t/vex/bin/vex-epoch-lease /opt/vex-git/vex-epoch-lease
COPY vex/docker/git-http/entrypoint.sh /usr/local/bin/vex-git-http-entrypoint
COPY vex/docker/git-http/bootstrap_http.py /usr/local/lib/vex-git-http/bootstrap_http.py
COPY vex/docker/git-http/vex_mysql_s3_http.py /usr/local/lib/vex-git-http/vex_mysql_s3_http.py
COPY vex/docker/git-http/vex-init-repo /usr/local/bin/vex-init-repo
COPY vex/docker/git-http/vex-init-mysql-s3-repo /usr/local/bin/vex-init-mysql-s3-repo

ENV GIT_EXEC_PATH=/opt/vex-git

RUN chmod 0755 /usr/local/bin/vex-git-http-entrypoint \
		/usr/local/bin/vex-init-repo \
		/usr/local/bin/vex-init-mysql-s3-repo \
		/usr/local/lib/vex-git-http/vex_mysql_s3_http.py \
	&& mkdir -p /srv/git /var/git-scratch /usr/local/lib/vex-git-http/bin \
	&& ln -s git /opt/vex-git/git-upload-pack \
	&& ln -s git /opt/vex-git/git-receive-pack \
	&& ln -s git /opt/vex-git/git-refs \
	&& ln -s git /opt/vex-git/git-commit-graph \
	&& ln -s git /opt/vex-git/git-multi-pack-index \
	&& ln -s /opt/vex-git/git /usr/local/lib/vex-git-http/bin/git \
	&& ln -s /opt/vex-git/git /usr/local/lib/vex-git-http/bin/git-upload-pack \
	&& ln -s /opt/vex-git/git /usr/local/lib/vex-git-http/bin/git-receive-pack

EXPOSE 8080 8443

ENTRYPOINT ["/usr/local/bin/vex-git-http-entrypoint"]

# The pack worker is a separate Rust program, not part of Git's optional Rust
# library.  Building it in its own stage keeps the Git build above explicitly
# NO_RUST while still producing one image with the Vex Git runtime available.
# This target becomes buildable as soon as vex/pack-worker/Cargo.toml exists;
# the default `vex-git-http` target intentionally does not depend on it.
FROM rust:1.96-bookworm AS vex-pack-worker-build

WORKDIR /src
COPY vex/pack-worker/ /src/vex/pack-worker/
RUN cargo build --locked --manifest-path /src/vex/pack-worker/Cargo.toml --release

FROM vex-git-runtime AS vex-pack-worker

COPY --from=vex-pack-worker-build /src/vex/pack-worker/target/release/vex-pack-worker /usr/local/bin/vex-pack-worker

# Let the worker invoke the source-built Vex Git commands by their normal
# names, while retaining GIT_EXEC_PATH from the shared runtime stage.
ENV PATH="/opt/vex-git:${PATH}"

ENTRYPOINT ["/usr/local/bin/vex-pack-worker"]
CMD ["--once"]

# Preserve git-http as the Dockerfile's default target so existing `docker
# build` and Compose workflows do not require the Rust worker crate yet.
FROM vex-git-runtime AS vex-git-http
