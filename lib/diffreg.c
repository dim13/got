/*	$OpenBSD: diffreg.c,v 1.91 2016/03/01 20:57:35 natano Exp $	*/

/*
 * Copyright (C) Caldera International Inc.  2001-2002.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code and documentation must retain the above
 *    copyright notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed or owned by Caldera
 *	International, Inc.
 * 4. Neither the name of Caldera International, Inc. nor the names of other
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * USE OF THE SOFTWARE PROVIDED FOR UNDER THIS LICENSE BY CALDERA
 * INTERNATIONAL, INC. AND CONTRIBUTORS ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL CALDERA INTERNATIONAL, INC. BE LIABLE FOR ANY DIRECT,
 * INDIRECT INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
/*-
 * Copyright (c) 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)diffreg.c   8.1 (Berkeley) 6/6/93
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/queue.h>

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <paths.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>
#include <sha1.h>
#include <zlib.h>

#include "got_error.h"
#include "got_object.h"
#include "got_diff.h"

#include "got_lib_diff.h"

#define MINIMUM(a, b)	(((a) < (b)) ? (a) : (b))
#define MAXIMUM(a, b)	(((a) > (b)) ? (a) : (b))

/*
 * diff - compare two files.
 */

/*
 *	Uses an algorithm due to Harold Stone, which finds
 *	a pair of longest identical subsequences in the two
 *	files.
 *
 *	The major goal is to generate the match vector J.
 *	J[i] is the index of the line in file1 corresponding
 *	to line i file0. J[i] = 0 if there is no
 *	such line in file1.
 *
 *	Lines are hashed so as to work in core. All potential
 *	matches are located by sorting the lines of each file
 *	on the hash (called ``value''). In particular, this
 *	collects the equivalence classes in file1 together.
 *	Subroutine equiv replaces the value of each line in
 *	file0 by the index of the first element of its
 *	matching equivalence in (the reordered) file1.
 *	To save space equiv squeezes file1 into a single
 *	array member in which the equivalence classes
 *	are simply concatenated, except that their first
 *	members are flagged by changing sign.
 *
 *	Next the indices that point into member are unsorted into
 *	array class according to the original order of file0.
 *
 *	The cleverness lies in routine stone. This marches
 *	through the lines of file0, developing a vector klist
 *	of "k-candidates". At step i a k-candidate is a matched
 *	pair of lines x,y (x in file0 y in file1) such that
 *	there is a common subsequence of length k
 *	between the first i lines of file0 and the first y
 *	lines of file1, but there is no such subsequence for
 *	any smaller y. x is the earliest possible mate to y
 *	that occurs in such a subsequence.
 *
 *	Whenever any of the members of the equivalence class of
 *	lines in file1 matable to a line in file0 has serial number
 *	less than the y of some k-candidate, that k-candidate
 *	with the smallest such y is replaced. The new
 *	k-candidate is chained (via pred) to the current
 *	k-1 candidate so that the actual subsequence can
 *	be recovered. When a member has serial number greater
 *	that the y of all k-candidates, the klist is extended.
 *	At the end, the longest subsequence is pulled out
 *	and placed in the array J by unravel
 *
 *	With J in hand, the matches there recorded are
 *	check'ed against reality to assure that no spurious
 *	matches have crept in due to hashing. If they have,
 *	they are broken, and "jackpot" is recorded--a harmless
 *	matter except that a true match for a spuriously
 *	mated line may now be unnecessarily reported as a change.
 *
 *	Much of the complexity of the program comes simply
 *	from trying to minimize core utilization and
 *	maximize the range of doable problems by dynamically
 *	allocating what is needed and reusing what is not.
 *	The core requirements for problems larger than somewhat
 *	are (in words) 2*length(file0) + length(file1) +
 *	3*(number of k-candidates installed),  typically about
 *	6n words for files of length n.
 */

struct cand {
	int	x;
	int	y;
	int	pred;
};

struct line {
	int	serial;
	int	value;
};

