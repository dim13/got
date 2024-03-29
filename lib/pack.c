/*
 * Copyright (c) 2018, 2019 Stefan Sperling <stsp@openbsd.org>
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

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/queue.h>
#include <sys/uio.h>
#include <sys/mman.h>

#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sha1.h>
#include <endian.h>
#include <zlib.h>
#include <imsg.h>

#include "got_error.h"
#include "got_object.h"
#include "got_opentemp.h"
#include "got_path.h"

#include "got_lib_sha1.h"
#include "got_lib_delta.h"
#include "got_lib_delta_cache.h"
#include "got_lib_inflate.h"
#include "got_lib_object.h"
#include "got_lib_object_parse.h"
#include "got_lib_privsep.h"
#include "got_lib_pack.h"

#ifndef nitems
#define nitems(_a) (sizeof(_a) / sizeof((_a)[0]))
#endif

#ifndef MIN
#define	MIN(_a,_b) ((_a) < (_b) ? (_a) : (_b))
#endif

static const struct got_error *
verify_fanout_table(uint32_t *fanout_table)
{
	int i;

	for (i = 0; i < 0xff - 1; i++) {
		if (be32toh(fanout_table[i]) > be32toh(fanout_table[i + 1]))
			return got_error(GOT_ERR_BAD_PACKIDX);
	}

	return NULL;
}

const struct got_error *
got_packidx_init_hdr(struct got_packidx *p, int verify)
{
	const struct got_error *err = NULL;
	struct got_packidx_v2_hdr *h;
	SHA1_CTX ctx;
	uint8_t sha1[SHA1_DIGEST_LENGTH];
	size_t nobj, len_fanout, len_ids, offset, remain;
	ssize_t n;
	int i;

	SHA1Init(&ctx);

	h = &p->hdr;
	offset = 0;
	remain = p->len;

	if (remain < sizeof(*h->magic)) {
		err = got_error(GOT_ERR_BAD_PACKIDX);
		goto done;
	}
	if (p->map)
		h->magic = (uint32_t *)(p->map + offset);
	else {
		h->magic = malloc(sizeof(*h->magic));
		if (h->magic == NULL) {
			err = got_error_from_errno("malloc");
			goto done;
		}
		n = read(p->fd, h->magic, sizeof(*h->magic));
		if (n < 0) {
			err = got_error_from_errno("read");
			goto done;
		} else if (n != sizeof(*h->magic)) {
			err = got_error(GOT_ERR_BAD_PACKIDX);
			goto done;
		}
	}
	if (betoh32(*h->magic) != GOT_PACKIDX_V2_MAGIC) {
		err = got_error(GOT_ERR_BAD_PACKIDX);
		goto done;
	}
	offset += sizeof(*h->magic);
	remain -= sizeof(*h->magic);

	if (verify)
		SHA1Update(&ctx, (uint8_t *)h->magic, sizeof(*h->magic));

	if (remain < sizeof(*h->version)) {
		err = got_error(GOT_ERR_BAD_PACKIDX);
		goto done;
	}
	if (p->map)
		h->version = (uint32_t *)(p->map + offset);
	else {
		h->version = malloc(sizeof(*h->version));
		if (h->version == NULL) {
			err = got_error_from_errno("malloc");
			goto done;
		}
		n = read(p->fd, h->version, sizeof(*h->version));
		if (n < 0) {
			err = got_error_from_errno("read");
			goto done;
		} else if (n != sizeof(*h->version)) {
			err = got_error(GOT_ERR_BAD_PACKIDX);
			goto done;
		}
	}
	if (betoh32(*h->version) != GOT_PACKIDX_VERSION) {
		err = got_error(GOT_ERR_BAD_PACKIDX);
		goto done;
	}
	offset += sizeof(*h->version);
	remain -= sizeof(*h->version);

	if (verify)
		SHA1Update(&ctx, (uint8_t *)h->version, sizeof(*h->version));

	len_fanout =
	    sizeof(*h->fanout_table) * GOT_PACKIDX_V2_FANOUT_TABLE_ITEMS;
	if (remain < len_fanout) {
		err = got_error(GOT_ERR_BAD_PACKIDX);
		goto done;
	}
	if (p->map)
		h->fanout_table = (uint32_t *)(p->map + offset);
	else {
		h->fanout_table = malloc(len_fanout);
		if (h->fanout_table == NULL) {
			err = got_error_from_errno("malloc");
			goto done;
		}
		n = read(p->fd, h->fanout_table, len_fanout);
		if (n < 0) {
			err = got_error_from_errno("read");
			goto done;
		} else if (n != len_fanout) {
			err = got_error(GOT_ERR_BAD_PACKIDX);
			goto done;
		}
	}
	err = verify_fanout_table(h->fanout_table);
	if (err)
		goto done;
	if (verify)
		SHA1Update(&ctx, (uint8_t *)h->fanout_table, len_fanout);
	offset += len_fanout;
	remain -= len_fanout;

	nobj = betoh32(h->fanout_table[0xff]);
	len_ids = nobj * sizeof(*h->sorted_ids);
	if (len_ids <= nobj || len_ids > remain) {
		err = got_error(GOT_ERR_BAD_PACKIDX);
		goto done;
	}
	if (p->map)
		h->sorted_ids =
		    (struct got_packidx_object_id *)((uint8_t*)(p->map + offset));
	else {
		h->sorted_ids = malloc(len_ids);
		if (h->sorted_ids == NULL) {
			err = got_error(GOT_ERR_BAD_PACKIDX);
			goto done;
		}
		n = read(p->fd, h->sorted_ids, len_ids);
		if (n < 0)
			err = got_error_from_errno("read");
		else if (n != len_ids) {
			err = got_error(GOT_ERR_BAD_PACKIDX);
			goto done;
		}
	}
	if (verify)
		SHA1Update(&ctx, (uint8_t *)h->sorted_ids, len_ids);
	offset += len_ids;
	remain -= len_ids;

	if (remain < nobj * sizeof(*h->crc32)) {
		err = got_error(GOT_ERR_BAD_PACKIDX);
		goto done;
	}
	if (p->map)
		h->crc32 = (uint32_t *)((uint8_t*)(p->map + offset));
	else {
		h->crc32 = malloc(nobj * sizeof(*h->crc32));
		if (h->crc32 == NULL) {
			err = got_error_from_errno("malloc");
			goto done;
		}
		n = read(p->fd, h->crc32, nobj * sizeof(*h->crc32));
		if (n < 0)
			err = got_error_from_errno("read");
		else if (n != nobj * sizeof(*h->crc32)) {
			err = got_error(GOT_ERR_BAD_PACKIDX);
			goto done;
		}
	}
	if (verify)
		SHA1Update(&ctx, (uint8_t *)h->crc32, nobj * sizeof(*h->crc32));
	remain -= nobj * sizeof(*h->crc32);
	offset += nobj * sizeof(*h->crc32);

	if (remain < nobj * sizeof(*h->offsets)) {
		err = got_error(GOT_ERR_BAD_PACKIDX);
		goto done;
	}
	if (p->map)
		h->offsets = (uint32_t *)((uint8_t*)(p->map + offset));
	else {
		h->offsets = malloc(nobj * sizeof(*h->offsets));
		if (h->offsets == NULL) {
			err = got_error_from_errno("malloc");
			goto done;
		}
		n = read(p->fd, h->offsets, nobj * sizeof(*h->offsets));
		if (n < 0)
			err = got_error_from_errno("read");
		else if (n != nobj * sizeof(*h->offsets)) {
			err = got_error(GOT_ERR_BAD_PACKIDX);
			goto done;
		}
	}
	if (verify)
		SHA1Update(&ctx, (uint8_t *)h->offsets,
		    nobj * sizeof(*h->offsets));
	remain -= nobj * sizeof(*h->offsets);
	offset += nobj * sizeof(*h->offsets);

	/* Large file offsets are contained only in files > 2GB. */
	for (i = 0; i < nobj; i++) {
		uint32_t o = betoh32(h->offsets[i]);
		if (o & GOT_PACKIDX_OFFSET_VAL_IS_LARGE_IDX)
			p->nlargeobj++;
	}
	if (p->nlargeobj == 0)
		goto checksum;

	if (remain < p->nlargeobj * sizeof(*h->large_offsets)) {
		err = got_error(GOT_ERR_BAD_PACKIDX);
		goto done;
	}
	if (p->map)
		h->large_offsets = (uint64_t *)((uint8_t*)(p->map + offset));
	else {
		h->large_offsets = malloc(p->nlargeobj *
		    sizeof(*h->large_offsets));
		if (h->large_offsets == NULL) {
			err = got_error_from_errno("malloc");
			goto done;
		}
		n = read(p->fd, h->large_offsets,
		    p->nlargeobj * sizeof(*h->large_offsets));
		if (n < 0)
			err = got_error_from_errno("read");
		else if (n != p->nlargeobj * sizeof(*h->large_offsets)) {
			err = got_error(GOT_ERR_BAD_PACKIDX);
			goto done;
		}
	}
	if (verify)
		SHA1Update(&ctx, (uint8_t*)h->large_offsets,
		    p->nlargeobj * sizeof(*h->large_offsets));
	remain -= p->nlargeobj * sizeof(*h->large_offsets);
	offset += p->nlargeobj * sizeof(*h->large_offsets);

