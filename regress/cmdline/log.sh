#!/bin/sh
#
# Copyright (c) 2019 Stefan Sperling <stsp@openbsd.org>
#
# Permission to use, copy, modify, and distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
# WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
# MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
# ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
# WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
# ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
# OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

. ./common.sh

function test_log_in_repo {
	local testroot=`test_init log_in_repo`
	local head_rev=`git_show_head $testroot/repo`

	echo "commit $head_rev (master)" > $testroot/stdout.expected

	for p in "" "." alpha epsilon epsilon/zeta; do
		(cd $testroot/repo && got log $p | \
			grep ^commit > $testroot/stdout)
		cmp -s $testroot/stdout.expected $testroot/stdout
		ret="$?"
		if [ "$ret" != "0" ]; then
			diff -u $testroot/stdout.expected $testroot/stdout
			test_done "$testroot" "$ret"
			return 1
		fi
	done

	for p in "" "." zeta; do
		(cd $testroot/repo/epsilon && got log $p | \
			grep ^commit > $testroot/stdout)
		cmp -s $testroot/stdout.expected $testroot/stdout
		ret="$?"
		if [ "$ret" != "0" ]; then
			diff -u $testroot/stdout.expected $testroot/stdout
			test_done "$testroot" "$ret"
			return 1
		fi
	done

	test_done "$testroot" "0"
}

function test_log_in_bare_repo {
	local testroot=`test_init log_in_bare_repo`
	local head_rev=`git_show_head $testroot/repo`

	echo "commit $head_rev (master)" > $testroot/stdout.expected

	for p in "" "." alpha epsilon epsilon/zeta; do
		(cd $testroot/repo/.git && got log $p | \
			grep ^commit > $testroot/stdout)
		cmp -s $testroot/stdout.expected $testroot/stdout
		ret="$?"
		if [ "$ret" != "0" ]; then
			diff -u $testroot/stdout.expected $testroot/stdout
			test_done "$testroot" "$ret"
			return 1
		fi
	done

	test_done "$testroot" "0"
}

function test_log_in_worktree {
	local testroot=`test_init log_in_worktree`
	local head_rev=`git_show_head $testroot/repo`

	got checkout $testroot/repo $testroot/wt > /dev/null
	ret="$?"
	if [ "$ret" != "0" ]; then
		test_done "$testroot" "$ret"
		return 1
	fi

	echo "commit $head_rev (master)" > $testroot/stdout.expected

	for p in "" "." alpha epsilon; do
		(cd $testroot/wt && got log $p | \
			grep ^commit > $testroot/stdout)
		cmp -s $testroot/stdout.expected $testroot/stdout
		ret="$?"
		if [ "$ret" != "0" ]; then
			diff -u $testroot/stdout.expected $testroot/stdout
			test_done "$testroot" "$ret"
			return 1
		fi
	done

	for p in "" "." zeta; do
		(cd $testroot/wt/epsilon && got log $p | \
			grep ^commit > $testroot/stdout)
		cmp -s $testroot/stdout.expected $testroot/stdout
		ret="$?"
		if [ "$ret" != "0" ]; then
			diff -u $testroot/stdout.expected $testroot/stdout
			test_done "$testroot" "$ret"
			return 1
		fi
	done

	test_done "$testroot" "0"
}

function test_log_in_worktree_with_path_prefix {
	local testroot=`test_init log_in_prefixed_worktree`
	local head_rev=`git_show_head $testroot/repo`

	echo "modified zeta" > $testroot/repo/epsilon/zeta
	git_commit $testroot/repo -m "modified zeta"
	local zeta_rev=`git_show_head $testroot/repo`

	echo "modified delta" > $testroot/repo/gamma/delta
	git_commit $testroot/repo -m "modified delta"

	got checkout -p epsilon $testroot/repo $testroot/wt > /dev/null
	ret="$?"
	if [ "$ret" != "0" ]; then
		test_done "$testroot" "$ret"
		return 1
	fi

	echo "commit $zeta_rev" > $testroot/stdout.expected
	echo "commit $head_rev" >> $testroot/stdout.expected

	for p in "" "." zeta; do
		(cd $testroot/wt && got log $p | \
			grep ^commit > $testroot/stdout)
		cmp -s $testroot/stdout.expected $testroot/stdout
		ret="$?"
		if [ "$ret" != "0" ]; then
			diff -u $testroot/stdout.expected $testroot/stdout
			test_done "$testroot" "$ret"
			return 1
		fi
	done

	test_done "$testroot" "0"
}

