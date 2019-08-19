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

function blame_cmp {
	local testroot="$1"
	local file="$2"

	(cd $testroot/wt && got blame "$file" | cut -d ' ' -f 2 \
		> $testroot/${file}.blame.got)
	(cd $testroot/repo && git reset --hard master > /dev/null)
	(cd $testroot/repo && git blame "$file" | cut -d ' ' -f 1 \
		> $testroot/${file}.blame.git)

	cmp -s $testroot/${file}.blame.git $testroot/${file}.blame.got
	ret="$?"
	if [ "$ret" != "0" ]; then
		diff -u $testroot/${file}.blame.git $testroot/${file}.blame.got
		return 1
	fi
	return 0
}

function test_blame_basic {
	local testroot=`test_init blame_basic`

	got checkout $testroot/repo $testroot/wt > /dev/null
	ret="$?"
	if [ "$ret" != "0" ]; then
		test_done "$testroot" "$ret"
		return 1
	fi

	echo 1 > $testroot/wt/alpha
	(cd $testroot/wt && got commit -m "change 1" > /dev/null)
	local commit1=`git_show_head $testroot/repo`

	echo 2 >> $testroot/wt/alpha
	(cd $testroot/wt && got commit -m "change 2" > /dev/null)
	local commit2=`git_show_head $testroot/repo`

	echo 3 >> $testroot/wt/alpha
	(cd $testroot/wt && got commit -m "change 3" > /dev/null)
	local commit3=`git_show_head $testroot/repo`
	local author_time=`git_show_author_time $testroot/repo`

	(cd $testroot/wt && got blame alpha > $testroot/stdout)

	local short_commit1=`trim_obj_id 32 $commit1`
	local short_commit2=`trim_obj_id 32 $commit2`
	local short_commit3=`trim_obj_id 32 $commit3`

	d=`date -r $author_time +"%g/%m/%d"`
	echo "1) $short_commit1 $d $GOT_AUTHOR_8 1" > $testroot/stdout.expected
	echo "2) $short_commit2 $d $GOT_AUTHOR_8 2" >> $testroot/stdout.expected
	echo "3) $short_commit3 $d $GOT_AUTHOR_8 3" >> $testroot/stdout.expected

	cmp -s $testroot/stdout.expected $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		diff -u $testroot/stdout.expected $testroot/stdout
		test_done "$testroot" "$ret"
		return 1
	fi

	blame_cmp "$testroot" "alpha"
	ret="$?"
	test_done "$testroot" "$ret"
}

function test_blame_tag {
	local testroot=`test_init blame_tag`
	local tag=1.0.0

	got checkout $testroot/repo $testroot/wt > /dev/null
	ret="$?"
	if [ "$ret" != "0" ]; then
		test_done "$testroot" "$ret"
		return 1
	fi
	echo 1 > $testroot/wt/alpha
	(cd $testroot/wt && got commit -m "change 1" > /dev/null)
	local commit1=`git_show_head $testroot/repo`

	echo 2 >> $testroot/wt/alpha
	(cd $testroot/wt && got commit -m "change 2" > /dev/null)
	local commit2=`git_show_head $testroot/repo`

	(cd $testroot/repo && git tag -a -m "test" $tag)

	echo 3 >> $testroot/wt/alpha
	(cd $testroot/wt && got commit -m "change 3" > /dev/null)
	local commit3=`git_show_head $testroot/repo`
	local author_time=`git_show_author_time $testroot/repo`

	(cd $testroot/wt && got blame -c $tag alpha > $testroot/stdout)

	local short_commit1=`trim_obj_id 32 $commit1`
	local short_commit2=`trim_obj_id 32 $commit2`

	d=`date -r $author_time +"%g/%m/%d"`
	echo "1) $short_commit1 $d $GOT_AUTHOR_8 1" > $testroot/stdout.expected
	echo "2) $short_commit2 $d $GOT_AUTHOR_8 2" >> $testroot/stdout.expected

	cmp -s $testroot/stdout.expected $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		diff -u $testroot/stdout.expected $testroot/stdout
		test_done "$testroot" "$ret"
		return 1
	fi

	blame_cmp "$testroot" "alpha"
	ret="$?"
	test_done "$testroot" "$ret"
}