checksum:
	if (remain < sizeof(*h->trailer)) {
		err = got_error(GOT_ERR_BAD_PACKIDX);
		goto done;
	}
	if (p->map)
		h->trailer =
		    (struct got_packidx_trailer *)((uint8_t*)(p->map + offset));
	else {
		h->trailer = malloc(sizeof(*h->trailer));
		if (h->trailer == NULL) {
			err = got_error_from_errno("malloc");
			goto done;
		}
		n = read(p->fd, h->trailer, sizeof(*h->trailer));
		if (n < 0)
			err = got_error_from_errno("read");
		else if (n != sizeof(*h->trailer)) {
			err = got_error(GOT_ERR_BAD_PACKIDX);
			goto done;
		}
	}
	if (verify) {
		SHA1Update(&ctx, h->trailer->packfile_sha1, SHA1_DIGEST_LENGTH);
		SHA1Final(sha1, &ctx);
		if (memcmp(h->trailer->packidx_sha1, sha1,
		    SHA1_DIGEST_LENGTH) != 0)
			err = got_error(GOT_ERR_PACKIDX_CSUM);
	}
done:
	return err;
}

const struct got_error *
got_packidx_open(struct got_packidx **packidx, const char *path, int verify)
{
	const struct got_error *err = NULL;
	struct got_packidx *p;
	struct stat sb;

	*packidx = NULL;

	p = calloc(1, sizeof(*p));
	if (p == NULL)
		return got_error_from_errno("calloc");

	p->fd = open(path, O_RDONLY | O_NOFOLLOW);
	if (p->fd == -1) {
		err = got_error_from_errno2("open", path);
		free(p);
		return err;
	}

	if (fstat(p->fd, &sb) != 0) {
		err = got_error_from_errno2("fstat", path);
		close(p->fd);
		free(p);
		return err;
	}
	p->len = sb.st_size;
	if (p->len < sizeof(p->hdr)) {
		err = got_error(GOT_ERR_BAD_PACKIDX);
		close(p->fd);
		free(p);
		return err;
	}

	p->path_packidx = strdup(path);
	if (p->path_packidx == NULL) {
		err = got_error_from_errno("strdup");
		goto done;
	}

#ifndef GOT_PACK_NO_MMAP
	p->map = mmap(NULL, p->len, PROT_READ, MAP_PRIVATE, p->fd, 0);
	if (p->map == MAP_FAILED) {
		if (errno != ENOMEM) {
			err = got_error_from_errno("mmap");
			goto done;
		}
		p->map = NULL; /* fall back to read(2) */
	}
#endif

	err = got_packidx_init_hdr(p, verify);
done:
	if (err)
		got_packidx_close(p);
	else
		*packidx = p;

	return err;
}