static void	 diff_output(FILE *, const char *, ...);
static int	 output(FILE *, struct got_diff_changes *, struct got_diff_state *, struct got_diff_args *, const char *, FILE *, const char *, FILE *, int);
static void	 check(struct got_diff_state *, FILE *, FILE *, int);
static void	 range(FILE *, int, int, char *);
static void	 uni_range(FILE *, int, int);
static void	 dump_unified_vec(FILE *, struct got_diff_changes *, struct got_diff_state *, struct got_diff_args *, FILE *, FILE *, int);
static int	 prepare(struct got_diff_state *, int, FILE *, off_t, int);
static void	 prune(struct got_diff_state *);
static void	 equiv(struct line *, int, struct line *, int, int *);
static void	 unravel(struct got_diff_state *, int);
static int	 unsort(struct line *, int, int *);
static int	 change(FILE *, struct got_diff_changes *, struct got_diff_state *, struct got_diff_args *, const char *, FILE *, const char *, FILE *, int, int, int, int, int *);
static void	 sort(struct line *, int);
static void	 print_header(FILE *, struct got_diff_state *, struct got_diff_args *, const char *, const char *);
static int	 asciifile(FILE *);
static void	 fetch(FILE *, struct got_diff_state *, struct got_diff_args *, long *, int, int, FILE *, int, int);
static int	 newcand(struct got_diff_state *, int, int, int, int *);
static int	 search(struct got_diff_state *, int *, int, int);
static int	 skipline(FILE *);
static int	 isqrt(int);
static int	 stone(struct got_diff_state *, int *, int, int *, int *, int);
static int	 readhash(struct got_diff_state *, FILE *, int);
static int	 files_differ(struct got_diff_state *, FILE *, FILE *, int);
static char	*match_function(struct got_diff_state *, const long *, int, FILE *);

/*
 * chrtran points to one of 2 translation tables: cup2low if folding upper to
 * lower case clow2low if not folding case
 */
u_char clow2low[256] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
	0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
	0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20,
	0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b,
	0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
	0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x41,
	0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4c,
	0x4d, 0x4e, 0x4f, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
	0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f, 0x60, 0x61, 0x62,
	0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d,
	0x6e, 0x6f, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
	0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f, 0x80, 0x81, 0x82, 0x83,
	0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e,
	0x8f, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
	0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f, 0xa0, 0xa1, 0xa2, 0xa3, 0xa4,
	0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf,
	0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba,
	0xbb, 0xbc, 0xbd, 0xbe, 0xbf, 0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5,
	0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf, 0xd0,
	0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xdb,
	0xdc, 0xdd, 0xde, 0xdf, 0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6,
	0xe7, 0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef, 0xf0, 0xf1,
	0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xfb, 0xfc,
	0xfd, 0xfe, 0xff
};

u_char cup2low[256] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
	0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
	0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20,
	0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b,
	0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
	0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x60, 0x61,
	0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c,
	0x6d, 0x6e, 0x6f, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77,
	0x78, 0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f, 0x60, 0x61, 0x62,
	0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d,
	0x6e, 0x6f, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
	0x79, 0x7a, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f, 0x80, 0x81, 0x82, 0x83,
	0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e,
	0x8f, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
	0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f, 0xa0, 0xa1, 0xa2, 0xa3, 0xa4,
	0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf,
	0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba,
	0xbb, 0xbc, 0xbd, 0xbe, 0xbf, 0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5,
	0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf, 0xd0,
	0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xdb,
	0xdc, 0xdd, 0xde, 0xdf, 0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6,
	0xe7, 0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef, 0xf0, 0xf1,
	0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xfb, 0xfc,
	0xfd, 0xfe, 0xff
};

static void
diff_output(FILE *outfile, const char *fmt, ...)
{
	va_list ap;

	if (outfile == NULL)
		return;

	va_start(ap, fmt);
	vfprintf(outfile, fmt, ap);
	va_end(ap);
}

void
got_diff_state_free(struct got_diff_state *ds)
{
	free(ds->J);
	free(ds->member);
	free(ds->class);
	free(ds->clist);
	free(ds->klist);
	free(ds->ixold);
	free(ds->ixnew);
}

const struct got_error *
got_diffreg(int *rval, FILE *f1, FILE *f2, int flags,
    struct got_diff_args *args, struct got_diff_state *ds, FILE *outfile,
    struct got_diff_changes *changes)
{
	const struct got_error *err = NULL;
	int i, *p;
	long *lp;

	*rval = D_SAME;
	ds->anychange = 0;
	ds->lastline = 0;
	ds->lastmatchline = 0;
	ds->context_vec_ptr = ds->context_vec_start - 1;
	ds->max_context = GOT_DIFF_MAX_CONTEXT;
	if (flags & D_IGNORECASE)
		ds->chrtran = cup2low;
	else
		ds->chrtran = clow2low;
	if (S_ISDIR(ds->stb1.st_mode) != S_ISDIR(ds->stb2.st_mode)) {
		*rval = (S_ISDIR(ds->stb1.st_mode) ? D_MISMATCH1 : D_MISMATCH2);
		return NULL;
	}
	if (flags & D_EMPTY1) {
		f1 = fopen(_PATH_DEVNULL, "r");
		if (f1 == NULL) {
			err = got_error_from_errno2("fopen", _PATH_DEVNULL);
			goto closem;
		}
	}
	else if (f1 == NULL) {
		args->status |= 2;
		goto closem;
	}

	if (flags & D_EMPTY2) {
		f2 = fopen(_PATH_DEVNULL, "r");
		if (f2 == NULL) {
			err = got_error_from_errno2("fopen", _PATH_DEVNULL);
			goto closem;
		}
	} else if (f2 == NULL) {
		args->status |= 2;
		goto closem;
	}