function test_blame_file_single_line {
	local testroot=`test_init blame_file_single_line`

	got checkout $testroot/repo $testroot/wt > /dev/null
	ret="$?"
	if [ "$ret" != "0" ]; then
		test_done "$testroot" "$ret"
		return 1
	fi

	echo 1 > $testroot/wt/alpha
	(cd $testroot/wt && got commit -m "change 1" > /dev/null)
	local commit1=`git_show_head $testroot/repo`
	local author_time=`git_show_author_time $testroot/repo`

	(cd $testroot/wt && got blame alpha > $testroot/stdout)

	local short_commit1=`trim_obj_id 32 $commit1`

	d=`date -r $author_time +"%g/%m/%d"`
	echo "1) $short_commit1 $d $GOT_AUTHOR_8 1" > $testroot/stdout.expected

	cmp -s $testroot/stdout.expected $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		diff -u $testroot/stdout.expected $testroot/stdout
		test_done "$testroot" "$ret"
		return 1
	fi

	blame_cmp "$testroot" "alpha"
	ret="$?"
	test_done "$testroot" "$ret"
}

function test_blame_file_single_line_no_newline {
	local testroot=`test_init blame_file_single_line_no_newline`

	got checkout $testroot/repo $testroot/wt > /dev/null
	ret="$?"
	if [ "$ret" != "0" ]; then
		test_done "$testroot" "$ret"
		return 1
	fi

	echo -n 1 > $testroot/wt/alpha
	(cd $testroot/wt && got commit -m "change 1" > /dev/null)
	local commit1=`git_show_head $testroot/repo`
	local author_time=`git_show_author_time $testroot/repo`

	(cd $testroot/wt && got blame alpha > $testroot/stdout)

	local short_commit1=`trim_obj_id 32 $commit1`

	d=`date -r $author_time +"%g/%m/%d"`
	echo "1) $short_commit1 $d $GOT_AUTHOR_8 1" > $testroot/stdout.expected

	cmp -s $testroot/stdout.expected $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		diff -u $testroot/stdout.expected $testroot/stdout
	fi
	test_done "$testroot" "$ret"
}

function test_blame_all_lines_replaced {
	local testroot=`test_init blame_all_lines_replaced`

	got checkout $testroot/repo $testroot/wt > /dev/null
	ret="$?"
	if [ "$ret" != "0" ]; then
		test_done "$testroot" "$ret"
		return 1
	fi

	jot 8 > $testroot/wt/alpha
	(cd $testroot/wt && got commit -m "change 1" > /dev/null)
	local commit1=`git_show_head $testroot/repo`
	local short_commit1=`trim_obj_id 32 $commit1`
	local author_time=`git_show_author_time $testroot/repo`

	(cd $testroot/wt && got blame alpha > $testroot/stdout)

	d=`date -r $author_time +"%g/%m/%d"`
	echo "1) $short_commit1 $d $GOT_AUTHOR_8 1" > $testroot/stdout.expected
	echo "2) $short_commit1 $d $GOT_AUTHOR_8 2" >> $testroot/stdout.expected
	echo "3) $short_commit1 $d $GOT_AUTHOR_8 3" >> $testroot/stdout.expected
	echo "4) $short_commit1 $d $GOT_AUTHOR_8 4" >> $testroot/stdout.expected
	echo "5) $short_commit1 $d $GOT_AUTHOR_8 5" >> $testroot/stdout.expected
	echo "6) $short_commit1 $d $GOT_AUTHOR_8 6" >> $testroot/stdout.expected
	echo "7) $short_commit1 $d $GOT_AUTHOR_8 7" >> $testroot/stdout.expected
	echo "8) $short_commit1 $d $GOT_AUTHOR_8 8" >> $testroot/stdout.expected

	cmp -s $testroot/stdout.expected $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		diff -u $testroot/stdout.expected $testroot/stdout
	fi
	test_done "$testroot" "$ret"

}

