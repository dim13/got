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

function test_checkout_basic {
	local testroot=`test_init checkout_basic`

	echo "A  $testroot/wt/alpha" > $testroot/stdout.expected
	echo "A  $testroot/wt/beta" >> $testroot/stdout.expected
	echo "A  $testroot/wt/epsilon/zeta" >> $testroot/stdout.expected
	echo "A  $testroot/wt/gamma/delta" >> $testroot/stdout.expected
	echo "Now shut up and hack" >> $testroot/stdout.expected

	got checkout $testroot/repo $testroot/wt > $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		test_done "$testroot" "$ret"
		return 1
	fi

	cmp -s $testroot/stdout.expected $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		diff -u $testroot/stdout.expected $testroot/stdout
		test_done "$testroot" "$ret"
		return 1
	fi

	echo "alpha" > $testroot/content.expected
	echo "beta" >> $testroot/content.expected
	echo "zeta" >> $testroot/content.expected
	echo "delta" >> $testroot/content.expected
	cat $testroot/wt/alpha $testroot/wt/beta $testroot/wt/epsilon/zeta \
	    $testroot/wt/gamma/delta > $testroot/content

	cmp -s $testroot/content.expected $testroot/content
	ret="$?"
	if [ "$ret" != "0" ]; then
		diff -u $testroot/content.expected $testroot/content
	fi
	test_done "$testroot" "$ret"
}

function test_checkout_dir_exists {
	local testroot=`test_init checkout_dir_exists`

	echo "A  $testroot/wt/alpha" > $testroot/stdout.expected
	echo "A  $testroot/wt/beta" >> $testroot/stdout.expected
	echo "A  $testroot/wt/epsilon/zeta" >> $testroot/stdout.expected
	echo "A  $testroot/wt/gamma/delta" >> $testroot/stdout.expected
	echo "Now shut up and hack" >> $testroot/stdout.expected

	mkdir $testroot/wt

	got checkout $testroot/repo $testroot/wt > $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		test_done "$testroot" "$ret"
		return 1
	fi

	cmp -s $testroot/stdout.expected $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		diff -u $testroot/stdout.expected $testroot/stdout
		test_done "$testroot" "$ret"
		return 1
	fi

	echo "alpha" > $testroot/content.expected
	echo "beta" >> $testroot/content.expected
	echo "zeta" >> $testroot/content.expected
	echo "delta" >> $testroot/content.expected
	cat $testroot/wt/alpha $testroot/wt/beta $testroot/wt/epsilon/zeta \
	    $testroot/wt/gamma/delta > $testroot/content

	cmp -s $testroot/content.expected $testroot/content
	ret="$?"
	if [ "$ret" != "0" ]; then
		diff -u $testroot/content.expected $testroot/content
	fi
	test_done "$testroot" "$ret"
}

function test_checkout_dir_not_empty {
	local testroot=`test_init checkout_dir_not_empty`

	echo "A  $testroot/wt/alpha" > $testroot/stdout.expected
	echo "A  $testroot/wt/beta" >> $testroot/stdout.expected
	echo "A  $testroot/wt/epsilon/zeta" >> $testroot/stdout.expected
	echo "A  $testroot/wt/gamma/delta" >> $testroot/stdout.expected
	echo "Now shut up and hack" >> $testroot/stdout.expected

	mkdir $testroot/wt
	touch $testroot/wt/foo

	got checkout $testroot/repo $testroot/wt > $testroot/stdout \
		2> $testroot/stderr
	ret="$?"
	if [ "$ret" == "0" ]; then
		echo "checkout succeeded unexpectedly" >&2
		test_done "$testroot" "1"
		return 1
	fi

	echo "got: $testroot/wt: directory exists and is not empty" \
		> $testroot/stderr.expected
	cmp -s $testroot/stderr.expected $testroot/stderr
	ret="$?"
	if [ "$ret" != "0" ]; then
		diff -u $testroot/stderr.expected $testroot/stderr
		test_done "$testroot" "$ret"
		return 1
	fi

	echo -n > $testroot/stdout.expected
	cmp -s $testroot/stdout.expected $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		diff -u $testroot/stdout.expected $testroot/stdout
	fi
	test_done "$testroot" "$ret"

}

function test_checkout_sets_xbit {
	local testroot=`test_init checkout_sets_xbit 1`

	touch $testroot/repo/xfile
	chmod +x $testroot/repo/xfile
	(cd $testroot/repo && git add .)
	git_commit $testroot/repo -m "adding executable file"

	echo "A  $testroot/wt/xfile" > $testroot/stdout.expected
	echo "Now shut up and hack" >> $testroot/stdout.expected

	got checkout $testroot/repo $testroot/wt > $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		test_done "$testroot" "$ret"
		return 1
	fi

	cmp -s $testroot/stdout.expected $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		diff -u $testroot/stdout.expected $testroot/stdout
		test_done "$testroot" "$ret"
		return 1
	fi

	ls -l $testroot/wt/xfile | grep -q '^-rwx'
	ret="$?"
	if [ "$ret" != "0" ]; then
		echo "file is not executable" >&2
		ls -l $testroot/wt/xfile >&2
	fi
	test_done "$testroot" "$ret"
}