const struct got_error *
got_packidx_close(struct got_packidx *packidx)
{
	const struct got_error *err = NULL;

	free(packidx->path_packidx);
	if (packidx->map) {
		if (munmap(packidx->map, packidx->len) == -1)
			err = got_error_from_errno("munmap");
	} else {
		free(packidx->hdr.magic);
		free(packidx->hdr.version);
		free(packidx->hdr.fanout_table);
		free(packidx->hdr.sorted_ids);
		free(packidx->hdr.crc32);
		free(packidx->hdr.offsets);
		free(packidx->hdr.large_offsets);
		free(packidx->hdr.trailer);
	}
	if (close(packidx->fd) != 0 && err == NULL)
		err = got_error_from_errno("close");
	free(packidx);

	return err;
}

static off_t
get_object_offset(struct got_packidx *packidx, int idx)
{
	uint32_t offset = betoh32(packidx->hdr.offsets[idx]);
	if (offset & GOT_PACKIDX_OFFSET_VAL_IS_LARGE_IDX) {
		uint64_t loffset;
		idx = offset & GOT_PACKIDX_OFFSET_VAL_MASK;
		if (idx < 0 || idx >= packidx->nlargeobj ||
		    packidx->hdr.large_offsets == NULL)
			return -1;
		loffset = betoh64(packidx->hdr.large_offsets[idx]);
		return (loffset > INT64_MAX ? -1 : (off_t)loffset);
	}
	return (off_t)(offset & GOT_PACKIDX_OFFSET_VAL_MASK);
}

int
got_packidx_get_object_idx(struct got_packidx *packidx, struct got_object_id *id)
{
	u_int8_t id0 = id->sha1[0];
	uint32_t totobj = betoh32(packidx->hdr.fanout_table[0xff]);
	int left = 0, right = totobj - 1;

	if (id0 > 0)
		left = betoh32(packidx->hdr.fanout_table[id0 - 1]);

	while (left <= right) {
		struct got_packidx_object_id *oid;
		int i, cmp;

		i = ((left + right) / 2);
		oid = &packidx->hdr.sorted_ids[i];
		cmp = memcmp(id->sha1, oid->sha1, SHA1_DIGEST_LENGTH);
		if (cmp == 0)
			return i;
		else if (cmp > 0)
			left = i + 1;
		else if (cmp < 0)
			right = i - 1;
	}

	return -1;
}

const struct got_error *
got_packidx_match_id_str_prefix(struct got_object_id_queue *matched_ids,
    struct got_packidx *packidx, const char *id_str_prefix)
{
	const struct got_error *err = NULL;
	u_int8_t id0;
	uint32_t totobj = betoh32(packidx->hdr.fanout_table[0xff]);
	char hex[3];
	size_t prefix_len = strlen(id_str_prefix);
	struct got_packidx_object_id *oid;
	int i;

	SIMPLEQ_INIT(matched_ids);

	if (prefix_len < 2)
		return got_error_path(id_str_prefix, GOT_ERR_BAD_OBJ_ID_STR);

	hex[0] = id_str_prefix[0];
	hex[1] = id_str_prefix[1];
	hex[2] = '\0';
	if (!got_parse_xdigit(&id0, hex))
		return got_error_path(id_str_prefix, GOT_ERR_BAD_OBJ_ID_STR);

	i = betoh32(packidx->hdr.fanout_table[id0 - 1]);
	if (i == 0)
		return NULL;

	oid = &packidx->hdr.sorted_ids[i];
	while (i < totobj && oid->sha1[0] == id0) {
		char id_str[SHA1_DIGEST_STRING_LENGTH];
		struct got_object_qid *qid;
		int cmp;

		if (!got_sha1_digest_to_str(oid->sha1, id_str, sizeof(id_str)))
		        return got_error(GOT_ERR_NO_SPACE);

		cmp = strncmp(id_str, id_str_prefix, prefix_len);
		if (cmp < 0) {
			oid = &packidx->hdr.sorted_ids[++i];
			continue;
		} else if (cmp > 0)
			break;

		err = got_object_qid_alloc_partial(&qid);
		if (err)
			break;
		memcpy(qid->id->sha1, oid->sha1, SHA1_DIGEST_LENGTH);
		SIMPLEQ_INSERT_TAIL(matched_ids, qid, entry);

		oid = &packidx->hdr.sorted_ids[++i];
	}

	if (err)
		got_object_id_queue_free(matched_ids);
	return err;
}

const struct got_error *
got_pack_stop_privsep_child(struct got_pack *pack)
{
	const struct got_error *err = NULL;

	if (pack->privsep_child == NULL)
		return NULL;

	err = got_privsep_send_stop(pack->privsep_child->imsg_fd);
	if (err)
		return err;
	err = got_privsep_wait_for_child(pack->privsep_child->pid);
	if (close(pack->privsep_child->imsg_fd) != 0 && err == NULL)
		err = got_error_from_errno("close");
	free(pack->privsep_child);
	pack->privsep_child = NULL;
	return err;
}

