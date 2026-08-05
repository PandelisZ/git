#!/bin/sh

test_description='Vex fs backend first-write smoke'

. ./test-lib.sh

test_expect_success 'fresh Vex bare repo writes its first blob and tags it' '
	GIT_VEX_FS_ROOT="$TRASH_DIRECTORY/vex-store" &&
	export GIT_VEX_FS_ROOT &&
	git init --bare vex.git &&
	git -C vex.git config core.repositoryformatversion 1 &&
	git -C vex.git config extensions.refstorage \
		vex://55555555555555555555555555555555 &&
	oid=$(printf "fresh Vex first write\\n" |
		git -C vex.git hash-object -w --stdin) &&
	git -C vex.git update-ref refs/tags/first-blob "$oid" &&
	git -C vex.git cat-file -e "$oid" &&
	test "$(git -C vex.git rev-parse refs/tags/first-blob)" = "$oid" &&
	test_path_is_file \
		"$GIT_VEX_FS_ROOT/blobs/git/v1/55555555555555555555555555555555/objects/$oid" &&
	test_path_is_file \
		"$GIT_VEX_FS_ROOT/meta/55555555555555555555555555555555/state"
'

test_done