	switch (files_differ(ds, f1, f2, flags)) {
	case 0:
		goto closem;
	case 1:
		break;
	default:
		/* error */
		args->status |= 2;
		goto closem;
	}

	if ((flags & D_FORCEASCII) == 0 &&
	    (!asciifile(f1) || !asciifile(f2))) {
		*rval = D_BINARY;
		args->status |= 1;
		goto closem;
	}
	if (prepare(ds, 0, f1, ds->stb1.st_size, flags)) {
		err = got_error_from_errno("prepare");
		goto closem;
	}
	if (prepare(ds, 1, f2, ds->stb2.st_size, flags)) {
		err = got_error_from_errno("prepare");
		goto closem;
	}

	prune(ds);
	sort(ds->sfile[0], ds->slen[0]);
	sort(ds->sfile[1], ds->slen[1]);

	ds->member = (int *)ds->file[1];
	equiv(ds->sfile[0], ds->slen[0], ds->sfile[1], ds->slen[1], ds->member);
	p = reallocarray(ds->member, ds->slen[1] + 2, sizeof(*ds->member));
	if (p == NULL) {
		err = got_error_from_errno("reallocarray");
		goto closem;
	}
	ds->member = p;

	ds->class = (int *)ds->file[0];
	if (unsort(ds->sfile[0], ds->slen[0], ds->class)) {
		err = got_error_from_errno("unsort");
		goto closem;
	}
	p = reallocarray(ds->class, ds->slen[0] + 2, sizeof(*ds->class));
	if (p == NULL) {
		err = got_error_from_errno("reallocarray");
		goto closem;
	}
	ds->class = p;

	ds->klist = calloc(ds->slen[0] + 2, sizeof(*ds->klist));
	if (ds->klist == NULL) {
		err = got_error_from_errno("calloc");
		goto closem;
	}
	ds->clen = 0;
	ds->clistlen = 100;
	ds->clist = calloc(ds->clistlen, sizeof(*ds->clist));
	if (ds->clist == NULL) {
		err = got_error_from_errno("calloc");
		goto closem;
	}
	i = stone(ds, ds->class, ds->slen[0], ds->member, ds->klist, flags);
	if (i < 0) {
		err = got_error_from_errno("stone");
		goto closem;
	}

	p = reallocarray(ds->J, ds->len[0] + 2, sizeof(*ds->J));
	if (p == NULL) {
		err = got_error_from_errno("reallocarray");
		goto closem;
	}
	ds->J = p;
	unravel(ds, ds->klist[i]);

	lp = reallocarray(ds->ixold, ds->len[0] + 2, sizeof(*ds->ixold));
	if (lp == NULL) {
		err = got_error_from_errno("reallocarray");
		goto closem;
	}
	ds->ixold = lp;
	lp = reallocarray(ds->ixnew, ds->len[1] + 2, sizeof(*ds->ixnew));
	if (lp == NULL) {
		err = got_error_from_errno("reallocarray");
		goto closem;
	}
	ds->ixnew = lp;
	check(ds, f1, f2, flags);
	if (output(outfile, changes, ds, args, args->label[0], f1,
	    args->label[1], f2, flags))
		err = got_error_from_errno("output");
closem:
	if (ds->anychange) {
		args->status |= 1;
		if (*rval == D_SAME)
			*rval = D_DIFFER;
	}
	if ((flags & D_EMPTY1) && f1) {
		if (fclose(f1) != 0 && err == NULL)
			err = got_error_from_errno("fclose");
	}
	if ((flags & D_EMPTY2) && f2) {
		if (fclose(f2) != 0 && err == NULL)
			err = got_error_from_errno("fclose");
	}
	return (err);
}

/*
 * Check to see if the given files differ.
 * Returns 0 if they are the same, 1 if different, and -1 on error.
 * XXX - could use code from cmp(1) [faster]
 */
static int
files_differ(struct got_diff_state *ds, FILE *f1, FILE *f2, int flags)
{
	char buf1[BUFSIZ], buf2[BUFSIZ];
	size_t i, j;

	if ((flags & (D_EMPTY1|D_EMPTY2)) || ds->stb1.st_size != ds->stb2.st_size ||
	    (ds->stb1.st_mode & S_IFMT) != (ds->stb2.st_mode & S_IFMT))
		return (1);
	for (;;) {
		i = fread(buf1, 1, sizeof(buf1), f1);
		j = fread(buf2, 1, sizeof(buf2), f2);
		if ((!i && ferror(f1)) || (!j && ferror(f2)))
			return (-1);
		if (i != j)
			return (1);
		if (i == 0)
			return (0);
		if (memcmp(buf1, buf2, i) != 0)
			return (1);
	}
}