const struct got_error *
got_pack_close(struct got_pack *pack)
{
	const struct got_error *err = NULL;

	err = got_pack_stop_privsep_child(pack);
	if (pack->map && munmap(pack->map, pack->filesize) == -1 && !err)
		err = got_error_from_errno("munmap");
	if (pack->fd != -1 && close(pack->fd) != 0 && err == NULL)
		err = got_error_from_errno("close");
	pack->fd = -1;
	free(pack->path_packfile);
	pack->path_packfile = NULL;
	pack->filesize = 0;
	if (pack->delta_cache) {
		got_delta_cache_free(pack->delta_cache);
		pack->delta_cache = NULL;
	}

	return err;
}

static const struct got_error *
parse_object_type_and_size(uint8_t *type, uint64_t *size, size_t *len,
    struct got_pack *pack, off_t offset)
{
	uint8_t t = 0;
	uint64_t s = 0;
	uint8_t sizeN;
	size_t mapoff = 0;
	int i = 0;

	*len = 0;

	if (offset >= pack->filesize)
		return got_error(GOT_ERR_PACK_OFFSET);

	if (pack->map) {
		mapoff = (size_t)offset;
	} else {
		if (lseek(pack->fd, offset, SEEK_SET) == -1)
			return got_error_from_errno("lseek");
	}

	do {
		/* We do not support size values which don't fit in 64 bit. */
		if (i > 9)
			return got_error(GOT_ERR_NO_SPACE);

		if (pack->map) {
			sizeN = *(pack->map + mapoff);
			mapoff += sizeof(sizeN);
		} else {
			ssize_t n = read(pack->fd, &sizeN, sizeof(sizeN));
			if (n < 0)
				return got_error_from_errno("read");
			if (n != sizeof(sizeN))
				return got_error(GOT_ERR_BAD_PACKFILE);
		}
		*len += sizeof(sizeN);

		if (i == 0) {
			t = (sizeN & GOT_PACK_OBJ_SIZE0_TYPE_MASK) >>
			    GOT_PACK_OBJ_SIZE0_TYPE_MASK_SHIFT;
			s = (sizeN & GOT_PACK_OBJ_SIZE0_VAL_MASK);
		} else {
			size_t shift = 4 + 7 * (i - 1);
			s |= ((sizeN & GOT_PACK_OBJ_SIZE_VAL_MASK) << shift);
		}
		i++;
	} while (sizeN & GOT_PACK_OBJ_SIZE_MORE);

	*type = t;
	*size = s;
	return NULL;
}

static const struct got_error *
open_plain_object(struct got_object **obj, struct got_object_id *id,
    uint8_t type, off_t offset, size_t size, int idx)
{
	*obj = calloc(1, sizeof(**obj));
	if (*obj == NULL)
		return got_error_from_errno("calloc");

	(*obj)->type = type;
	(*obj)->flags = GOT_OBJ_FLAG_PACKED;
	(*obj)->pack_idx = idx;
	(*obj)->hdrlen = 0;
	(*obj)->size = size;
	memcpy(&(*obj)->id, id, sizeof((*obj)->id));
	(*obj)->pack_offset = offset;

	return NULL;
}

static const struct got_error *
parse_negative_offset(int64_t *offset, size_t *len, struct got_pack *pack,
    off_t delta_offset)
{
	int64_t o = 0;
	uint8_t offN;
	int i = 0;

	*offset = 0;
	*len = 0;

	do {
		/* We do not support offset values which don't fit in 64 bit. */
		if (i > 8)
			return got_error(GOT_ERR_NO_SPACE);

		if (pack->map) {
			size_t mapoff;
			if (delta_offset >= pack->filesize)
				return got_error(GOT_ERR_PACK_OFFSET);
			mapoff = (size_t)delta_offset + *len;
			offN = *(pack->map + mapoff);
		} else {
			ssize_t n;
			n = read(pack->fd, &offN, sizeof(offN));
			if (n < 0)
				return got_error_from_errno("read");
			if (n != sizeof(offN))
				return got_error(GOT_ERR_BAD_PACKFILE);
		}
		*len += sizeof(offN);

		if (i == 0)
			o = (offN & GOT_PACK_OBJ_DELTA_OFF_VAL_MASK);
		else {
			o++;
			o <<= 7;
			o += (offN & GOT_PACK_OBJ_DELTA_OFF_VAL_MASK);
		}
		i++;
	} while (offN & GOT_PACK_OBJ_DELTA_OFF_MORE);

	*offset = o;
	return NULL;
}

static const struct got_error *
parse_offset_delta(off_t *base_offset, size_t *len, struct got_pack *pack,
    off_t offset, int tslen)
{
	const struct got_error *err;
	int64_t negoffset;
	size_t negofflen;

	*len = 0;

	err = parse_negative_offset(&negoffset, &negofflen, pack,
	    offset + tslen);
	if (err)
		return err;

	/* Compute the base object's offset (must be in the same pack file). */
	*base_offset = (offset - negoffset);
	if (*base_offset <= 0)
		return got_error(GOT_ERR_BAD_PACKFILE);

	*len = negofflen;
	return NULL;
}

static const struct got_error *
resolve_delta_chain(struct got_delta_chain *, struct got_packidx *,
    struct got_pack *, off_t, size_t, int, size_t, unsigned int);

