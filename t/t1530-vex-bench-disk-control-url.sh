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

test_done