static int
prepare(struct got_diff_state *ds, int i, FILE *fd, off_t filesize, int flags)
{
	struct line *p, *q;
	int j, h;
	size_t sz;

	rewind(fd);

	sz = (filesize <= SIZE_MAX ? filesize : SIZE_MAX) / 25;
	if (sz < 100)
		sz = 100;

	p = calloc(sz + 3, sizeof(*p));
	if (p == NULL)
		return (-1);
	for (j = 0; (h = readhash(ds, fd, flags));) {
		if (j == sz) {
			sz = sz * 3 / 2;
			q = reallocarray(p, sz + 3, sizeof(*p));
			if (q == NULL) {
				free(p);
				return (-1);
			}
			p = q;
		}
		p[++j].value = h;
	}
	ds->len[i] = j;
	ds->file[i] = p;

	return (0);
}

static void
prune(struct got_diff_state *ds)
{
	int i, j;

	for (ds->pref = 0; ds->pref < ds->len[0] && ds->pref < ds->len[1] &&
	    ds->file[0][ds->pref + 1].value == ds->file[1][ds->pref + 1].value;
	    ds->pref++)
		;
	for (ds->suff = 0; ds->suff < ds->len[0] - ds->pref && ds->suff < ds->len[1] - ds->pref &&
	    ds->file[0][ds->len[0] - ds->suff].value == ds->file[1][ds->len[1] - ds->suff].value;
	    ds->suff++)
		;
	for (j = 0; j < 2; j++) {
		ds->sfile[j] = ds->file[j] + ds->pref;
		ds->slen[j] = ds->len[j] - ds->pref - ds->suff;
		for (i = 0; i <= ds->slen[j]; i++)
			ds->sfile[j][i].serial = i;
	}
}

static void
equiv(struct line *a, int n, struct line *b, int m, int *c)
{
	int i, j;

	i = j = 1;
	while (i <= n && j <= m) {
		if (a[i].value < b[j].value)
			a[i++].value = 0;
		else if (a[i].value == b[j].value)
			a[i++].value = j;
		else
			j++;
	}
	while (i <= n)
		a[i++].value = 0;
	b[m + 1].value = 0;
	j = 0;
	while (++j <= m) {
		c[j] = -b[j].serial;
		while (b[j + 1].value == b[j].value) {
			j++;
			c[j] = b[j].serial;
		}
	}
	c[j] = -1;
}

/* Code taken from ping.c */
static int
isqrt(int n)
{
	int y, x = 1;

	if (n == 0)
		return (0);

	do { /* newton was a stinker */
		y = x;
		x = n / x;
		x += y;
		x /= 2;
	} while ((x - y) > 1 || (x - y) < -1);

	return (x);
}

static int
stone(struct got_diff_state *ds, int *a, int n, int *b, int *c, int flags)
{
	int i, k, y, j, l;
	int oldc, tc, oldl, sq;
	u_int numtries, bound;
	int error;

	if (flags & D_MINIMAL)
		bound = UINT_MAX;
	else {
		sq = isqrt(n);
		bound = MAXIMUM(256, sq);
	}

	k = 0;
	c[0] = newcand(ds, 0, 0, 0, &error);
	if (error)
		return -1;
	for (i = 1; i <= n; i++) {
		j = a[i];
		if (j == 0)
			continue;
		y = -b[j];
		oldl = 0;
		oldc = c[0];
		numtries = 0;
		do {
			if (y <= ds->clist[oldc].y)
				continue;
			l = search(ds, c, k, y);
			if (l != oldl + 1)
				oldc = c[l - 1];
			if (l <= k) {
				if (ds->clist[c[l]].y <= y)
					continue;
				tc = c[l];
				c[l] = newcand(ds, i, y, oldc, &error);
				if (error)
					return -1;
				oldc = tc;
				oldl = l;
				numtries++;
			} else {
				c[l] = newcand(ds, i, y, oldc, &error);
				if (error)
					return -1;
				k++;
				break;
			}
		} while ((y = b[++j]) > 0 && numtries < bound);
	}
	return (k);
}

static int
newcand(struct got_diff_state *ds, int x, int y, int pred, int *errorp)
{
	struct cand *q;

	if (ds->clen == ds->clistlen) {
		ds->clistlen = ds->clistlen * 11 / 10;
		q = reallocarray(ds->clist, ds->clistlen, sizeof(*ds->clist));
		if (q == NULL) {
			*errorp = -1;
			free(ds->clist);
			ds->clist = NULL;
			return 0;
		}
		ds->clist = q;
	}
	q = ds->clist + ds->clen;
	q->x = x;
	q->y = y;
	q->pred = pred;
	*errorp = 0;
	return (ds->clen++);
}