function test_checkout_commit_from_wrong_branch {
	local testroot=`test_init checkout_commit_from_wrong_branch`

	(cd $testroot/repo && git checkout -q -b newbranch)
	echo "modified alpha on new branch" > $testroot/repo/alpha
	git_commit $testroot/repo -m "modified alpha on new branch"

	local head_rev=`git_show_head $testroot/repo`
	got checkout -b master -c $head_rev $testroot/repo $testroot/wt \
		> $testroot/stdout 2> $testroot/stderr
	ret="$?"
	if [ "$ret" == "0" ]; then
		test_done "$testroot" "1"
		return 1
	fi

	echo -n "" > $testroot/stdout.expected
	cmp -s $testroot/stdout.expected $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		diff -u $testroot/stdout.expected $testroot/stdout
		test_done "$testroot" "$ret"
		return 1
	fi

	echo  "got: target commit is on a different branch" \
		> $testroot/stderr.expected
	cmp -s $testroot/stderr.expected $testroot/stderr
	ret="$?"
	if [ "$ret" != "0" ]; then
		diff -u $testroot/stderr.expected $testroot/stderr
		test_done "$testroot" "$ret"
		return 1
	fi

	test_done "$testroot" "$ret"
}

function test_checkout_tag {
	local testroot=`test_init checkout_tag`
	local tag="1.0.0"

	(cd $testroot/repo && git tag -a -m "test" $tag)

	echo "A  $testroot/wt/alpha" > $testroot/stdout.expected
	echo "A  $testroot/wt/beta" >> $testroot/stdout.expected
	echo "A  $testroot/wt/epsilon/zeta" >> $testroot/stdout.expected
	echo "A  $testroot/wt/gamma/delta" >> $testroot/stdout.expected
	echo "Now shut up and hack" >> $testroot/stdout.expected

	got checkout -c $tag $testroot/repo $testroot/wt > $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		test_done "$testroot" "$ret"
		return 1
	fi

	cmp -s $testroot/stdout.expected $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		diff -u $testroot/stdout.expected $testroot/stdout
		test_done "$testroot" "$ret"
		return 1
	fi

	echo "alpha" > $testroot/content.expected
	echo "beta" >> $testroot/content.expected
	echo "zeta" >> $testroot/content.expected
	echo "delta" >> $testroot/content.expected
	cat $testroot/wt/alpha $testroot/wt/beta $testroot/wt/epsilon/zeta \
	    $testroot/wt/gamma/delta > $testroot/content

	cmp -s $testroot/content.expected $testroot/content
	ret="$?"
	if [ "$ret" != "0" ]; then
		diff -u $testroot/content.expected $testroot/content
	fi
	test_done "$testroot" "$ret"
}

function test_checkout_ignores_submodules {
	local testroot=`test_init checkout_ignores_submodules`

	(cd $testroot && git clone -q repo repo2 >/dev/null)
	(cd $testroot/repo && git submodule -q add ../repo2)
	(cd $testroot/repo && git commit -q -m 'adding submodule')

	echo "A  $testroot/wt/.gitmodules" > $testroot/stdout.expected
	echo "A  $testroot/wt/alpha" >> $testroot/stdout.expected
	echo "A  $testroot/wt/beta" >> $testroot/stdout.expected
	echo "A  $testroot/wt/epsilon/zeta" >> $testroot/stdout.expected
	echo "A  $testroot/wt/gamma/delta" >> $testroot/stdout.expected
	echo "Now shut up and hack" >> $testroot/stdout.expected

	got checkout $testroot/repo $testroot/wt > $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		test_done "$testroot" "$ret"
		return 1
	fi

	cmp -s $testroot/stdout.expected $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		diff -u $testroot/stdout.expected $testroot/stdout
		test_done "$testroot" "$ret"
		return 1
	fi

	echo "alpha" > $testroot/content.expected
	echo "beta" >> $testroot/content.expected
	echo "zeta" >> $testroot/content.expected
	echo "delta" >> $testroot/content.expected
	cat $testroot/wt/alpha $testroot/wt/beta $testroot/wt/epsilon/zeta \
	    $testroot/wt/gamma/delta > $testroot/content

	cmp -s $testroot/content.expected $testroot/content
	ret="$?"
	if [ "$ret" != "0" ]; then
		diff -u $testroot/content.expected $testroot/content
	fi
	test_done "$testroot" "$ret"
}

run_test test_checkout_basic
run_test test_checkout_dir_exists
run_test test_checkout_dir_not_empty
run_test test_checkout_sets_xbit
run_test test_checkout_commit_from_wrong_branch
run_test test_checkout_tag
run_test test_checkout_ignores_submodules