function test_log_tag {
	local testroot=`test_init log_tag`
	local commit_id=`git_show_head $testroot/repo`
	local tag="1.0.0"
	local tag2="2.0.0"

	got checkout $testroot/repo $testroot/wt > /dev/null
	ret="$?"
	if [ "$ret" != "0" ]; then
		test_done "$testroot" "$ret"
		return 1
	fi

	(cd $testroot/repo && git tag -a -m "test" $tag)

	echo "commit $commit_id (master, tags/$tag)" > $testroot/stdout.expected
	(cd $testroot/wt && got log -l1 -c $tag | grep ^commit \
		> $testroot/stdout)
	cmp -s $testroot/stdout.expected $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		diff -u $testroot/stdout.expected $testroot/stdout
		test_done "$testroot" "$ret"
		return 1
	fi

	# test a "leightweight" tag
	(cd $testroot/repo && git tag $tag2)

	echo "commit $commit_id (master, tags/$tag, tags/$tag2)" \
		> $testroot/stdout.expected
	(cd $testroot/wt && got log -l1 -c $tag2 | grep ^commit \
		> $testroot/stdout)
	cmp -s $testroot/stdout.expected $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		diff -u $testroot/stdout.expected $testroot/stdout
	fi
	test_done "$testroot" "$ret"
}

function test_log_limit {
	local testroot=`test_init log_limit`
	local commit_id0=`git_show_head $testroot/repo`

	got checkout $testroot/repo $testroot/wt > /dev/null
	ret="$?"
	if [ "$ret" != "0" ]; then
		test_done "$testroot" "$ret"
		return 1
	fi

	echo "modified alpha" > $testroot/wt/alpha
	(cd $testroot/wt && got commit -m 'test log_limit' > /dev/null)
	local commit_id1=`git_show_head $testroot/repo`

	(cd $testroot/wt && got rm beta >/dev/null)
	(cd $testroot/wt && got commit -m 'test log_limit' > /dev/null)
	local commit_id2=`git_show_head $testroot/repo`

	echo "new file" > $testroot/wt/new
	(cd $testroot/wt && got add new >/dev/null)
	(cd $testroot/wt && got commit -m 'test log_limit' > /dev/null)
	local commit_id3=`git_show_head $testroot/repo`

	# -l1 should print the first commit only
	echo "commit $commit_id3 (master)" > $testroot/stdout.expected
	(cd $testroot/wt && got log -l1 | grep ^commit > $testroot/stdout)
	cmp -s $testroot/stdout.expected $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		diff -u $testroot/stdout.expected $testroot/stdout
		test_done "$testroot" "$ret"
		return 1
	fi

	# env var can be used to set a log limit without -l option
	echo "commit $commit_id3 (master)" > $testroot/stdout.expected
	echo "commit $commit_id2" >> $testroot/stdout.expected
	(cd $testroot/wt && env GOT_LOG_DEFAULT_LIMIT=2 got log | \
		grep ^commit > $testroot/stdout)
	cmp -s $testroot/stdout.expected $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		diff -u $testroot/stdout.expected $testroot/stdout
		test_done "$testroot" "$ret"
		return 1
	fi

	# non-numeric env var is ignored
	(cd $testroot/wt && env GOT_LOG_DEFAULT_LIMIT=foobar got log | \
		grep ^commit > $testroot/stdout)
	echo "commit $commit_id3 (master)" > $testroot/stdout.expected
	echo "commit $commit_id2" >> $testroot/stdout.expected
	echo "commit $commit_id1" >> $testroot/stdout.expected
	echo "commit $commit_id0" >> $testroot/stdout.expected
	cmp -s $testroot/stdout.expected $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		diff -u $testroot/stdout.expected $testroot/stdout
		test_done "$testroot" "$ret"
		return 1
	fi

	# -l option takes precedence over env var
	echo "commit $commit_id3 (master)" > $testroot/stdout.expected
	echo "commit $commit_id2" >> $testroot/stdout.expected
	echo "commit $commit_id1" >> $testroot/stdout.expected
	echo "commit $commit_id0" >> $testroot/stdout.expected
	(cd $testroot/wt && env GOT_LOG_DEFAULT_LIMIT=1 got log -l0 | \
		grep ^commit > $testroot/stdout)
	cmp -s $testroot/stdout.expected $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		diff -u $testroot/stdout.expected $testroot/stdout
	fi
	test_done "$testroot" "0"
}

run_test test_log_in_repo
run_test test_log_in_bare_repo
run_test test_log_in_worktree
run_test test_log_in_worktree_with_path_prefix
run_test test_log_tag
run_test test_log_limit