static int
search(struct got_diff_state *ds, int *c, int k, int y)
{
	int i, j, l, t;

	if (ds->clist[c[k]].y < y)	/* quick look for typical case */
		return (k + 1);
	i = 0;
	j = k + 1;
	for (;;) {
		l = (i + j) / 2;
		if (l <= i)
			break;
		t = ds->clist[c[l]].y;
		if (t > y)
			j = l;
		else if (t < y)
			i = l;
		else
			return (l);
	}
	return (l + 1);
}

static void
unravel(struct got_diff_state *ds, int p)
{
	struct cand *q;
	int i;

	for (i = 0; i <= ds->len[0]; i++)
		ds->J[i] = i <= ds->pref ? i :
		    i > ds->len[0] - ds->suff ? i + ds->len[1] - ds->len[0] : 0;
	for (q = ds->clist + p; q->y != 0; q = ds->clist + q->pred)
		ds->J[q->x + ds->pref] = q->y + ds->pref;
}

/*
 * Check does double duty:
 *  1.	ferret out any fortuitous correspondences due
 *	to confounding by hashing (which result in "jackpot")
 *  2.  collect random access indexes to the two files
 */
static void
check(struct got_diff_state *ds, FILE *f1, FILE *f2, int flags)
{
	int i, j, jackpot, c, d;
	long ctold, ctnew;

	rewind(f1);
	rewind(f2);
	j = 1;
	ds->ixold[0] = ds->ixnew[0] = 0;
	jackpot = 0;
	ctold = ctnew = 0;
	for (i = 1; i <= ds->len[0]; i++) {
		if (ds->J[i] == 0) {
			ds->ixold[i] = ctold += skipline(f1);
			continue;
		}
		while (j < ds->J[i]) {
			ds->ixnew[j] = ctnew += skipline(f2);
			j++;
		}
		if (flags & (D_FOLDBLANKS|D_IGNOREBLANKS|D_IGNORECASE)) {
			for (;;) {
				c = getc(f1);
				d = getc(f2);
				/*
				 * GNU diff ignores a missing newline
				 * in one file for -b or -w.
				 */
				if (flags & (D_FOLDBLANKS|D_IGNOREBLANKS)) {
					if (c == EOF && d == '\n') {
						ctnew++;
						break;
					} else if (c == '\n' && d == EOF) {
						ctold++;
						break;
					}
				}
				ctold++;
				ctnew++;
				if ((flags & D_FOLDBLANKS) && isspace(c) &&
				    isspace(d)) {
					do {
						if (c == '\n')
							break;
						ctold++;
					} while (isspace(c = getc(f1)));
					do {
						if (d == '\n')
							break;
						ctnew++;
					} while (isspace(d = getc(f2)));
				} else if ((flags & D_IGNOREBLANKS)) {
					while (isspace(c) && c != '\n') {
						c = getc(f1);
						ctold++;
					}
					while (isspace(d) && d != '\n') {
						d = getc(f2);
						ctnew++;
					}
				}
				if (ds->chrtran[c] != ds->chrtran[d]) {
					jackpot++;
					ds->J[i] = 0;
					if (c != '\n' && c != EOF)
						ctold += skipline(f1);
					if (d != '\n' && c != EOF)
						ctnew += skipline(f2);
					break;
				}
				if (c == '\n' || c == EOF)
					break;
			}
		} else {
			for (;;) {
				ctold++;
				ctnew++;
				if ((c = getc(f1)) != (d = getc(f2))) {
					/* jackpot++; */
					ds->J[i] = 0;
					if (c != '\n' && c != EOF)
						ctold += skipline(f1);
					if (d != '\n' && c != EOF)
						ctnew += skipline(f2);
					break;
				}
				if (c == '\n' || c == EOF)
					break;
			}
		}
		ds->ixold[i] = ctold;
		ds->ixnew[j] = ctnew;
		j++;
	}
	for (; j <= ds->len[1]; j++)
		ds->ixnew[j] = ctnew += skipline(f2);
	/*
	 * if (jackpot)
	 *	fprintf(stderr, "jackpot\n");
	 */
}

/* shellsort CACM #201 */
static void
sort(struct line *a, int n)
{
	struct line *ai, *aim, w;
	int j, m = 0, k;

	if (n == 0)
		return;
	for (j = 1; j <= n; j *= 2)
		m = 2 * j - 1;
	for (m /= 2; m != 0; m /= 2) {
		k = n - m;
		for (j = 1; j <= k; j++) {
			for (ai = &a[j]; ai > a; ai -= m) {
				aim = &ai[m];
				if (aim < ai)
					break;	/* wraparound */
				if (aim->value > ai[0].value ||
				    (aim->value == ai[0].value &&
					aim->serial > ai[0].serial))
					break;
				w.value = ai[0].value;
				ai[0].value = aim->value;
				aim->value = w.value;
				w.serial = ai[0].serial;
				ai[0].serial = aim->serial;
				aim->serial = w.serial;
			}
		}
	}
}

