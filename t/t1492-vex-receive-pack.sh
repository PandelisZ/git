#!/bin/sh

test_description='Vex receive-pack fallback'

. ./test-lib.sh

test_expect_success 'stock Git push uses Vex ODB and leaves no open receiver epoch' '
	test_when_finished "rm -rf client clone remote.git vex-root before after actual err" &&
	GIT_REFERENCE_BACKEND=vex://00000000000000000000000000000011 \
	GIT_VEX_FS_ROOT="$TRASH_DIRECTORY/vex-root" \
	git init --bare --initial-branch=main --ref-format=vex remote.git &&
	git -C remote.git config vex.storeRoot "$TRASH_DIRECTORY/vex-root" &&
	# A value below the three-object test pack would select index-pack without
	# the Vex receive fallback.
	git -C remote.git config receive.unpackLimit 1 &&
	git -C remote.git config receive.denyNonFastForwards true &&

	git init --initial-branch=main client &&
	git -C client config user.email test@example.com &&
	git -C client config user.name Test &&
	git -C client config commit.gpgsign false &&
	printf "one\\n" >client/file &&
	git -C client add file &&
	git -C client commit -m one &&
	git -C client remote add origin "$TRASH_DIRECTORY/remote.git" &&
	git -C client push --receive-pack="git receive-pack" origin main &&

	git -C remote.git rev-parse refs/heads/main >before &&
	git -C remote.git cat-file -t "$(cat before)" >actual &&
	echo commit >expect &&
	test_cmp expect actual &&
	test_dir_is_empty remote.git/objects/pack &&
	git clone --no-local --branch main --upload-pack="git upload-pack" \
		"$TRASH_DIRECTORY/remote.git" clone &&
	git -C clone fsck --full &&

	git -C client checkout --orphan rejected &&
	git -C client rm -rf . &&
	printf "rejected\\n" >client/file &&
	git -C client add file &&
	git -C client commit -m rejected &&
	test_must_fail git -C client push --receive-pack="git receive-pack" \
		origin rejected:refs/heads/main 2>err &&
	git -C remote.git rev-parse refs/heads/main >after &&
	test_cmp before after &&
	! awk -F "	" "\$1 == \"E\" && \$3 == 1 { found = 1 } END { exit !found }" \
		vex-root/meta/00000000000000000000000000000011/state
'

test_done
