/*
 * Copyright (c) 2018 Stefan Sperling <stsp@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

struct got_pathlist_head;

const struct got_error *got_object_qid_alloc_partial(struct got_object_qid **);
struct got_commit_object *got_object_commit_alloc_partial(void);
struct got_tree_entry *got_alloc_tree_entry_partial(void);

const struct got_error *got_object_parse_commit(struct got_commit_object **,
    char *, size_t);

/*
 * In a pathlist returned by got_object_parse_tree(), a pointer to the entry's
 * name is stored in the pathlist element's path, while the pathlist element's
 * data pointer points to a struct got_parsed_tree_entry.
 */
struct got_parsed_tree_entry {
	mode_t mode; /* Mode parsed from tree buffer. */
	uint8_t *id; /* Points to ID in parsed tree buffer. */
};
const struct got_error *got_object_parse_tree(struct got_pathlist_head *, int *,
    uint8_t *, size_t);

const struct got_error *got_object_parse_tag(struct got_tag_object **,
    uint8_t *, size_t);
const struct got_error *got_read_file_to_mem(uint8_t **, size_t *, FILE *);

struct got_pack;
struct got_packidx;

const struct got_error *got_object_parse_header(struct got_object **, char *, size_t);
const struct got_error *got_object_read_header(struct got_object **, int);