static const struct got_error *
read_delta_data(uint8_t **delta_buf, size_t *delta_len,
    size_t delta_data_offset, struct got_pack *pack)
{
	const struct got_error *err = NULL;

	if (pack->map) {
		if (delta_data_offset >= pack->filesize)
			return got_error(GOT_ERR_PACK_OFFSET);
		err = got_inflate_to_mem_mmap(delta_buf, delta_len, pack->map,
		    delta_data_offset, pack->filesize - delta_data_offset);
	} else {
		if (lseek(pack->fd, delta_data_offset, SEEK_SET) == -1)
			return got_error_from_errno("lseek");
		err = got_inflate_to_mem_fd(delta_buf, delta_len, pack->fd);
	}
	return err;
}

static const struct got_error *
add_delta(struct got_delta_chain *deltas, off_t delta_offset, size_t tslen,
    int delta_type, size_t delta_size, size_t delta_data_offset)
{
	struct got_delta *delta;

	delta = got_delta_open(delta_offset, tslen, delta_type, delta_size,
	    delta_data_offset);
	if (delta == NULL)
		return got_error_from_errno("got_delta_open");
	/* delta is freed in got_object_close() */
	deltas->nentries++;
	SIMPLEQ_INSERT_HEAD(&deltas->entries, delta, entry);
	return NULL;
}

static const struct got_error *
resolve_offset_delta(struct got_delta_chain *deltas,
    struct got_packidx *packidx, struct got_pack *pack, off_t delta_offset,
    size_t tslen, int delta_type, size_t delta_size, unsigned int recursion)

{
	const struct got_error *err;
	off_t base_offset;
	uint8_t base_type;
	uint64_t base_size;
	size_t base_tslen;
	off_t delta_data_offset;
	size_t consumed;

	err = parse_offset_delta(&base_offset, &consumed, pack,
	    delta_offset, tslen);
	if (err)
		return err;

	delta_data_offset = delta_offset + tslen + consumed;
	if (delta_data_offset >= pack->filesize)
		return got_error(GOT_ERR_PACK_OFFSET);

	if (pack->map == NULL) {
		delta_data_offset = lseek(pack->fd, 0, SEEK_CUR);
		if (delta_data_offset == -1)
			return got_error_from_errno("lseek");
	}

	err = add_delta(deltas, delta_offset, tslen, delta_type, delta_size,
	    delta_data_offset);
	if (err)
		return err;

	/* An offset delta must be in the same packfile. */
	if (base_offset >= pack->filesize)
		return got_error(GOT_ERR_PACK_OFFSET);

	err = parse_object_type_and_size(&base_type, &base_size, &base_tslen,
	    pack, base_offset);
	if (err)
		return err;

	return resolve_delta_chain(deltas, packidx, pack, base_offset,
	    base_tslen, base_type, base_size, recursion - 1);
}

static const struct got_error *
resolve_ref_delta(struct got_delta_chain *deltas, struct got_packidx *packidx,
    struct got_pack *pack, off_t delta_offset, size_t tslen, int delta_type,
    size_t delta_size, unsigned int recursion)
{
	const struct got_error *err;
	struct got_object_id id;
	int idx;
	off_t base_offset;
	uint8_t base_type;
	uint64_t base_size;
	size_t base_tslen;
	off_t delta_data_offset;
	uint8_t *delta_buf;
	size_t delta_len;

	if (delta_offset + tslen >= pack->filesize)
		return got_error(GOT_ERR_PACK_OFFSET);

	if (pack->map == NULL) {
	}

	if (pack->map) {
		size_t mapoff = delta_offset + tslen;
		memcpy(&id, pack->map + mapoff, sizeof(id));
		mapoff += sizeof(id);
		delta_data_offset = (off_t)mapoff;
		err = got_inflate_to_mem_mmap(&delta_buf, &delta_len, pack->map,
		    mapoff, pack->filesize - mapoff);
		if (err)
			return err;
	} else {
		ssize_t n;
		delta_data_offset = lseek(pack->fd, 0, SEEK_CUR);
		if (delta_data_offset == -1)
			return got_error_from_errno("lseek");
		n = read(pack->fd, &id, sizeof(id));
		if (n < 0)
			return got_error_from_errno("read");
		if (n != sizeof(id))
			return got_error(GOT_ERR_BAD_PACKFILE);
		err = got_inflate_to_mem_fd(&delta_buf, &delta_len, pack->fd);
		if (err)
			return err;
	}

	err = add_delta(deltas, delta_offset, tslen, delta_type, delta_size,
	    delta_data_offset);
	if (err)
		return err;

	/* Delta base must be in the same pack file. */
	idx = got_packidx_get_object_idx(packidx, &id);
	if (idx == -1)
		return got_error(GOT_ERR_BAD_PACKFILE);

	base_offset = get_object_offset(packidx, idx);
	if (base_offset == (uint64_t)-1)
		return got_error(GOT_ERR_BAD_PACKIDX);

	if (base_offset >= pack->filesize)
		return got_error(GOT_ERR_PACK_OFFSET);

	err = parse_object_type_and_size(&base_type, &base_size, &base_tslen,
	    pack, base_offset);
	if (err)
		return err;

	return resolve_delta_chain(deltas, packidx, pack, base_offset,
	    base_tslen, base_type, base_size, recursion - 1);
}