function test_blame_lines_shifted_up {
	local testroot=`test_init blame_lines_shifted_up`

	got checkout $testroot/repo $testroot/wt > /dev/null
	ret="$?"
	if [ "$ret" != "0" ]; then
		test_done "$testroot" "$ret"
		return 1
	fi

	jot 8 > $testroot/wt/alpha
	(cd $testroot/wt && got commit -m "change 1" > /dev/null)
	local commit1=`git_show_head $testroot/repo`
	local short_commit1=`trim_obj_id 32 $commit1`
	local author_time=`git_show_author_time $testroot/repo`

	sed -i -e '/^[345]$/d' $testroot/wt/alpha
	(cd $testroot/wt && got commit -m "change 2" > /dev/null)
	local commit2=`git_show_head $testroot/repo`
	local short_commit2=`trim_obj_id 32 $commit2`

	jot 2 > $testroot/wt/alpha
	echo foo >> $testroot/wt/alpha
	echo bar >> $testroot/wt/alpha
	echo baz >> $testroot/wt/alpha
	jot 8 6 8 1 >> $testroot/wt/alpha
	(cd $testroot/wt && got commit -m "change 3" > /dev/null)
	local commit3=`git_show_head $testroot/repo`
	local short_commit3=`trim_obj_id 32 $commit3`
	local author_time=`git_show_author_time $testroot/repo`

	(cd $testroot/wt && got blame alpha > $testroot/stdout)

	d=`date -r $author_time +"%g/%m/%d"`
	echo "1) $short_commit1 $d $GOT_AUTHOR_8 1" > $testroot/stdout.expected
	echo "2) $short_commit1 $d $GOT_AUTHOR_8 2" >> $testroot/stdout.expected
	echo "3) $short_commit3 $d $GOT_AUTHOR_8 foo" >> $testroot/stdout.expected
	echo "4) $short_commit3 $d $GOT_AUTHOR_8 bar" >> $testroot/stdout.expected
	echo "5) $short_commit3 $d $GOT_AUTHOR_8 baz" >> $testroot/stdout.expected
	echo "6) $short_commit1 $d $GOT_AUTHOR_8 6" >> $testroot/stdout.expected
	echo "7) $short_commit1 $d $GOT_AUTHOR_8 7" >> $testroot/stdout.expected
	echo "8) $short_commit1 $d $GOT_AUTHOR_8 8" >> $testroot/stdout.expected

	cmp -s $testroot/stdout.expected $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		diff -u $testroot/stdout.expected $testroot/stdout
		test_done "$testroot" "$ret"
		return 1
	fi

	blame_cmp "$testroot" "alpha"
	ret="$?"
	test_done "$testroot" "$ret"
}

function test_blame_lines_shifted_down {
	local testroot=`test_init blame_lines_shifted_down`

	got checkout $testroot/repo $testroot/wt > /dev/null
	ret="$?"
	if [ "$ret" != "0" ]; then
		test_done "$testroot" "$ret"
		return 1
	fi

	jot 8 > $testroot/wt/alpha
	(cd $testroot/wt && got commit -m "change 1" > /dev/null)
	local commit1=`git_show_head $testroot/repo`
	local short_commit1=`trim_obj_id 32 $commit1`
	local author_time=`git_show_author_time $testroot/repo`

	sed -i -e '/^8$/d' $testroot/wt/alpha
	(cd $testroot/wt && got commit -m "change 2" > /dev/null)
	local commit2=`git_show_head $testroot/repo`
	local short_commit2=`trim_obj_id 32 $commit2`

	jot 2 > $testroot/wt/alpha
	echo foo >> $testroot/wt/alpha
	echo bar >> $testroot/wt/alpha
	echo baz >> $testroot/wt/alpha
	jot 8 3 8 1 >> $testroot/wt/alpha
	(cd $testroot/wt && got commit -m "change 3" > /dev/null)
	local commit3=`git_show_head $testroot/repo`
	local short_commit3=`trim_obj_id 32 $commit3`
	local author_time=`git_show_author_time $testroot/repo`

	(cd $testroot/wt && got blame alpha > $testroot/stdout)

	d=`date -r $author_time +"%g/%m/%d"`
	echo "01) $short_commit1 $d $GOT_AUTHOR_8 1" \
		> $testroot/stdout.expected
	echo "02) $short_commit1 $d $GOT_AUTHOR_8 2" \
		>> $testroot/stdout.expected
	echo "03) $short_commit3 $d $GOT_AUTHOR_8 foo" \
		>> $testroot/stdout.expected
	echo "04) $short_commit3 $d $GOT_AUTHOR_8 bar" \
		>> $testroot/stdout.expected
	echo "05) $short_commit3 $d $GOT_AUTHOR_8 baz" \
		>> $testroot/stdout.expected
	echo "06) $short_commit1 $d $GOT_AUTHOR_8 3" \
		>> $testroot/stdout.expected
	echo "07) $short_commit1 $d $GOT_AUTHOR_8 4" \
		>> $testroot/stdout.expected
	echo "08) $short_commit1 $d $GOT_AUTHOR_8 5" \
		>> $testroot/stdout.expected
	echo "09) $short_commit1 $d $GOT_AUTHOR_8 6" \
		>> $testroot/stdout.expected
	echo "10) $short_commit1 $d $GOT_AUTHOR_8 7" \
		>> $testroot/stdout.expected
	echo "11) $short_commit3 $d $GOT_AUTHOR_8 8" \
		>> $testroot/stdout.expected

	cmp -s $testroot/stdout.expected $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		diff -u $testroot/stdout.expected $testroot/stdout
		test_done "$testroot" "$ret"
		return 1
	fi

	blame_cmp "$testroot" "alpha"
	ret="$?"
	test_done "$testroot" "$ret"
}