static int
unsort(struct line *f, int l, int *b)
{
	int *a, i;

	a = calloc(l + 1, sizeof(*a));
	if (a == NULL)
		return (-1);
	for (i = 1; i <= l; i++)
		a[f[i].serial] = f[i].value;
	for (i = 1; i <= l; i++)
		b[i] = a[i];
	free(a);

	return (0);
}

static int
skipline(FILE *f)
{
	int i, c;

	for (i = 1; (c = getc(f)) != '\n' && c != EOF; i++)
		continue;
	return (i);
}

static int
output(FILE *outfile, struct got_diff_changes *changes,
    struct got_diff_state *ds, struct got_diff_args *args,
    const char *file1, FILE *f1, const char *file2, FILE *f2, int flags)
{
	int m, i0, i1, j0, j1;
	int error = 0;

	rewind(f1);
	rewind(f2);
	m = ds->len[0];
	ds->J[0] = 0;
	ds->J[m + 1] = ds->len[1] + 1;
	for (i0 = 1; i0 <= m; i0 = i1 + 1) {
		while (i0 <= m && ds->J[i0] == ds->J[i0 - 1] + 1)
			i0++;
		j0 = ds->J[i0 - 1] + 1;
		i1 = i0 - 1;
		while (i1 < m && ds->J[i1 + 1] == 0)
			i1++;
		j1 = ds->J[i1 + 1] - 1;
		ds->J[i1] = j1;
		error = change(outfile, changes, ds, args, file1, f1, file2, f2,
		    i0, i1, j0, j1, &flags);
		if (error)
			return (error);
	}
	if (m == 0) {
		error = change(outfile, changes, ds, args, file1, f1, file2, f2,
		    1, 0, 1, ds->len[1], &flags);
		if (error)
			return (error);
	}
	if (ds->anychange != 0 && args->diff_format == D_UNIFIED)
		dump_unified_vec(outfile, changes, ds, args, f1, f2, flags);

	return (0);
}

static void
range(FILE *outfile, int a, int b, char *separator)
{
	diff_output(outfile, "%d", a > b ? b : a);
	if (a < b)
		diff_output(outfile, "%s%d", separator, b);
}

static void
uni_range(FILE *outfile, int a, int b)
{
	if (a < b)
		diff_output(outfile, "%d,%d", a, b - a + 1);
	else if (a == b)
		diff_output(outfile, "%d", b);
	else
		diff_output(outfile, "%d,0", b);
}

/*
 * Indicate that there is a difference between lines a and b of the from file
 * to get to lines c to d of the to file.  If a is greater then b then there
 * are no lines in the from file involved and this means that there were
 * lines appended (beginning at b).  If c is greater than d then there are
 * lines missing from the to file.
 */
static int
change(FILE *outfile, struct got_diff_changes *changes,
    struct got_diff_state *ds, struct got_diff_args *args,
    const char *file1, FILE *f1, const char *file2, FILE *f2,
    int a, int b, int c, int d, int *pflags)
{
	if (a > b && c > d)
		return (0);

	if (*pflags & D_HEADER) {
		diff_output(outfile, "%s %s %s\n", args->diffargs, file1, file2);
		*pflags &= ~D_HEADER;
	}
	if (args->diff_format == D_UNIFIED) {
		/*
		 * Allocate change records as needed.
		 */
		if (ds->context_vec_ptr == ds->context_vec_end - 1) {
			struct context_vec *cvp;
			ptrdiff_t offset;
			offset = ds->context_vec_ptr - ds->context_vec_start;
			ds->max_context <<= 1;
			cvp = reallocarray(ds->context_vec_start,
			    ds->max_context, sizeof(*ds->context_vec_start));
			if (cvp == NULL) {
				free(ds->context_vec_start);
				return (-1);
			}
			ds->context_vec_start = cvp;
			ds->context_vec_end = ds->context_vec_start +
			    ds->max_context;
			ds->context_vec_ptr = ds->context_vec_start + offset;
		}
		if (ds->anychange == 0) {
			/*
			 * Print the context/unidiff header first time through.
			 */
			print_header(outfile, ds, args, file1, file2);
			ds->anychange = 1;
		} else if (a > ds->context_vec_ptr->b + (2 * args->diff_context) + 1 &&
		    c > ds->context_vec_ptr->d + (2 * args->diff_context) + 1) {
			/*
			 * If this change is more than 'diff_context' lines from the
			 * previous change, dump the record and reset it.
			 */
			dump_unified_vec(outfile, changes, ds, args, f1, f2,
			    *pflags);
		}
		ds->context_vec_ptr++;
		ds->context_vec_ptr->a = a;
		ds->context_vec_ptr->b = b;
		ds->context_vec_ptr->c = c;
		ds->context_vec_ptr->d = d;
		return (0);
	}
	if (ds->anychange == 0)
		ds->anychange = 1;
	if (args->diff_format == D_BRIEF)
		return (0);
	if (args->diff_format == D_NORMAL) {
		range(outfile, a, b, ",");
		diff_output(outfile, "%c", a > b ? 'a' : c > d ? 'd' : 'c');
		range(outfile, c, d, ",");
		diff_output(outfile, "\n");
		fetch(outfile, ds, args, ds->ixold, a, b, f1, '<', *pflags);
		if (a <= b && c <= d)
			diff_output(outfile, "---\n");
	}
	fetch(outfile, ds, args, ds->ixnew, c, d, f2,
	    args->diff_format == D_NORMAL ? '>' : '\0', *pflags);
	return (0);
}

