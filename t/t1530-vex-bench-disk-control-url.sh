#!/bin/sh

test_description='Vex L4 benchmark disk-control URL contract'

TEST_NO_CREATE_REPO=1

. ./test-lib.sh

benchmark="$GIT_BUILD_DIR/scripts/vex/bench-clone.sh"
origin='https://bench.example:8443/git/git.git'
disk_control='https://bench.example:8443/__vex_bench_disk__/git-git-disk.git'

reject_url() {
	label=$1
	expected=$2
	test_origin=$3
	test_disk=$4

	test_expect_code 2 env VEX_STOCK_GIT="$GIT_BUILD_DIR/git" \
		"$benchmark" \
		--origin "$test_origin" \
		--disk-origin "$test_disk" \
		--branch main \
		--runs 1 \
		--warmups 0 \
		--output "$TRASH_DIRECTORY/$label.json" \
		>actual 2>&1 &&
	grep -F -- "$expected" actual
}

test_expect_success 'L4 disk control is a fixed same-frontend HTTPS route' '
	reject_url wrong-host \
		"--disk-origin must use the same HTTPS frontend as --origin" \
		"$origin" \
		"https://other.example:8443/__vex_bench_disk__/git-git-disk.git" &&
	reject_url wrong-port \
		"--disk-origin must use the same HTTPS frontend as --origin" \
		"$origin" \
		"https://bench.example:9443/__vex_bench_disk__/git-git-disk.git" &&
	reject_url wrong-path \
		"--disk-origin must be exactly https://<same-host-and-port>/__vex_bench_disk__/git-git-disk.git" \
		"$origin" \
		"https://bench.example:8443/__vex_bench_disk__/other.git" &&
	reject_url trailing-slash \
		"--disk-origin must be exactly https://<same-host-and-port>/__vex_bench_disk__/git-git-disk.git" \
		"$origin" \
		"$disk_control/" &&
	reject_url disk-credentials \
		"--disk-origin must not include credentials" \
		"$origin" \
		"https://bench:secret@bench.example:8443/__vex_bench_disk__/git-git-disk.git" &&
	reject_url disk-query \
		"--disk-origin must not include a query string" \
		"$origin" \
		"$disk_control?token=secret" &&
	reject_url disk-fragment \
		"--disk-origin must not include a fragment" \
		"$origin" \
		"$disk_control#fragment" &&
	reject_url origin-credentials \
		"--origin must not include credentials" \
		"https://bench:secret@bench.example:8443/git/git.git" \
		"$disk_control" &&
	reject_url origin-query \
		"--origin must not include a query string" \
		"$origin?token=secret" \
		"$disk_control" &&
	reject_url origin-fragment \
		"--origin must not include a fragment" \
		"$origin#fragment" \
		"$disk_control"
'

test_expect_success '--git-config stays on Vex/disk controls and is isolated from GitHub' '
	mkdir baseline &&
	test_write_lines \
		"[http]" \
		"extraHeader = X-Vex-Benchmark-Secret: must-not-reach-github" >private.gitconfig &&
	write_script fake-stock-git <<-\EOF &&
	oid=1111111111111111111111111111111111111111

	fail () {
		echo "fake git: $*" >&2
		exit 1
	}

	require_private_config () {
		test "${GIT_CONFIG_GLOBAL-}" = "$PRIVATE_CONFIG" || fail "private global config missing"
		test "${GIT_CONFIG_NOSYSTEM-}" = 1 || fail "private no-system config missing"
		test "${GIT_CONFIG_COUNT-}" = 1 || fail "private command config missing"
		test "${GIT_CONFIG_KEY_0-}" = http.extraHeader || fail "private command config key missing"
		test "${GIT_CONFIG_VALUE_0-}" = "X-Vex-Test: header" || fail "private command config value missing"
		test "${GIT_ASKPASS-}" = "$PRIVATE_ASKPASS" || fail "private askpass missing"
	}

	require_github_config () {
		test -n "${GIT_CONFIG_GLOBAL-}" || fail "GitHub global config missing"
		test "$GIT_CONFIG_GLOBAL" != "$PRIVATE_CONFIG" || fail "GitHub inherited private global config"
		test -f "$GIT_CONFIG_GLOBAL" || fail "GitHub global config does not exist"
		test ! -s "$GIT_CONFIG_GLOBAL" || fail "GitHub global config is not empty"
		test "${GIT_CONFIG_NOSYSTEM-}" = 1 || fail "GitHub no-system config missing"
		test -z "${GIT_CONFIG_COUNT-}" || fail "GitHub inherited command config"
		test -z "${GIT_CONFIG_KEY_0-}" || fail "GitHub inherited command config key"
		test -z "${GIT_CONFIG_VALUE_0-}" || fail "GitHub inherited command config value"
		test -z "${GIT_ASKPASS-}" || fail "GitHub inherited askpass"
		test -z "${SSH_ASKPASS-}" || fail "GitHub inherited SSH askpass"
		test "${GIT_TERMINAL_PROMPT-}" = 0 || fail "GitHub terminal prompt is not disabled"
	}

	previous= && last=
	for arg
	do
		previous=$last
		last=$arg
	done

	case "$1" in
	--version)
		echo "git version fake"
		;;
	check-ref-format)
		;;
	ls-remote)
		if test "$2" = "$GITHUB_ORIGIN"
		then
			require_github_config && echo github-isolated >>"$FAKE_GIT_LOG"
		else
			require_private_config && echo private-control >>"$FAKE_GIT_LOG"
		fi &&
		printf "%s\\trefs/heads/main\\n" "$oid"
		;;
	clone)
		if test "$previous" = "$GITHUB_ORIGIN"
		then
			require_github_config && echo github-isolated >>"$FAKE_GIT_LOG"
		else
			require_private_config && echo private-control >>"$FAKE_GIT_LOG"
		fi &&
		mkdir -p "$last/.git/objects"
		;;
	-C)
		case "$2" in
		*github*) require_github_config && echo github-isolated >>"$FAKE_GIT_LOG" ;;
		*) require_private_config && echo private-control >>"$FAKE_GIT_LOG" ;;
		esac &&
		case "$3" in
		rev-parse) echo "$oid" ;;
		status|fsck) ;;
		*) fail "unsupported -C command: $3" ;;
		esac
		;;
	*)
		fail "unsupported command: $*"
		;;
	esac
	EOF
	PRIVATE_CONFIG="$PWD/private.gitconfig" \
	PRIVATE_ASKPASS="$PWD/private-askpass" \
	GITHUB_ORIGIN="https://github.example/git/git" \
	FAKE_GIT_LOG="$PWD/fake-git.log" \
	GIT_ASKPASS="$PWD/private-askpass" \
	SSH_ASKPASS="$PWD/private-askpass" \
	GIT_CONFIG_COUNT=1 \
	GIT_CONFIG_KEY_0=http.extraHeader \
	GIT_CONFIG_VALUE_0="X-Vex-Test: header" \
	VEX_STOCK_GIT="$PWD/fake-stock-git" \
	"$benchmark" \
		--origin https://vex.example/git/repo.git \
		--baseline "$PWD/baseline" \
		--branch main \
		--runs 1 \
		--warmups 0 \
		--include-github \
		--github-origin https://github.example/git/git \
		--git-config "$PWD/private.gitconfig" \
		--output "$PWD/result.json" &&
	grep -q '^github-isolated$' fake-git.log &&
	grep -q '^private-control$' fake-git.log &&
	! grep -F "must-not-reach-github" result.json
'

test_done