static const struct got_error *
resolve_delta_chain(struct got_delta_chain *deltas, struct got_packidx *packidx,
    struct got_pack *pack, off_t delta_offset, size_t tslen, int delta_type,
    size_t delta_size, unsigned int recursion)
{
	const struct got_error *err = NULL;

	if (--recursion == 0)
		return got_error(GOT_ERR_RECURSION);

	switch (delta_type) {
	case GOT_OBJ_TYPE_COMMIT:
	case GOT_OBJ_TYPE_TREE:
	case GOT_OBJ_TYPE_BLOB:
	case GOT_OBJ_TYPE_TAG:
		/* Plain types are the final delta base. Recursion ends. */
		err = add_delta(deltas, delta_offset, tslen, delta_type,
		    delta_size, 0);
		break;
	case GOT_OBJ_TYPE_OFFSET_DELTA:
		err = resolve_offset_delta(deltas, packidx, pack,
		    delta_offset, tslen, delta_type, delta_size, recursion - 1);
		break;
	case GOT_OBJ_TYPE_REF_DELTA:
		err = resolve_ref_delta(deltas, packidx, pack,
		    delta_offset, tslen, delta_type, delta_size, recursion - 1);
		break;
	default:
		return got_error(GOT_ERR_OBJ_TYPE);
	}

	return err;
}

static const struct got_error *
open_delta_object(struct got_object **obj, struct got_packidx *packidx,
    struct got_pack *pack, struct got_object_id *id, off_t offset,
    size_t tslen, int delta_type, size_t delta_size, int idx)
{
	const struct got_error *err = NULL;
	int resolved_type;

	*obj = calloc(1, sizeof(**obj));
	if (*obj == NULL)
		return got_error_from_errno("calloc");

	(*obj)->flags = 0;
	(*obj)->hdrlen = 0;
	(*obj)->size = 0; /* Not known because deltas aren't applied yet. */
	memcpy(&(*obj)->id, id, sizeof((*obj)->id));
	(*obj)->pack_offset = offset + tslen;

	SIMPLEQ_INIT(&(*obj)->deltas.entries);
	(*obj)->flags |= GOT_OBJ_FLAG_DELTIFIED;
	(*obj)->flags |= GOT_OBJ_FLAG_PACKED;
	(*obj)->pack_idx = idx;

	err = resolve_delta_chain(&(*obj)->deltas, packidx, pack, offset,
	    tslen, delta_type, delta_size, GOT_DELTA_CHAIN_RECURSION_MAX);
	if (err)
		goto done;

	err = got_delta_chain_get_base_type(&resolved_type, &(*obj)->deltas);
	if (err)
		goto done;
	(*obj)->type = resolved_type;
done:
	if (err) {
		got_object_close(*obj);
		*obj = NULL;
	}
	return err;
}

const struct got_error *
got_packfile_open_object(struct got_object **obj, struct got_pack *pack,
    struct got_packidx *packidx, int idx, struct got_object_id *id)
{
	const struct got_error *err = NULL;
	off_t offset;
	uint8_t type;
	uint64_t size;
	size_t tslen;

	*obj = NULL;

	offset = get_object_offset(packidx, idx);
	if (offset == (uint64_t)-1)
		return got_error(GOT_ERR_BAD_PACKIDX);

	err = parse_object_type_and_size(&type, &size, &tslen, pack, offset);
	if (err)
		return err;

	switch (type) {
	case GOT_OBJ_TYPE_COMMIT:
	case GOT_OBJ_TYPE_TREE:
	case GOT_OBJ_TYPE_BLOB:
	case GOT_OBJ_TYPE_TAG:
		err = open_plain_object(obj, id, type, offset + tslen,
		    size, idx);
		break;
	case GOT_OBJ_TYPE_OFFSET_DELTA:
	case GOT_OBJ_TYPE_REF_DELTA:
		err = open_delta_object(obj, packidx, pack, id, offset,
		    tslen, type, size, idx);
		break;
	default:
		err = got_error(GOT_ERR_OBJ_TYPE);
		break;
	}

	return err;
}

static const struct got_error *
get_delta_chain_max_size(uint64_t *max_size, struct got_delta_chain *deltas,
    struct got_pack *pack)
{
	struct got_delta *delta;
	uint64_t base_size = 0, result_size = 0;

	*max_size = 0;
	SIMPLEQ_FOREACH(delta, &deltas->entries, entry) {
		/* Plain object types are the delta base. */
		if (delta->type != GOT_OBJ_TYPE_COMMIT &&
		    delta->type != GOT_OBJ_TYPE_TREE &&
		    delta->type != GOT_OBJ_TYPE_BLOB &&
		    delta->type != GOT_OBJ_TYPE_TAG) {
			const struct got_error *err;
			uint8_t *delta_buf;
			size_t delta_len;
			int cached = 1;

			got_delta_cache_get(&delta_buf, &delta_len,
			    pack->delta_cache, delta->data_offset);
			if (delta_buf == NULL) {
				cached = 0;
				err = read_delta_data(&delta_buf, &delta_len,
				    delta->data_offset, pack);
				if (err)
					return err;
				err = got_delta_cache_add(pack->delta_cache,
				    delta->data_offset, delta_buf, delta_len);
				if (err == NULL)
					cached = 1;
				else if (err->code != GOT_ERR_NO_SPACE) {
					free(delta_buf);
					return err;
				}
			}
			err = got_delta_get_sizes(&base_size, &result_size,
			    delta_buf, delta_len);
			if (!cached)
				free(delta_buf);
			if (err)
				return err;
		} else
			base_size = delta->size;
		if (base_size > *max_size)
			*max_size = base_size;
		if (result_size > *max_size)
			*max_size = result_size;
	}

	return NULL;
}

const struct got_error *
got_pack_get_max_delta_object_size(uint64_t *size, struct got_object *obj,
    struct got_pack *pack)
{
	if ((obj->flags & GOT_OBJ_FLAG_DELTIFIED) == 0)
		return got_error(GOT_ERR_OBJ_TYPE);

	return get_delta_chain_max_size(size, &obj->deltas, pack);
}