static void
fetch(FILE *outfile, struct got_diff_state *ds, struct got_diff_args *args,
    long *f, int a, int b, FILE *lb, int ch, int flags)
{
	int i, j, c, col, nc;

	if (a > b)
		return;
	for (i = a; i <= b; i++) {
		fseek(lb, f[i - 1], SEEK_SET);
		nc = f[i] - f[i - 1];
		if (ch != '\0') {
			diff_output(outfile, "%c", ch);
			if (args->Tflag && (args->diff_format == D_UNIFIED ||
			    args->diff_format == D_NORMAL))
				diff_output(outfile, "\t");
			else if (args->diff_format != D_UNIFIED)
				diff_output(outfile, " ");
		}
		col = 0;
		for (j = 0; j < nc; j++) {
			if ((c = getc(lb)) == EOF) {
				diff_output(outfile, "\n\\ No newline at end of "
				    "file\n");
				return;
			}
			if (c == '\t' && (flags & D_EXPANDTABS)) {
				do {
					diff_output(outfile, " ");
				} while (++col & 7);
			} else {
				diff_output(outfile, "%c", c);
				col++;
			}
		}
	}
}

/*
 * Hash function taken from Robert Sedgewick, Algorithms in C, 3d ed., p 578.
 */
static int
readhash(struct got_diff_state *ds, FILE *f, int flags)
{
	int i, t, space;
	int sum;

	sum = 1;
	space = 0;
	if ((flags & (D_FOLDBLANKS|D_IGNOREBLANKS)) == 0) {
		if (flags & D_IGNORECASE)
			for (i = 0; (t = getc(f)) != '\n'; i++) {
				if (t == EOF) {
					if (i == 0)
						return (0);
					break;
				}
				sum = sum * 127 + ds->chrtran[t];
			}
		else
			for (i = 0; (t = getc(f)) != '\n'; i++) {
				if (t == EOF) {
					if (i == 0)
						return (0);
					break;
				}
				sum = sum * 127 + t;
			}
	} else {
		for (i = 0;;) {
			switch (t = getc(f)) {
			case '\t':
			case '\r':
			case '\v':
			case '\f':
			case ' ':
				space++;
				continue;
			default:
				if (space && (flags & D_IGNOREBLANKS) == 0) {
					i++;
					space = 0;
				}
				sum = sum * 127 + ds->chrtran[t];
				i++;
				continue;
			case EOF:
				if (i == 0)
					return (0);
				/* FALLTHROUGH */
			case '\n':
				break;
			}
			break;
		}
	}
	/*
	 * There is a remote possibility that we end up with a zero sum.
	 * Zero is used as an EOF marker, so return 1 instead.
	 */
	return (sum == 0 ? 1 : sum);
}

static int
asciifile(FILE *f)
{
	unsigned char buf[BUFSIZ];
	size_t cnt;

	if (f == NULL)
		return (1);

	rewind(f);
	cnt = fread(buf, 1, sizeof(buf), f);
	return (memchr(buf, '\0', cnt) == NULL);
}

#define begins_with(s, pre) (strncmp(s, pre, sizeof(pre)-1) == 0)

static char *
match_function(struct got_diff_state *ds, const long *f, int pos, FILE *fp)
{
	unsigned char buf[FUNCTION_CONTEXT_SIZE];
	size_t nc;
	int last = ds->lastline;
	char *state = NULL;

	ds->lastline = pos;
	while (pos > last) {
		fseek(fp, f[pos - 1], SEEK_SET);
		nc = f[pos] - f[pos - 1];
		if (nc >= sizeof(buf))
			nc = sizeof(buf) - 1;
		nc = fread(buf, 1, nc, fp);
		if (nc > 0) {
			buf[nc] = '\0';
			buf[strcspn(buf, "\n")] = '\0';
			if (isalpha(buf[0]) || buf[0] == '_' || buf[0] == '$') {
				if (begins_with(buf, "private:")) {
					if (!state)
						state = " (private)";
				} else if (begins_with(buf, "protected:")) {
					if (!state)
						state = " (protected)";
				} else if (begins_with(buf, "public:")) {
					if (!state)
						state = " (public)";
				} else {
					strlcpy(ds->lastbuf, buf, sizeof ds->lastbuf);
					if (state)
						strlcat(ds->lastbuf, state,
						    sizeof ds->lastbuf);
					ds->lastmatchline = pos;
					return ds->lastbuf;
				}
			}
		}
		pos--;
	}
	return ds->lastmatchline > 0 ? ds->lastbuf : NULL;
}

