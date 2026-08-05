#!/bin/sh

test_description='Vex receive-pack epoch handoff'

. ./test-lib.sh

vex_repo_id=00000000000000000000000000000013

vex_final_object () {
	echo "$TRASH_DIRECTORY/vex-root/blobs/git/v1/$vex_repo_id/objects/$1"
}

test_expect_success 'stock receive-pack seals accepted objects with refs and compensates rejected objects' '
	test_when_finished "rm -rf client remote.git vex-root before after state pushed rejected err" &&
	GIT_REFERENCE_BACKEND=vex://$vex_repo_id \
	GIT_VEX_FS_ROOT="$TRASH_DIRECTORY/vex-root" \
	git init --bare --initial-branch=main --ref-format=vex remote.git &&
	git -C remote.git config vex.storeRoot "$TRASH_DIRECTORY/vex-root" &&
	git -C remote.git config receive.denyNonFastForwards true &&

	git init --initial-branch=main client &&
	git -C client config user.email test@example.com &&
	git -C client config user.name Test &&
	git -C client config commit.gpgsign false &&
	printf "accepted\\n" >client/file &&
	git -C client add file &&
	git -C client commit -m accepted &&
	git -C client remote add origin "$TRASH_DIRECTORY/remote.git" &&
	git -C client push --receive-pack="git receive-pack" origin main &&

	git -C client rev-parse main >pushed &&
	git -C remote.git rev-parse refs/heads/main >before &&
	test_cmp pushed before &&
	test_path_is_file "$(vex_final_object $(cat pushed))" &&
	cp vex-root/meta/$vex_repo_id/state state &&
	awk -F "	" -v oid="$(cat pushed)" \
		"\$1 == \"E\" { sealed[\$2] = (\$3 == 2) } \
		 \$1 == \"O\" && \$2 == oid && \$6 == 2 { found++; epoch = \$5 } \
		 END { exit !(found == 1 && sealed[epoch]) }" state &&
	awk -F "	" \
		"\$1 == \"E\" && \$3 == 1 { found = 1 } END { exit found }" state &&

	git -C client checkout --orphan rejected &&
	git -C client rm -rf . &&
	printf "rejected\\n" >client/file &&
	git -C client add file &&
	git -C client commit -m rejected &&
	git -C client rev-parse rejected >rejected &&
	test_must_fail git -C client push --receive-pack="git receive-pack" \
		origin rejected:refs/heads/main 2>err &&
	git -C remote.git rev-parse refs/heads/main >after &&
	test_cmp before after &&
	test_path_is_missing "$(vex_final_object $(cat rejected))" &&
	cp vex-root/meta/$vex_repo_id/state state &&
	! awk -F "	" -v oid="$(cat rejected)" \
		"\$1 == \"O\" && \$2 == oid { found = 1 } END { exit !found }" state &&
	! awk -F "	" \
		"\$1 == \"E\" && \$3 == 1 { found = 1 } END { exit !found }" state
'

test_done
