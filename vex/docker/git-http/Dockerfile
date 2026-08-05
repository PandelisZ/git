FROM debian:bookworm-slim

RUN apt-get update \
	&& apt-get install -y --no-install-recommends ca-certificates curl git python3 \
	&& rm -rf /var/lib/apt/lists/*

COPY entrypoint.sh /usr/local/bin/vex-git-http-entrypoint
COPY bootstrap_http.py /usr/local/lib/vex-git-http/bootstrap_http.py
COPY vex-init-repo /usr/local/bin/vex-init-repo

RUN chmod 0755 /usr/local/bin/vex-git-http-entrypoint /usr/local/bin/vex-init-repo \
	&& mkdir -p /srv/git /var/git-scratch /usr/local/lib/vex-git-http

EXPOSE 8080 8443

ENTRYPOINT ["/usr/local/bin/vex-git-http-entrypoint"]