/* dump accumulated "unified" diff changes */
static void
dump_unified_vec(FILE *outfile, struct got_diff_changes *changes,
    struct got_diff_state *ds, struct got_diff_args *args,
    FILE *f1, FILE *f2, int flags)
{
	struct context_vec *cvp = ds->context_vec_start;
	int lowa, upb, lowc, upd;
	int a, b, c, d;
	char ch, *f;

	if (ds->context_vec_start > ds->context_vec_ptr)
		return;

	b = d = 0;		/* gcc */
	lowa = MAXIMUM(1, cvp->a - args->diff_context);
	upb = MINIMUM(ds->len[0], ds->context_vec_ptr->b + args->diff_context);
	lowc = MAXIMUM(1, cvp->c - args->diff_context);
	upd = MINIMUM(ds->len[1], ds->context_vec_ptr->d + args->diff_context);

	diff_output(outfile, "@@ -");
	uni_range(outfile, lowa, upb);
	diff_output(outfile, " +");
	uni_range(outfile, lowc, upd);
	diff_output(outfile, " @@");
	if ((flags & D_PROTOTYPE)) {
		f = match_function(ds, ds->ixold, lowa-1, f1);
		if (f != NULL)
			diff_output(outfile, " %s", f);
	}
	diff_output(outfile, "\n");

	/*
	 * Output changes in "unified" diff format--the old and new lines
	 * are printed together.
	 */
	for (; cvp <= ds->context_vec_ptr; cvp++) {
		if (changes) {
			struct got_diff_change *change;
			change = calloc(1, sizeof(*change));
			if (change) {
				memcpy(&change->cv, cvp, sizeof(change->cv));
				SIMPLEQ_INSERT_TAIL(&changes->entries, change,
				    entry);
				changes->nchanges++;
			}
		}

		a = cvp->a;
		b = cvp->b;
		c = cvp->c;
		d = cvp->d;

		/*
		 * c: both new and old changes
		 * d: only changes in the old file
		 * a: only changes in the new file
		 */
		if (a <= b && c <= d)
			ch = 'c';
		else
			ch = (a <= b) ? 'd' : 'a';

		switch (ch) {
		case 'c':
			fetch(outfile, ds, args, ds->ixold, lowa, a - 1, f1, ' ', flags);
			fetch(outfile, ds, args, ds->ixold, a, b, f1, '-', flags);
			fetch(outfile, ds, args, ds->ixnew, c, d, f2, '+', flags);
			break;
		case 'd':
			fetch(outfile, ds, args, ds->ixold, lowa, a - 1, f1, ' ', flags);
			fetch(outfile, ds, args, ds->ixold, a, b, f1, '-', flags);
			break;
		case 'a':
			fetch(outfile, ds, args, ds->ixnew, lowc, c - 1, f2, ' ', flags);
			fetch(outfile, ds, args, ds->ixnew, c, d, f2, '+', flags);
			break;
		}
		lowa = b + 1;
		lowc = d + 1;
	}
	fetch(outfile, ds, args, ds->ixnew, d + 1, upd, f2, ' ', flags);

	ds->context_vec_ptr = ds->context_vec_start - 1;
}

void
got_diff_dump_change(FILE *outfile, struct got_diff_change *change,
    struct got_diff_state *ds, struct got_diff_args *args,
    FILE *f1, FILE *f2, int diff_flags)
{
	ds->context_vec_ptr = &change->cv;
	ds->context_vec_start = &change->cv;
	ds->context_vec_end = &change->cv;

	/* XXX TODO needs error checking */
	dump_unified_vec(outfile, NULL, ds, args, f1, f2, diff_flags);
}

static void
print_header(FILE *outfile, struct got_diff_state *ds, struct got_diff_args *args,
    const char *file1, const char *file2)
{
	if (args->label[0] != NULL)
		diff_output(outfile, "--- %s\n", args->label[0]);
	else
		diff_output(outfile, "--- %s\t%s", file1,
		    ctime(&ds->stb1.st_mtime));
	if (args->label[1] != NULL)
		diff_output(outfile, "+++ %s\n", args->label[1]);
	else
		diff_output(outfile, "+++ %s\t%s", file2,
		    ctime(&ds->stb2.st_mtime));
}