function test_blame_commit_subsumed {
	local testroot=`test_init blame_commit_subsumed`

	got checkout $testroot/repo $testroot/wt > /dev/null
	ret="$?"
	if [ "$ret" != "0" ]; then
		test_done "$testroot" "$ret"
		return 1
	fi

	cat > $testroot/wt/alpha <<EOF
SUBDIRS = ext modules codedocs docs

if WITH_PDNS_SERVER
  SUBDIRS += pdns
endif

EXTRA_DIST =
	INSTALL
	NOTICE
	README
	.version
	build-aux/gen-version
	codedocs/doxygen.conf
	contrib/powerdns.solaris.init.d
	pdns/named.conf.parsertest
	regression-tests/zones/unit.test

ACLOCAL_AMFLAGS = -I m4

dvi: # do nothing to build dvi
EOF
	(cd $testroot/wt && got commit -m "change 1" > /dev/null)
	local commit1=`git_show_head $testroot/repo`
	local short_commit1=`trim_obj_id 32 $commit1`
	local author_time1=`git_show_author_time $testroot/repo`
	local d1=`date -r $author_time1 +"%g/%m/%d"`

	cat > $testroot/wt/alpha <<EOF
SUBDIRS = ext modules codedocs docs

SUBDIRS += pdns

EXTRA_DIST =
	INSTALL
	NOTICE
	README
	.version
	build-aux/gen-version
	codedocs/doxygen.conf
	contrib/powerdns.solaris.init.d
	pdns/named.conf.parsertest
	regression-tests/zones/unit.test

ACLOCAL_AMFLAGS = -I m4

dvi: # do nothing to build dvi
EOF
	# all changes in this commit will be subsumed by later commits
	(cd $testroot/wt && got commit -m "change 2" > /dev/null)
	local commit2=`git_show_head $testroot/repo`
	local short_commit2=`trim_obj_id 32 $commit2`
	local author_time2=`git_show_author_time $testroot/repo`
	local d2=`date -r $author_time2 +"%g/%m/%d"`

	cat > $testroot/wt/alpha <<EOF
SUBDIRS = ext modules pdns codedocs docs

EXTRA_DIST =
	INSTALL
	NOTICE
	README
	.version
	build-aux/gen-version
	codedocs/doxygen.conf
	contrib/powerdns.solaris.init.d
	pdns/named.conf.parsertest
	regression-tests/zones/unit.test

ACLOCAL_AMFLAGS = -I m4

dvi: # do nothing to build dvi
EOF
	(cd $testroot/wt && got commit -m "change 3" > /dev/null)
	local commit3=`git_show_head $testroot/repo`
	local short_commit3=`trim_obj_id 32 $commit3`
	local author_time3=`git_show_author_time $testroot/repo`
	local d3=`date -r $author_time3 +"%g/%m/%d"`

	cat > $testroot/wt/alpha <<EOF
SUBDIRS = ext modules pdns codedocs docs

EXTRA_DIST =
	INSTALL
	NOTICE
	README
	COPYING
	codedocs/doxygen.conf
	contrib/powerdns.solaris.init.d
	pdns/named.conf.parsertest
	regression-tests/zones/unit.test
	builder-support/gen-version

ACLOCAL_AMFLAGS = -I m4

dvi: # do nothing to build dvi
EOF
	(cd $testroot/wt && got commit -m "change 4" > /dev/null)
	local commit4=`git_show_head $testroot/repo`
	local short_commit4=`trim_obj_id 32 $commit4`
	local author_time4=`git_show_author_time $testroot/repo`
	local d4=`date -r $author_time4 +"%g/%m/%d"`

	(cd $testroot/wt && got blame alpha > $testroot/stdout)

	echo -n "01) $short_commit3 $d3 $GOT_AUTHOR_8 " \
		> $testroot/stdout.expected
	echo "SUBDIRS = ext modules pdns codedocs docs" \
		>> $testroot/stdout.expected
	echo "02) $short_commit1 $d1 $GOT_AUTHOR_8 " \
		>> $testroot/stdout.expected
	echo -n "03) $short_commit1 $d1 $GOT_AUTHOR_8 " \
		>> $testroot/stdout.expected
	echo 'EXTRA_DIST =' >> $testroot/stdout.expected
	echo -n "04) $short_commit1 $d1 $GOT_AUTHOR_8 " \
		>> $testroot/stdout.expected
	printf "\tINSTALL\n" >> $testroot/stdout.expected
	echo -n "05) $short_commit1 $d1 $GOT_AUTHOR_8 " \
		>> $testroot/stdout.expected
	printf "\tNOTICE\n" >> $testroot/stdout.expected
	echo -n "06) $short_commit1 $d1 $GOT_AUTHOR_8 " \
		>> $testroot/stdout.expected
	printf "\tREADME\n"  >> $testroot/stdout.expected
	echo -n "07) $short_commit4 $d4 $GOT_AUTHOR_8 " \
		>> $testroot/stdout.expected
	printf "\tCOPYING\n" >> $testroot/stdout.expected
	echo -n "08) $short_commit1 $d1 $GOT_AUTHOR_8 " \
		>> $testroot/stdout.expected
	printf "\tcodedocs/doxygen.conf\n" >> $testroot/stdout.expected
	echo -n "09) $short_commit1 $d1 $GOT_AUTHOR_8 " \
		>> $testroot/stdout.expected
	printf "\tcontrib/powerdns.solaris.init.d\n" \
		>> $testroot/stdout.expected
	echo -n "10) $short_commit1 $d1 $GOT_AUTHOR_8 " \
		>> $testroot/stdout.expected
	printf "\tpdns/named.conf.parsertest\n" >> $testroot/stdout.expected
	echo -n "11) $short_commit1 $d1 $GOT_AUTHOR_8 " \
		>> $testroot/stdout.expected
	printf "\tregression-tests/zones/unit.test\n" \
		>> $testroot/stdout.expected
	echo -n "12) $short_commit4 $d4 $GOT_AUTHOR_8 " \
		>> $testroot/stdout.expected
	printf "\tbuilder-support/gen-version\n" >> $testroot/stdout.expected
	echo "13) $short_commit1 $d1 $GOT_AUTHOR_8 " \
		>> $testroot/stdout.expected
	echo -n "14) $short_commit1 $d1 $GOT_AUTHOR_8 " \
		>> $testroot/stdout.expected
	echo "ACLOCAL_AMFLAGS = -I m4" \
		>> $testroot/stdout.expected
	echo "15) $short_commit1 $d1 $GOT_AUTHOR_8 " \
		>> $testroot/stdout.expected
	echo -n "16) $short_commit1 $d1 $GOT_AUTHOR_8 " \
		>> $testroot/stdout.expected
	echo "dvi: # do nothing to build dvi" \
		>> $testroot/stdout.expected

	cmp -s $testroot/stdout.expected $testroot/stdout
	ret="$?"
	if [ "$ret" != "0" ]; then
		diff -u $testroot/stdout.expected $testroot/stdout
		test_done "$testroot" "$ret"
		return 1
	fi

	blame_cmp "$testroot" "alpha"
	ret="$?"
	test_done "$testroot" "$ret"
}

run_test test_blame_basic
run_test test_blame_tag
run_test test_blame_file_single_line
run_test test_blame_file_single_line_no_newline
run_test test_blame_all_lines_replaced
run_test test_blame_lines_shifted_up
run_test test_blame_lines_shifted_down
run_test test_blame_commit_subsumed