static const struct got_error *
dump_delta_chain_to_file(size_t *result_size, struct got_delta_chain *deltas,
    struct got_pack *pack, FILE *outfile, FILE *base_file, FILE *accum_file)
{
	const struct got_error *err = NULL;
	struct got_delta *delta;
	uint8_t *base_buf = NULL, *accum_buf = NULL, *delta_buf;
	size_t base_bufsz = 0, accum_size = 0, delta_len;
	uint64_t max_size;
	int n = 0;

	*result_size = 0;

	if (SIMPLEQ_EMPTY(&deltas->entries))
		return got_error(GOT_ERR_BAD_DELTA_CHAIN);

	/* We process small enough files entirely in memory for speed. */
	err = get_delta_chain_max_size(&max_size, deltas, pack);
	if (err)
		return err;
	if (max_size < GOT_DELTA_RESULT_SIZE_CACHED_MAX) {
		accum_buf = malloc(max_size);
		if (accum_buf == NULL)
			return got_error_from_errno("malloc");
		base_file = NULL;
		accum_file = NULL;
	}

	/* Deltas are ordered in ascending order. */
	SIMPLEQ_FOREACH(delta, &deltas->entries, entry) {
		int cached = 1;
		if (n == 0) {
			size_t mapoff;
			off_t delta_data_offset;

			/* Plain object types are the delta base. */
			if (delta->type != GOT_OBJ_TYPE_COMMIT &&
			    delta->type != GOT_OBJ_TYPE_TREE &&
			    delta->type != GOT_OBJ_TYPE_BLOB &&
			    delta->type != GOT_OBJ_TYPE_TAG) {
				err = got_error(GOT_ERR_BAD_DELTA_CHAIN);
				goto done;
			}

			delta_data_offset = delta->offset + delta->tslen;
			if (delta_data_offset >= pack->filesize) {
				err = got_error(GOT_ERR_PACK_OFFSET);
				goto done;
			}
			if (pack->map == NULL) {
				if (lseek(pack->fd, delta_data_offset, SEEK_SET)
				    == -1) {
					err = got_error_from_errno("lseek");
					goto done;
				}
			}
			if (base_file) {
				if (pack->map) {
					mapoff = (size_t)delta_data_offset;
					err = got_inflate_to_file_mmap(
					    &base_bufsz, pack->map, mapoff,
					    pack->filesize - mapoff, base_file);
				} else
					err = got_inflate_to_file_fd(
					    &base_bufsz, pack->fd, base_file);
			} else {
				if (pack->map) {
					mapoff = (size_t)delta_data_offset;
					err = got_inflate_to_mem_mmap(&base_buf,
					    &base_bufsz, pack->map, mapoff,
					    pack->filesize - mapoff);
				} else
					err = got_inflate_to_mem_fd(&base_buf,
					    &base_bufsz, pack->fd);
			}
			if (err)
				goto done;
			n++;
			if (base_file)
				rewind(base_file);
			continue;
		}

		got_delta_cache_get(&delta_buf, &delta_len,
		    pack->delta_cache, delta->data_offset);
		if (delta_buf == NULL) {
			cached = 0;
			err = read_delta_data(&delta_buf, &delta_len,
			    delta->data_offset, pack);
			if (err)
				goto done;
			err = got_delta_cache_add(pack->delta_cache,
			    delta->data_offset, delta_buf, delta_len);
			if (err == NULL)
				cached = 1;
			else if (err->code != GOT_ERR_NO_SPACE) {
				free(delta_buf);
				goto done;
			}
		}
		if (base_buf) {
			err = got_delta_apply_in_mem(base_buf, base_bufsz,
			    delta_buf, delta_len, accum_buf,
			    &accum_size, max_size);
			n++;
		} else {
			err = got_delta_apply(base_file, delta_buf,
			    delta_len,
			    /* Final delta application writes to output file. */
			    ++n < deltas->nentries ? accum_file : outfile,
			    &accum_size);
		}
		if (!cached)
			free(delta_buf);
		if (err)
			goto done;

		if (n < deltas->nentries) {
			/* Accumulated delta becomes the new base. */
			if (base_buf) {
				uint8_t *tmp = accum_buf;
				/*
				 * Base buffer switches roles with accumulation
				 * buffer. Ensure it can hold the largest
				 * result in the delta chain. The initial
				 * allocation might have been smaller.
				 */
				if (base_bufsz < max_size) {
					uint8_t *p;
					p = reallocarray(base_buf, 1, max_size);
					if (p == NULL) {
						err = got_error_from_errno(
						    "reallocarray");
						goto done;
					}
					base_buf = p;
					base_bufsz = max_size;
				}
				accum_buf = base_buf;
				base_buf = tmp;
			} else {
				FILE *tmp = accum_file;
				accum_file = base_file;
				base_file = tmp;
				rewind(base_file);
				rewind(accum_file);
			}
		}
	}

done:
	free(base_buf);
	if (accum_buf) {
		size_t len = fwrite(accum_buf, 1, accum_size, outfile);
		free(accum_buf);
		if (len != accum_size)
			err = got_ferror(outfile, GOT_ERR_IO);
	}
	rewind(outfile);
	if (err == NULL)
		*result_size = accum_size;
	return err;
}

static const struct got_error *
dump_delta_chain_to_mem(uint8_t **outbuf, size_t *outlen,
    struct got_delta_chain *deltas, struct got_pack *pack)
{
	const struct got_error *err = NULL;
	struct got_delta *delta;
	uint8_t *base_buf = NULL, *accum_buf = NULL, *delta_buf;
	size_t base_bufsz = 0, accum_size = 0, delta_len;
	uint64_t max_size;
	int n = 0;

	*outbuf = NULL;
	*outlen = 0;

	if (SIMPLEQ_EMPTY(&deltas->entries))
		return got_error(GOT_ERR_BAD_DELTA_CHAIN);

