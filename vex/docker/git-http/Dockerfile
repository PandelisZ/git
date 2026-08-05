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
	git git-http-backend vex-sweep-plan

FROM debian:bookworm-slim

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
COPY vex/docker/git-http/entrypoint.sh /usr/local/bin/vex-git-http-entrypoint
COPY vex/docker/git-http/bootstrap_http.py /usr/local/lib/vex-git-http/bootstrap_http.py
COPY vex/docker/git-http/vex_mysql_s3_http.py /usr/local/lib/vex-git-http/vex_mysql_s3_http.py
COPY vex/docker/git-http/vex-init-repo /usr/local/bin/vex-init-repo
COPY vex/docker/git-http/vex-init-mysql-s3-repo /usr/local/bin/vex-init-mysql-s3-repo

RUN chmod 0755 /usr/local/bin/vex-git-http-entrypoint \
		/usr/local/bin/vex-init-repo \
		/usr/local/bin/vex-init-mysql-s3-repo \
		/usr/local/lib/vex-git-http/vex_mysql_s3_http.py \
	&& mkdir -p /srv/git /var/git-scratch /usr/local/lib/vex-git-http/bin \
	&& ln -s /opt/vex-git/git /usr/local/lib/vex-git-http/bin/git \
	&& ln -s /opt/vex-git/git /usr/local/lib/vex-git-http/bin/git-upload-pack \
	&& ln -s /opt/vex-git/git /usr/local/lib/vex-git-http/bin/git-receive-pack

EXPOSE 8080 8443

ENTRYPOINT ["/usr/local/bin/vex-git-http-entrypoint"]
