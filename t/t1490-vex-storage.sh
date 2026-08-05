#!/bin/sh

test_description='Vex object and reference storage wiring'

. ./test-lib.sh

test_expect_success 'vex URI stores objects and refs outside the gitdir' '
	test_when_finished "rm -rf vex-root vex-repo input oid expect actual err" &&
	GIT_REFERENCE_BACKEND=vex://00000000000000000000000000000001 \
	GIT_VEX_FS_ROOT="$TRASH_DIRECTORY/vex-root" \
	git init --bare --ref-format=vex vex-repo &&

	echo vex://00000000000000000000000000000001 >expect &&
	git -C vex-repo config --get extensions.refstorage >actual &&
	test_cmp expect actual &&
	test_path_is_file vex-repo/HEAD &&
	test_path_is_dir vex-repo/objects &&
	test_path_is_dir vex-repo/refs &&

	printf "vex object\\n" >input &&
	GIT_VEX_FS_ROOT="$TRASH_DIRECTORY/vex-root" \
	git -C vex-repo hash-object -w "$TRASH_DIRECTORY/input" >oid &&
	GIT_VEX_FS_ROOT="$TRASH_DIRECTORY/vex-root" \
	git -C vex-repo update-ref refs/tags/vex-smoke "$(cat oid)" &&
	GIT_VEX_FS_ROOT="$TRASH_DIRECTORY/vex-root" \
	git -C vex-repo cat-file -p "$(cat oid)" >actual &&
	test_cmp input actual &&

	git -C vex-repo config vex.storeRoot "$TRASH_DIRECTORY/vex-root" &&
	git -C vex-repo show-ref --verify refs/tags/vex-smoke >actual &&
	test_grep "$(cat oid)" actual &&
	git -C vex-repo cat-file -t "$(cat oid)" >actual &&
	echo blob >expect &&
	test_cmp expect actual
'

test_expect_success 'Vex maintenance commands fail safely' '
	test_when_finished "rm -rf vex-root vex-repo err" &&
	GIT_REFERENCE_BACKEND=vex://00000000000000000000000000000002 \
	GIT_VEX_FS_ROOT="$TRASH_DIRECTORY/vex-root" \
	git init --bare --ref-format=vex vex-repo &&
	git -C vex-repo config vex.storeRoot "$TRASH_DIRECTORY/vex-root" &&
	test_must_fail git -C vex-repo gc 2>err &&
	test_grep "not supported for Vex storage" err &&
	test_must_fail git -C vex-repo repack 2>err &&
	test_grep "not supported for Vex storage" err &&
	test_must_fail git -C vex-repo multi-pack-index write 2>err &&
	test_grep "not supported for Vex storage" err &&
	test_must_fail git -C vex-repo commit-graph write --reachable 2>err &&
	test_grep "not supported for Vex storage" err
'

test_done