	err = get_delta_chain_max_size(&max_size, deltas, pack);
	if (err)
		return err;
	accum_buf = malloc(max_size);
	if (accum_buf == NULL)
		return got_error_from_errno("malloc");

	/* Deltas are ordered in ascending order. */
	SIMPLEQ_FOREACH(delta, &deltas->entries, entry) {
		int cached = 1;
		if (n == 0) {
			size_t delta_data_offset;

			/* Plain object types are the delta base. */
			if (delta->type != GOT_OBJ_TYPE_COMMIT &&
			    delta->type != GOT_OBJ_TYPE_TREE &&
			    delta->type != GOT_OBJ_TYPE_BLOB &&
			    delta->type != GOT_OBJ_TYPE_TAG) {
				err = got_error(GOT_ERR_BAD_DELTA_CHAIN);
				goto done;
			}

			delta_data_offset = delta->offset + delta->tslen;
			if (delta_data_offset >= pack->filesize) {
				err = got_error(GOT_ERR_PACK_OFFSET);
				goto done;
			}
			if (pack->map) {
				size_t mapoff = (size_t)delta_data_offset;
				err = got_inflate_to_mem_mmap(&base_buf,
				    &base_bufsz, pack->map, mapoff,
				    pack->filesize - mapoff);
			} else {
				if (lseek(pack->fd, delta_data_offset, SEEK_SET)
				    == -1) {
					err = got_error_from_errno("lseek");
					goto done;
				}
				err = got_inflate_to_mem_fd(&base_buf,
				    &base_bufsz, pack->fd);
			}
			if (err)
				goto done;
			n++;
			continue;
		}

		got_delta_cache_get(&delta_buf, &delta_len,
		    pack->delta_cache, delta->data_offset);
		if (delta_buf == NULL) {
			cached = 0;
			err = read_delta_data(&delta_buf, &delta_len,
			    delta->data_offset, pack);
			if (err)
				goto done;
			err = got_delta_cache_add(pack->delta_cache,
			    delta->data_offset, delta_buf, delta_len);
			if (err == NULL)
				cached = 1;
			else if (err->code != GOT_ERR_NO_SPACE) {
				free(delta_buf);
				goto done;
			}
		}
		err = got_delta_apply_in_mem(base_buf, base_bufsz,
		    delta_buf, delta_len, accum_buf,
		    &accum_size, max_size);
		if (!cached)
			free(delta_buf);
		n++;
		if (err)
			goto done;

		if (n < deltas->nentries) {
			/* Accumulated delta becomes the new base. */
			uint8_t *tmp = accum_buf;
			/*
			 * Base buffer switches roles with accumulation buffer.
			 * Ensure it can hold the largest result in the delta
			 * chain. Initial allocation might have been smaller.
			 */
			if (base_bufsz < max_size) {
				uint8_t *p;
				p = reallocarray(base_buf, 1, max_size);
				if (p == NULL) {
					err = got_error_from_errno(
					    "reallocarray");
					goto done;
				}
				base_buf = p;
				base_bufsz = max_size;
			}
			accum_buf = base_buf;
			base_buf = tmp;
		}
	}

done:
	free(base_buf);
	if (err) {
		free(accum_buf);
		*outbuf = NULL;
		*outlen = 0;
	} else {
		*outbuf = accum_buf;
		*outlen = accum_size;
	}
	return err;
}

const struct got_error *
got_packfile_extract_object(struct got_pack *pack, struct got_object *obj,
    FILE *outfile, FILE *base_file, FILE *accum_file)
{
	const struct got_error *err = NULL;

	if ((obj->flags & GOT_OBJ_FLAG_PACKED) == 0)
		return got_error(GOT_ERR_OBJ_NOT_PACKED);

	if ((obj->flags & GOT_OBJ_FLAG_DELTIFIED) == 0) {
		if (obj->pack_offset >= pack->filesize)
			return got_error(GOT_ERR_PACK_OFFSET);

		if (pack->map) {
			size_t mapoff = (size_t)obj->pack_offset;
			err = got_inflate_to_file_mmap(&obj->size, pack->map,
			    mapoff, pack->filesize - mapoff, outfile);
		} else {
			if (lseek(pack->fd, obj->pack_offset, SEEK_SET) == -1)
				return got_error_from_errno("lseek");
			err = got_inflate_to_file_fd(&obj->size, pack->fd,
			    outfile);
		}
	} else
		err = dump_delta_chain_to_file(&obj->size, &obj->deltas, pack,
		    outfile, base_file, accum_file);

	return err;
}

const struct got_error *
got_packfile_extract_object_to_mem(uint8_t **buf, size_t *len,
    struct got_object *obj, struct got_pack *pack)
{
	const struct got_error *err = NULL;

	if ((obj->flags & GOT_OBJ_FLAG_PACKED) == 0)
		return got_error(GOT_ERR_OBJ_NOT_PACKED);

	if ((obj->flags & GOT_OBJ_FLAG_DELTIFIED) == 0) {
		if (obj->pack_offset >= pack->filesize)
			return got_error(GOT_ERR_PACK_OFFSET);
		if (pack->map) {
			size_t mapoff = (size_t)obj->pack_offset;
			err = got_inflate_to_mem_mmap(buf, len, pack->map,
			    mapoff, pack->filesize - mapoff);
		} else {
			if (lseek(pack->fd, obj->pack_offset, SEEK_SET) == -1)
				return got_error_from_errno("lseek");
			err = got_inflate_to_mem_fd(buf, len, pack->fd);
		}
	} else
		err = dump_delta_chain_to_mem(buf, len, &obj->deltas, pack);

	return err;
}
