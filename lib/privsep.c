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
#include <sys/queue.h>
#include <sys/uio.h>
#include <sys/syslimits.h>
#include <sys/wait.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <poll.h>
#include <imsg.h>
#include <sha1.h>
#include <zlib.h>
#include <time.h>

#include "got_object.h"
#include "got_error.h"
#include "got_path.h"
#include "got_repository.h"

#include "got_lib_sha1.h"
#include "got_lib_delta.h"
#include "got_lib_inflate.h"
#include "got_lib_object.h"
#include "got_lib_object_parse.h"
#include "got_lib_privsep.h"
#include "got_lib_pack.h"

#ifndef MIN
#define	MIN(_a,_b) ((_a) < (_b) ? (_a) : (_b))
#endif

#ifndef nitems
#define nitems(_a)	(sizeof((_a)) / sizeof((_a)[0]))
#endif

static const struct got_error *
poll_fd(int fd, int events, int timeout)
{
	struct pollfd pfd[1];
	int n;

	pfd[0].fd = fd;
	pfd[0].events = events;

	n = poll(pfd, 1, timeout);
	if (n == -1)
		return got_error_from_errno("poll");
	if (n == 0)
		return got_error(GOT_ERR_TIMEOUT);
	if (pfd[0].revents & (POLLERR | POLLNVAL))
		return got_error_from_errno("poll error");
	if (pfd[0].revents & (events | POLLHUP))
		return NULL;

	return got_error(GOT_ERR_INTERRUPT);
}

static const struct got_error *
read_imsg(struct imsgbuf *ibuf)
{
	const struct got_error *err;
	size_t n;

	err = poll_fd(ibuf->fd, POLLIN, INFTIM);
	if (err)
		return err;

	n = imsg_read(ibuf);
	if (n == -1) {
		if (errno == EAGAIN) /* Could be a file-descriptor leak. */
			return got_error(GOT_ERR_PRIVSEP_NO_FD);
		return got_error(GOT_ERR_PRIVSEP_READ);
	}
	if (n == 0)
		return got_error(GOT_ERR_PRIVSEP_PIPE);

	return NULL;
}

const struct got_error *
got_privsep_wait_for_child(pid_t pid)
{
	int child_status;

	if (waitpid(pid, &child_status, 0) == -1)
		return got_error_from_errno("waitpid");

	if (!WIFEXITED(child_status))
		return got_error(GOT_ERR_PRIVSEP_DIED);

	if (WEXITSTATUS(child_status) != 0)
		return got_error(GOT_ERR_PRIVSEP_EXIT);

	return NULL;
}

static const struct got_error *
recv_imsg_error(struct imsg *imsg, size_t datalen)
{
	struct got_imsg_error *ierr;

	if (datalen != sizeof(*ierr))
		return got_error(GOT_ERR_PRIVSEP_LEN);

	ierr = imsg->data;
	if (ierr->code == GOT_ERR_ERRNO) {
		static struct got_error serr;
		serr.code = GOT_ERR_ERRNO;
		serr.msg = strerror(ierr->errno_code);
		return &serr;
	}

	return got_error(ierr->code);
}

const struct got_error *
got_privsep_recv_imsg(struct imsg *imsg, struct imsgbuf *ibuf,
    size_t min_datalen)
{
	const struct got_error *err;
	ssize_t n;

	n = imsg_get(ibuf, imsg);
	if (n == -1)
		return got_error_from_errno("imsg_get");

	while (n == 0) {
		err = read_imsg(ibuf);
		if (err)
			return err;
		n = imsg_get(ibuf, imsg);
	}

	if (imsg->hdr.len < IMSG_HEADER_SIZE + min_datalen)
		return got_error(GOT_ERR_PRIVSEP_LEN);

	if (imsg->hdr.type == GOT_IMSG_ERROR) {
		size_t datalen = imsg->hdr.len - IMSG_HEADER_SIZE;
		return recv_imsg_error(imsg, datalen);
	}

	return NULL;
}

/* Attempt to send an error in an imsg. Complain on stderr as a last resort. */
void
got_privsep_send_error(struct imsgbuf *ibuf, const struct got_error *err)
{
	const struct got_error *poll_err;
	struct got_imsg_error ierr;
	int ret;

	ierr.code = err->code;
	if (err->code == GOT_ERR_ERRNO)
		ierr.errno_code = errno;
	else
		ierr.errno_code = 0;
	ret = imsg_compose(ibuf, GOT_IMSG_ERROR, 0, 0, -1, &ierr, sizeof(ierr));
	if (ret == -1) {
		fprintf(stderr, "%s: error %d \"%s\": imsg_compose: %s\n",
		    getprogname(), err->code, err->msg, strerror(errno));
		return;
	}

	poll_err = poll_fd(ibuf->fd, POLLOUT, INFTIM);
	if (poll_err) {
		fprintf(stderr, "%s: error %d \"%s\": poll: %s\n",
		    getprogname(), err->code, err->msg, poll_err->msg);
		return;
	}

	ret = imsg_flush(ibuf);
	if (ret == -1) {
		fprintf(stderr, "%s: error %d \"%s\": imsg_flush: %s\n",
		    getprogname(), err->code, err->msg, strerror(errno));
		return;
	}
}

static const struct got_error *
flush_imsg(struct imsgbuf *ibuf)
{
	const struct got_error *err;

	err = poll_fd(ibuf->fd, POLLOUT, INFTIM);
	if (err)
		return err;

	if (imsg_flush(ibuf) == -1)
		return got_error_from_errno("imsg_flush");

	return NULL;
}

const struct got_error *
got_privsep_send_stop(int fd)
{
	const struct got_error *err = NULL;
	struct imsgbuf ibuf;

	imsg_init(&ibuf, fd);

	if (imsg_compose(&ibuf, GOT_IMSG_STOP, 0, 0, -1, NULL, 0) == -1)
		return got_error_from_errno("imsg_compose STOP");

	err = flush_imsg(&ibuf);
	imsg_clear(&ibuf);
	return err;
}

const struct got_error *
got_privsep_send_obj_req(struct imsgbuf *ibuf, int fd)
{
	if (imsg_compose(ibuf, GOT_IMSG_OBJECT_REQUEST, 0, 0, fd, NULL, 0)
	    == -1)
		return got_error_from_errno("imsg_compose OBJECT_REQUEST");

	return flush_imsg(ibuf);
}

const struct got_error *
got_privsep_send_commit_req(struct imsgbuf *ibuf, int fd,
    struct got_object_id *id, int pack_idx)
{
	const struct got_error *err = NULL;
	struct got_imsg_packed_object iobj, *iobjp;
	size_t len;

	if (id) { /* commit is packed */
		iobj.idx = pack_idx;
		memcpy(iobj.id, id->sha1, sizeof(iobj.id));
		iobjp = &iobj;
		len = sizeof(iobj);
	} else {
		iobjp = NULL;
		len = 0;
	}

	if (imsg_compose(ibuf, GOT_IMSG_COMMIT_REQUEST, 0, 0, fd, iobjp, len)
	    == -1) {
		err = got_error_from_errno("imsg_compose COMMIT_REQUEST");
		close(fd);
		return err;
	}

	return flush_imsg(ibuf);
}

const struct got_error *
got_privsep_send_tree_req(struct imsgbuf *ibuf, int fd,
    struct got_object_id *id, int pack_idx)
{
	const struct got_error *err = NULL;
	struct ibuf *wbuf;
	size_t len = id ? sizeof(struct got_imsg_packed_object) : 0;

	wbuf = imsg_create(ibuf, GOT_IMSG_TREE_REQUEST, 0, 0, len);
	if (wbuf == NULL)
		return got_error_from_errno("imsg_create TREE_REQUEST");

	if (id) { /* tree is packed */
		if (imsg_add(wbuf, id->sha1, SHA1_DIGEST_LENGTH) == -1) {
			err = got_error_from_errno("imsg_add TREE_ENTRY");
			ibuf_free(wbuf);
			return err;
		}

		if (imsg_add(wbuf, &pack_idx, sizeof(pack_idx)) == -1) {
			err = got_error_from_errno("imsg_add TREE_ENTRY");
			ibuf_free(wbuf);
			return err;
		}
	}

	wbuf->fd = fd;
	imsg_close(ibuf, wbuf);

	return flush_imsg(ibuf);
}

const struct got_error *
got_privsep_send_tag_req(struct imsgbuf *ibuf, int fd,
    struct got_object_id *id, int pack_idx)
{
	struct got_imsg_packed_object iobj, *iobjp;
	size_t len;

	if (id) { /* tag is packed */
		iobj.idx = pack_idx;
		memcpy(iobj.id, id->sha1, sizeof(iobj.id));
		iobjp = &iobj;
		len = sizeof(iobj);
	} else {
		iobjp = NULL;
		len = 0;
	}

	if (imsg_compose(ibuf, GOT_IMSG_TAG_REQUEST, 0, 0, fd, iobjp, len)
	    == -1)
		return got_error_from_errno("imsg_compose TAG_REQUEST");

	return flush_imsg(ibuf);
}

const struct got_error *
got_privsep_send_blob_req(struct imsgbuf *ibuf, int infd,
    struct got_object_id *id, int pack_idx)
{
	const struct got_error *err = NULL;
	struct got_imsg_packed_object iobj, *iobjp;
	size_t len;

	if (id) { /* blob is packed */
		iobj.idx = pack_idx;
		memcpy(iobj.id, id->sha1, sizeof(iobj.id));
		iobjp = &iobj;
		len = sizeof(iobj);
	} else {
		iobjp = NULL;
		len = 0;
	}

	if (imsg_compose(ibuf, GOT_IMSG_BLOB_REQUEST, 0, 0, infd, iobjp, len)
	    == -1) {
		err = got_error_from_errno("imsg_compose BLOB_REQUEST");
		close(infd);
		return err;
	}

	return flush_imsg(ibuf);
}

const struct got_error *
got_privsep_send_blob_outfd(struct imsgbuf *ibuf, int outfd)
{
	const struct got_error *err = NULL;

	if (imsg_compose(ibuf, GOT_IMSG_BLOB_OUTFD, 0, 0, outfd, NULL, 0)
	    == -1) {
		err = got_error_from_errno("imsg_compose BLOB_OUTFD");
		close(outfd);
		return err;
	}

	return flush_imsg(ibuf);
}

const struct got_error *
got_privsep_send_tmpfd(struct imsgbuf *ibuf, int fd)
{
	const struct got_error *err = NULL;

	if (imsg_compose(ibuf, GOT_IMSG_TMPFD, 0, 0, fd, NULL, 0)
	    == -1) {
		err = got_error_from_errno("imsg_compose TMPFD");
		close(fd);
		return err;
	}

	return flush_imsg(ibuf);
}

const struct got_error *
got_privsep_send_obj(struct imsgbuf *ibuf, struct got_object *obj)
{
	struct got_imsg_object iobj;

	memcpy(iobj.id, obj->id.sha1, sizeof(iobj.id));
	iobj.type = obj->type;
	iobj.flags = obj->flags;
	iobj.hdrlen = obj->hdrlen;
	iobj.size = obj->size;
	if (iobj.flags & GOT_OBJ_FLAG_PACKED) {
		iobj.pack_offset = obj->pack_offset;
		iobj.pack_idx = obj->pack_idx;
	}

	if (imsg_compose(ibuf, GOT_IMSG_OBJECT, 0, 0, -1, &iobj, sizeof(iobj))
	    == -1)
		return got_error_from_errno("imsg_compose OBJECT");

	return flush_imsg(ibuf);
}

const struct got_error *
got_privsep_get_imsg_obj(struct got_object **obj, struct imsg *imsg,
    struct imsgbuf *ibuf)
{
	const struct got_error *err = NULL;
	struct got_imsg_object *iobj;
	size_t datalen = imsg->hdr.len - IMSG_HEADER_SIZE;

	if (datalen != sizeof(*iobj))
		return got_error(GOT_ERR_PRIVSEP_LEN);
	iobj = imsg->data;

	*obj = calloc(1, sizeof(**obj));
	if (*obj == NULL)
		return got_error_from_errno("calloc");

	memcpy((*obj)->id.sha1, iobj->id, SHA1_DIGEST_LENGTH);
	(*obj)->type = iobj->type;
	(*obj)->flags = iobj->flags;
	(*obj)->hdrlen = iobj->hdrlen;
	(*obj)->size = iobj->size;
	/* path_packfile is handled by caller */
	if (iobj->flags & GOT_OBJ_FLAG_PACKED) {
		(*obj)->pack_offset = iobj->pack_offset;
		(*obj)->pack_idx = iobj->pack_idx;
	}

	return err;
}

const struct got_error *
got_privsep_recv_obj(struct got_object **obj, struct imsgbuf *ibuf)
{
	const struct got_error *err = NULL;
	struct imsg imsg;
	const size_t min_datalen =
	    MIN(sizeof(struct got_imsg_error), sizeof(struct got_imsg_object));

	*obj = NULL;

	err = got_privsep_recv_imsg(&imsg, ibuf, min_datalen);
	if (err)
		return err;

	switch (imsg.hdr.type) {
	case GOT_IMSG_OBJECT:
		err = got_privsep_get_imsg_obj(obj, &imsg, ibuf);
		break;
	default:
		err = got_error(GOT_ERR_PRIVSEP_MSG);
		break;
	}

	imsg_free(&imsg);

	return err;
}

static const struct got_error *
send_commit_logmsg(struct imsgbuf *ibuf, struct got_commit_object *commit,
    size_t logmsg_len)
{
	const struct got_error *err = NULL;
	size_t offset, remain;

	offset = 0;
	remain = logmsg_len;
	while (remain > 0) {
		size_t n = MIN(MAX_IMSGSIZE - IMSG_HEADER_SIZE, remain);

		if (imsg_compose(ibuf, GOT_IMSG_COMMIT_LOGMSG, 0, 0, -1,
		    commit->logmsg + offset, n) == -1) {
			err = got_error_from_errno("imsg_compose "
			    "COMMIT_LOGMSG");
			break;
		}

		err = flush_imsg(ibuf);
		if (err)
			break;

		offset += n;
		remain -= n;
	}

	return err;
}

const struct got_error *
got_privsep_send_commit(struct imsgbuf *ibuf, struct got_commit_object *commit)
{
	const struct got_error *err = NULL;
	struct got_imsg_commit_object *icommit;
	uint8_t *buf;
	size_t len, total;
	struct got_object_qid *qid;
	size_t author_len = strlen(commit->author);
	size_t committer_len = strlen(commit->committer);
	size_t logmsg_len = strlen(commit->logmsg);

	total = sizeof(*icommit) + author_len + committer_len +
	    commit->nparents * SHA1_DIGEST_LENGTH;

	buf = malloc(total);
	if (buf == NULL)
		return got_error_from_errno("malloc");

	icommit = (struct got_imsg_commit_object *)buf;
	memcpy(icommit->tree_id, commit->tree_id->sha1,
	    sizeof(icommit->tree_id));
	icommit->author_len = author_len;
	icommit->author_time = commit->author_time;
	icommit->author_gmtoff = commit->author_gmtoff;
	icommit->committer_len = committer_len;
	icommit->committer_time = commit->committer_time;
	icommit->committer_gmtoff = commit->committer_gmtoff;
	icommit->logmsg_len = logmsg_len;
	icommit->nparents = commit->nparents;

	len = sizeof(*icommit);
	memcpy(buf + len, commit->author, author_len);
	len += author_len;
	memcpy(buf + len, commit->committer, committer_len);
	len += committer_len;
	SIMPLEQ_FOREACH(qid, &commit->parent_ids, entry) {
		memcpy(buf + len, qid->id, SHA1_DIGEST_LENGTH);
		len += SHA1_DIGEST_LENGTH;
	}

	if (imsg_compose(ibuf, GOT_IMSG_COMMIT, 0, 0, -1, buf, len) == -1) {
		err = got_error_from_errno("imsg_compose COMMIT");
		goto done;
	}

	if (logmsg_len == 0 ||
	    logmsg_len + len > MAX_IMSGSIZE - IMSG_HEADER_SIZE) {
		err = flush_imsg(ibuf);
		if (err)
			goto done;
	}
	err = send_commit_logmsg(ibuf, commit, logmsg_len);
done:
	free(buf);
	return err;
}

const struct got_error *
got_privsep_recv_commit(struct got_commit_object **commit, struct imsgbuf *ibuf)
{
	const struct got_error *err = NULL;
	struct imsg imsg;
	struct got_imsg_commit_object *icommit;
	size_t len, datalen;
	int i;
	const size_t min_datalen =
	    MIN(sizeof(struct got_imsg_error),
	    sizeof(struct got_imsg_commit_object));

	*commit = NULL;

	err = got_privsep_recv_imsg(&imsg, ibuf, min_datalen);
	if (err)
		return err;

	datalen = imsg.hdr.len - IMSG_HEADER_SIZE;
	len = 0;

	switch (imsg.hdr.type) {
	case GOT_IMSG_COMMIT:
		if (datalen < sizeof(*icommit)) {
			err = got_error(GOT_ERR_PRIVSEP_LEN);
			break;
		}
		icommit = imsg.data;
		if (datalen != sizeof(*icommit) + icommit->author_len +
		    icommit->committer_len +
		    icommit->nparents * SHA1_DIGEST_LENGTH) {
			err = got_error(GOT_ERR_PRIVSEP_LEN);
			break;
		}
		if (icommit->nparents < 0) {
			err = got_error(GOT_ERR_PRIVSEP_LEN);
			break;
		}
		len += sizeof(*icommit);

		*commit = got_object_commit_alloc_partial();
		if (*commit == NULL) {
			err = got_error_from_errno(
			    "got_object_commit_alloc_partial");
			break;
		}

		memcpy((*commit)->tree_id->sha1, icommit->tree_id,
		    SHA1_DIGEST_LENGTH);
		(*commit)->author_time = icommit->author_time;
		(*commit)->author_gmtoff = icommit->author_gmtoff;
		(*commit)->committer_time = icommit->committer_time;
		(*commit)->committer_gmtoff = icommit->committer_gmtoff;

		if (icommit->author_len == 0) {
			(*commit)->author = strdup("");
			if ((*commit)->author == NULL) {
				err = got_error_from_errno("strdup");
				break;
			}
		} else {
			(*commit)->author = malloc(icommit->author_len + 1);
			if ((*commit)->author == NULL) {
				err = got_error_from_errno("malloc");
				break;
			}
			memcpy((*commit)->author, imsg.data + len,
			    icommit->author_len);
			(*commit)->author[icommit->author_len] = '\0';
		}
		len += icommit->author_len;

		if (icommit->committer_len == 0) {
			(*commit)->committer = strdup("");
			if ((*commit)->committer == NULL) {
				err = got_error_from_errno("strdup");
				break;
			}
		} else {
			(*commit)->committer =
			    malloc(icommit->committer_len + 1);
			if ((*commit)->committer == NULL) {
				err = got_error_from_errno("malloc");
				break;
			}
			memcpy((*commit)->committer, imsg.data + len,
			    icommit->committer_len);
			(*commit)->committer[icommit->committer_len] = '\0';
		}
		len += icommit->committer_len;

		if (icommit->logmsg_len == 0) {
			(*commit)->logmsg = strdup("");
			if ((*commit)->logmsg == NULL) {
				err = got_error_from_errno("strdup");
				break;
			}
		} else {
			size_t offset = 0, remain = icommit->logmsg_len;

			(*commit)->logmsg = malloc(icommit->logmsg_len + 1);
			if ((*commit)->logmsg == NULL) {
				err = got_error_from_errno("malloc");
				break;
			}
			while (remain > 0) {
				struct imsg imsg_log;
				size_t n = MIN(MAX_IMSGSIZE - IMSG_HEADER_SIZE,
				    remain);

				err = got_privsep_recv_imsg(&imsg_log, ibuf, n);
				if (err)
					return err;

				if (imsg_log.hdr.type != GOT_IMSG_COMMIT_LOGMSG)
					return got_error(GOT_ERR_PRIVSEP_MSG);

				memcpy((*commit)->logmsg + offset,
				    imsg_log.data, n);
				imsg_free(&imsg_log);
				offset += n;
				remain -= n;
			}
			(*commit)->logmsg[icommit->logmsg_len] = '\0';
		}

		for (i = 0; i < icommit->nparents; i++) {
			struct got_object_qid *qid;

			err = got_object_qid_alloc_partial(&qid);
			if (err)
				break;
			memcpy(qid->id, imsg.data + len +
			    i * SHA1_DIGEST_LENGTH, sizeof(*qid->id));
			SIMPLEQ_INSERT_TAIL(&(*commit)->parent_ids, qid, entry);
			(*commit)->nparents++;
		}
		break;
	default:
		err = got_error(GOT_ERR_PRIVSEP_MSG);
		break;
	}

	imsg_free(&imsg);

	return err;
}

const struct got_error *
got_privsep_send_tree(struct imsgbuf *ibuf, struct got_pathlist_head *entries,
    int nentries)
{
	const struct got_error *err = NULL;
	struct got_imsg_tree_object itree;
	struct got_pathlist_entry *pe;
	size_t totlen;
	int nimsg; /* number of imsg queued in ibuf */

	itree.nentries = nentries;
	if (imsg_compose(ibuf, GOT_IMSG_TREE, 0, 0, -1, &itree, sizeof(itree))
	    == -1)
		return got_error_from_errno("imsg_compose TREE");

	totlen = sizeof(itree);
	nimsg = 1;
	TAILQ_FOREACH(pe, entries, entry) {
		const char *name = pe->path;
		struct got_parsed_tree_entry *pte = pe->data;
		struct ibuf *wbuf;
		size_t namelen = strlen(name);
		size_t len = sizeof(struct got_imsg_tree_entry) + namelen;

		if (len > MAX_IMSGSIZE)
			return got_error(GOT_ERR_NO_SPACE);

		nimsg++;
		if (totlen + len >= MAX_IMSGSIZE - (IMSG_HEADER_SIZE * nimsg)) {
			err = flush_imsg(ibuf);
			if (err)
				return err;
			nimsg = 0;
		}

		wbuf = imsg_create(ibuf, GOT_IMSG_TREE_ENTRY, 0, 0, len);
		if (wbuf == NULL)
			return got_error_from_errno("imsg_create TREE_ENTRY");

		/* Keep in sync with struct got_imsg_tree_object definition! */
		if (imsg_add(wbuf, pte->id, SHA1_DIGEST_LENGTH) == -1) {
			err = got_error_from_errno("imsg_add TREE_ENTRY");
			ibuf_free(wbuf);
			return err;
		}
		if (imsg_add(wbuf, &pte->mode, sizeof(pte->mode)) == -1) {
			err = got_error_from_errno("imsg_add TREE_ENTRY");
			ibuf_free(wbuf);
			return err;
		}

		if (imsg_add(wbuf, name, namelen) == -1) {
			err = got_error_from_errno("imsg_add TREE_ENTRY");
			ibuf_free(wbuf);
			return err;
		}

		wbuf->fd = -1;
		imsg_close(ibuf, wbuf);

		totlen += len;
	}

	return flush_imsg(ibuf);
}

const struct got_error *
got_privsep_recv_tree(struct got_tree_object **tree, struct imsgbuf *ibuf)
{
	const struct got_error *err = NULL;
	const size_t min_datalen =
	    MIN(sizeof(struct got_imsg_error),
	    sizeof(struct got_imsg_tree_object));
	struct got_imsg_tree_object *itree;
	int nentries = 0;

	*tree = NULL;
get_more:
	err = read_imsg(ibuf);
	if (err)
		goto done;

	for (;;) {
		struct imsg imsg;
		size_t n;
		size_t datalen;
		struct got_imsg_tree_entry *ite;
		struct got_tree_entry *te = NULL;

		n = imsg_get(ibuf, &imsg);
		if (n == 0) {
			if (*tree && (*tree)->nentries != nentries)
				goto get_more;
			break;
		}

		if (imsg.hdr.len < IMSG_HEADER_SIZE + min_datalen)
			return got_error(GOT_ERR_PRIVSEP_LEN);

		datalen = imsg.hdr.len - IMSG_HEADER_SIZE;

		switch (imsg.hdr.type) {
		case GOT_IMSG_ERROR:
			err = recv_imsg_error(&imsg, datalen);
			break;
		case GOT_IMSG_TREE:
			/* This message should only appear once. */
			if (*tree != NULL) {
				err = got_error(GOT_ERR_PRIVSEP_MSG);
				break;
			}
			if (datalen != sizeof(*itree)) {
				err = got_error(GOT_ERR_PRIVSEP_LEN);
				break;
			}
			itree = imsg.data;
			*tree = malloc(sizeof(**tree));
			if (*tree == NULL) {
				err = got_error_from_errno("malloc");
				break;
			}
			(*tree)->entries = calloc(itree->nentries,
			    sizeof(struct got_tree_entry));
			if ((*tree)->entries == NULL) {
				err = got_error_from_errno("malloc");
				break;
			}
			(*tree)->nentries = itree->nentries;
			(*tree)->refcnt = 0;
			break;
		case GOT_IMSG_TREE_ENTRY:
			/* This message should be preceeded by GOT_IMSG_TREE. */
			if (*tree == NULL) {
				err = got_error(GOT_ERR_PRIVSEP_MSG);
				break;
			}
			if (datalen < sizeof(*ite) || datalen > MAX_IMSGSIZE) {
				err = got_error(GOT_ERR_PRIVSEP_LEN);
				break;
			}

			/* Remaining data contains the entry's name. */
			datalen -= sizeof(*ite);
			if (datalen == 0 || datalen > MAX_IMSGSIZE) {
				err = got_error(GOT_ERR_PRIVSEP_LEN);
				break;
			}
			ite = imsg.data;

			if (datalen + 1 > sizeof(te->name)) {
				err = got_error(GOT_ERR_NO_SPACE);
				break;
			}
			te = &(*tree)->entries[nentries];
			memcpy(te->name, imsg.data + sizeof(*ite), datalen);
			te->name[datalen] = '\0';

			memcpy(te->id.sha1, ite->id, SHA1_DIGEST_LENGTH);
			te->mode = ite->mode;
			te->idx = nentries;
			nentries++;
			break;
		default:
			err = got_error(GOT_ERR_PRIVSEP_MSG);
			break;
		}

		imsg_free(&imsg);
	}
done:
	if (*tree && (*tree)->nentries != nentries) {
		if (err == NULL)
			err = got_error(GOT_ERR_PRIVSEP_LEN);
		got_object_tree_close(*tree);
		*tree = NULL;
	}

	return err;
}

const struct got_error *
got_privsep_send_blob(struct imsgbuf *ibuf, size_t size, size_t hdrlen,
    const uint8_t *data)
{
	struct got_imsg_blob iblob;

	iblob.size = size;
	iblob.hdrlen = hdrlen;

	if (data) {
		uint8_t *buf;

		if (size > GOT_PRIVSEP_INLINE_BLOB_DATA_MAX)
			return got_error(GOT_ERR_NO_SPACE);

		buf = malloc(sizeof(iblob) + size);
		if (buf == NULL)
			return got_error_from_errno("malloc");

		memcpy(buf, &iblob, sizeof(iblob));
		memcpy(buf + sizeof(iblob), data, size);
		if (imsg_compose(ibuf, GOT_IMSG_BLOB, 0, 0, -1, buf,
		   sizeof(iblob) + size) == -1) {
			free(buf);
			return got_error_from_errno("imsg_compose BLOB");
		}
		free(buf);
	} else {
		/* Data has already been written to file descriptor. */
		if (imsg_compose(ibuf, GOT_IMSG_BLOB, 0, 0, -1, &iblob,
		    sizeof(iblob)) == -1)
			return got_error_from_errno("imsg_compose BLOB");
	}


	return flush_imsg(ibuf);
}

const struct got_error *
got_privsep_recv_blob(uint8_t **outbuf, size_t *size, size_t *hdrlen,
    struct imsgbuf *ibuf)
{
	const struct got_error *err = NULL;
	struct imsg imsg;
	struct got_imsg_blob *iblob;
	size_t datalen;

	*outbuf = NULL;

	err = got_privsep_recv_imsg(&imsg, ibuf, 0);
	if (err)
		return err;

	datalen = imsg.hdr.len - IMSG_HEADER_SIZE;

	switch (imsg.hdr.type) {
	case GOT_IMSG_BLOB:
		if (datalen < sizeof(*iblob)) {
			err = got_error(GOT_ERR_PRIVSEP_LEN);
			break;
		}
		iblob = imsg.data;
		*size = iblob->size;
		*hdrlen = iblob->hdrlen;

		if (datalen == sizeof(*iblob)) {
			/* Data has been written to file descriptor. */
			break;
		}

		if (*size > GOT_PRIVSEP_INLINE_BLOB_DATA_MAX) {
			err = got_error(GOT_ERR_PRIVSEP_LEN);
			break;
		}

		*outbuf = malloc(*size);
		if (*outbuf == NULL) {
			err = got_error_from_errno("malloc");
			break;
		}
		memcpy(*outbuf, imsg.data + sizeof(*iblob), *size);
		break;
	default:
		err = got_error(GOT_ERR_PRIVSEP_MSG);
		break;
	}

	imsg_free(&imsg);

	return err;
}

static const struct got_error *
send_tagmsg(struct imsgbuf *ibuf, struct got_tag_object *tag, size_t tagmsg_len)
{
	const struct got_error *err = NULL;
	size_t offset, remain;

	offset = 0;
	remain = tagmsg_len;
	while (remain > 0) {
		size_t n = MIN(MAX_IMSGSIZE - IMSG_HEADER_SIZE, remain);

		if (imsg_compose(ibuf, GOT_IMSG_TAG_TAGMSG, 0, 0, -1,
		    tag->tagmsg + offset, n) == -1) {
			err = got_error_from_errno("imsg_compose TAG_TAGMSG");
			break;
		}

		err = flush_imsg(ibuf);
		if (err)
			break;

		offset += n;
		remain -= n;
	}

	return err;
}

const struct got_error *
got_privsep_send_tag(struct imsgbuf *ibuf, struct got_tag_object *tag)
{
	const struct got_error *err = NULL;
	struct got_imsg_tag_object *itag;
	uint8_t *buf;
	size_t len, total;
	size_t tag_len = strlen(tag->tag);
	size_t tagger_len = strlen(tag->tagger);
	size_t tagmsg_len = strlen(tag->tagmsg);

	total = sizeof(*itag) + tag_len + tagger_len + tagmsg_len;

	buf = malloc(total);
	if (buf == NULL)
		return got_error_from_errno("malloc");

	itag = (struct got_imsg_tag_object *)buf;
	memcpy(itag->id, tag->id.sha1, sizeof(itag->id));
	itag->obj_type = tag->obj_type;
	itag->tag_len = tag_len;
	itag->tagger_len = tagger_len;
	itag->tagger_time = tag->tagger_time;
	itag->tagger_gmtoff = tag->tagger_gmtoff;
	itag->tagmsg_len = tagmsg_len;

	len = sizeof(*itag);
	memcpy(buf + len, tag->tag, tag_len);
	len += tag_len;
	memcpy(buf + len, tag->tagger, tagger_len);
	len += tagger_len;

	if (imsg_compose(ibuf, GOT_IMSG_TAG, 0, 0, -1, buf, len) == -1) {
		err = got_error_from_errno("imsg_compose TAG");
		goto done;
	}

	if (tagmsg_len == 0 ||
	    tagmsg_len + len > MAX_IMSGSIZE - IMSG_HEADER_SIZE) {
		err = flush_imsg(ibuf);
		if (err)
			goto done;
	}
	err = send_tagmsg(ibuf, tag, tagmsg_len);
done:
	free(buf);
	return err;
}

const struct got_error *
got_privsep_recv_tag(struct got_tag_object **tag, struct imsgbuf *ibuf)
{
	const struct got_error *err = NULL;
	struct imsg imsg;
	struct got_imsg_tag_object *itag;
	size_t len, datalen;
	const size_t min_datalen =
	    MIN(sizeof(struct got_imsg_error),
	    sizeof(struct got_imsg_tag_object));

	*tag = NULL;

	err = got_privsep_recv_imsg(&imsg, ibuf, min_datalen);
	if (err)
		return err;

	datalen = imsg.hdr.len - IMSG_HEADER_SIZE;
	len = 0;

	switch (imsg.hdr.type) {
	case GOT_IMSG_TAG:
		if (datalen < sizeof(*itag)) {
			err = got_error(GOT_ERR_PRIVSEP_LEN);
			break;
		}
		itag = imsg.data;
		if (datalen != sizeof(*itag) + itag->tag_len +
		    itag->tagger_len) {
			err = got_error(GOT_ERR_PRIVSEP_LEN);
			break;
		}
		len += sizeof(*itag);

		*tag = calloc(1, sizeof(**tag));
		if (*tag == NULL) {
			err = got_error_from_errno("calloc");
			break;
		}

		memcpy((*tag)->id.sha1, itag->id, SHA1_DIGEST_LENGTH);

		if (itag->tag_len == 0) {
			(*tag)->tag = strdup("");
			if ((*tag)->tag == NULL) {
				err = got_error_from_errno("strdup");
				break;
			}
		} else {
			(*tag)->tag = malloc(itag->tag_len + 1);
			if ((*tag)->tag == NULL) {
				err = got_error_from_errno("malloc");
				break;
			}
			memcpy((*tag)->tag, imsg.data + len,
			    itag->tag_len);
			(*tag)->tag[itag->tag_len] = '\0';
		}
		len += itag->tag_len;

		(*tag)->obj_type = itag->obj_type;
		(*tag)->tagger_time = itag->tagger_time;
		(*tag)->tagger_gmtoff = itag->tagger_gmtoff;

		if (itag->tagger_len == 0) {
			(*tag)->tagger = strdup("");
			if ((*tag)->tagger == NULL) {
				err = got_error_from_errno("strdup");
				break;
			}
		} else {
			(*tag)->tagger = malloc(itag->tagger_len + 1);
			if ((*tag)->tagger == NULL) {
				err = got_error_from_errno("malloc");
				break;
			}
			memcpy((*tag)->tagger, imsg.data + len,
			    itag->tagger_len);
			(*tag)->tagger[itag->tagger_len] = '\0';
		}
		len += itag->tagger_len;

		if (itag->tagmsg_len == 0) {
			(*tag)->tagmsg = strdup("");
			if ((*tag)->tagmsg == NULL) {
				err = got_error_from_errno("strdup");
				break;
			}
		} else {
			size_t offset = 0, remain = itag->tagmsg_len;

			(*tag)->tagmsg = malloc(itag->tagmsg_len + 1);
			if ((*tag)->tagmsg == NULL) {
				err = got_error_from_errno("malloc");
				break;
			}
			while (remain > 0) {
				struct imsg imsg_log;
				size_t n = MIN(MAX_IMSGSIZE - IMSG_HEADER_SIZE,
				    remain);

				err = got_privsep_recv_imsg(&imsg_log, ibuf, n);
				if (err)
					return err;

				if (imsg_log.hdr.type != GOT_IMSG_TAG_TAGMSG)
					return got_error(GOT_ERR_PRIVSEP_MSG);

				memcpy((*tag)->tagmsg + offset, imsg_log.data,
				    n);
				imsg_free(&imsg_log);
				offset += n;
				remain -= n;
			}
			(*tag)->tagmsg[itag->tagmsg_len] = '\0';
		}

		break;
	default:
		err = got_error(GOT_ERR_PRIVSEP_MSG);
		break;
	}

	imsg_free(&imsg);

	return err;
}

const struct got_error *
got_privsep_init_pack_child(struct imsgbuf *ibuf, struct got_pack *pack,
    struct got_packidx *packidx)
{
	const struct got_error *err = NULL;
	struct got_imsg_packidx ipackidx;
	struct got_imsg_pack ipack;
	int fd;

	ipackidx.len = packidx->len;
	fd = dup(packidx->fd);
	if (fd == -1)
		return got_error_from_errno("dup");

	if (imsg_compose(ibuf, GOT_IMSG_PACKIDX, 0, 0, fd, &ipackidx,
	    sizeof(ipackidx)) == -1) {
		err = got_error_from_errno("imsg_compose PACKIDX");
		close(fd);
		return err;
	}

	if (strlcpy(ipack.path_packfile, pack->path_packfile,
	    sizeof(ipack.path_packfile)) >= sizeof(ipack.path_packfile))
		return got_error(GOT_ERR_NO_SPACE);
	ipack.filesize = pack->filesize;

	fd = dup(pack->fd);
	if (fd == -1)
		return got_error_from_errno("dup");

	if (imsg_compose(ibuf, GOT_IMSG_PACK, 0, 0, fd, &ipack, sizeof(ipack))
	    == -1) {
		err = got_error_from_errno("imsg_compose PACK");
		close(fd);
		return err;
	}

	return flush_imsg(ibuf);
}

const struct got_error *
got_privsep_send_packed_obj_req(struct imsgbuf *ibuf, int idx,
    struct got_object_id *id)
{
	struct got_imsg_packed_object iobj;

	iobj.idx = idx;
	memcpy(iobj.id, id->sha1, sizeof(iobj.id));

	if (imsg_compose(ibuf, GOT_IMSG_PACKED_OBJECT_REQUEST, 0, 0, -1,
	    &iobj, sizeof(iobj)) == -1)
		return got_error_from_errno("imsg_compose "
		    "PACKED_OBJECT_REQUEST");

	return flush_imsg(ibuf);
}

const struct got_error *
got_privsep_send_gitconfig_parse_req(struct imsgbuf *ibuf, int fd)
{
	const struct got_error *err = NULL;

	if (imsg_compose(ibuf, GOT_IMSG_GITCONFIG_PARSE_REQUEST, 0, 0, fd,
	    NULL, 0) == -1) {
		err = got_error_from_errno("imsg_compose "
		    "GITCONFIG_PARSE_REQUEST");
		close(fd);
		return err;
	}

	return flush_imsg(ibuf);
}

const struct got_error *
got_privsep_send_gitconfig_repository_format_version_req(struct imsgbuf *ibuf)
{
	if (imsg_compose(ibuf,
	    GOT_IMSG_GITCONFIG_REPOSITORY_FORMAT_VERSION_REQUEST, 0, 0, -1,
	    NULL, 0) == -1)
		return got_error_from_errno("imsg_compose "
		    "GITCONFIG_REPOSITORY_FORMAT_VERSION_REQUEST");

	return flush_imsg(ibuf);
}

const struct got_error *
got_privsep_send_gitconfig_author_name_req(struct imsgbuf *ibuf)
{
	if (imsg_compose(ibuf,
	    GOT_IMSG_GITCONFIG_AUTHOR_NAME_REQUEST, 0, 0, -1, NULL, 0) == -1)
		return got_error_from_errno("imsg_compose "
		    "GITCONFIG_AUTHOR_NAME_REQUEST");

	return flush_imsg(ibuf);
}

const struct got_error *
got_privsep_send_gitconfig_author_email_req(struct imsgbuf *ibuf)
{
	if (imsg_compose(ibuf,
	    GOT_IMSG_GITCONFIG_AUTHOR_EMAIL_REQUEST, 0, 0, -1, NULL, 0) == -1)
		return got_error_from_errno("imsg_compose "
		    "GITCONFIG_AUTHOR_EMAIL_REQUEST");

	return flush_imsg(ibuf);
}

const struct got_error *
got_privsep_send_gitconfig_remotes_req(struct imsgbuf *ibuf)
{
	if (imsg_compose(ibuf,
	    GOT_IMSG_GITCONFIG_REMOTES_REQUEST, 0, 0, -1, NULL, 0) == -1)
		return got_error_from_errno("imsg_compose "
		    "GITCONFIG_REMOTE_REQUEST");

	return flush_imsg(ibuf);
}

const struct got_error *
got_privsep_send_gitconfig_str(struct imsgbuf *ibuf, const char *value)
{
	size_t len = value ? strlen(value) + 1 : 0;

	if (imsg_compose(ibuf, GOT_IMSG_GITCONFIG_STR_VAL, 0, 0, -1,
	    value, len) == -1)
		return got_error_from_errno("imsg_compose GITCONFIG_STR_VAL");

	return flush_imsg(ibuf);
}

const struct got_error *
got_privsep_recv_gitconfig_str(char **str, struct imsgbuf *ibuf)
{
	const struct got_error *err = NULL;
	struct imsg imsg;
	size_t datalen;
	const size_t min_datalen = 0;

	*str = NULL;

	err = got_privsep_recv_imsg(&imsg, ibuf, min_datalen);
	if (err)
		return err;
	datalen = imsg.hdr.len - IMSG_HEADER_SIZE;

	switch (imsg.hdr.type) {
	case GOT_IMSG_GITCONFIG_STR_VAL:
		if (datalen == 0)
			break;
		*str = malloc(datalen);
		if (*str == NULL) {
			err = got_error_from_errno("malloc");
			break;
		}
		if (strlcpy(*str, imsg.data, datalen) >= datalen)
			err = got_error(GOT_ERR_NO_SPACE);
		break;
	default:
		err = got_error(GOT_ERR_PRIVSEP_MSG);
		break;
	}

	imsg_free(&imsg);
	return err;
}

const struct got_error *
got_privsep_send_gitconfig_int(struct imsgbuf *ibuf, int value)
{
	if (imsg_compose(ibuf, GOT_IMSG_GITCONFIG_INT_VAL, 0, 0, -1,
	    &value, sizeof(value)) == -1)
		return got_error_from_errno("imsg_compose GITCONFIG_INT_VAL");

	return flush_imsg(ibuf);
}

const struct got_error *
got_privsep_recv_gitconfig_int(int *val, struct imsgbuf *ibuf)
{
	const struct got_error *err = NULL;
	struct imsg imsg;
	size_t datalen;
	const size_t min_datalen =
	    MIN(sizeof(struct got_imsg_error), sizeof(int));

	*val = 0;

	err = got_privsep_recv_imsg(&imsg, ibuf, min_datalen);
	if (err)
		return err;
	datalen = imsg.hdr.len - IMSG_HEADER_SIZE;

	switch (imsg.hdr.type) {
	case GOT_IMSG_GITCONFIG_INT_VAL:
		if (datalen != sizeof(*val)) {
			err = got_error(GOT_ERR_PRIVSEP_LEN);
			break;
		}
		memcpy(val, imsg.data, sizeof(*val));
		break;
	default:
		err = got_error(GOT_ERR_PRIVSEP_MSG);
		break;
	}

	imsg_free(&imsg);
	return err;
}

const struct got_error *
got_privsep_send_gitconfig_remotes(struct imsgbuf *ibuf,
    struct got_remote_repo *remotes, int nremotes)
{
	const struct got_error *err = NULL;
	struct got_imsg_remotes iremotes;
	int i;

	iremotes.nremotes = nremotes;
	if (imsg_compose(ibuf, GOT_IMSG_GITCONFIG_REMOTES, 0, 0, -1,
	    &iremotes, sizeof(iremotes)) == -1)
		return got_error_from_errno("imsg_compose GITCONFIG_REMOTES");

	err = flush_imsg(ibuf);
	imsg_clear(ibuf);
	if (err)
		return err;

	for (i = 0; i < nremotes; i++) {
		struct got_imsg_remote iremote;
		size_t len = sizeof(iremote);
		struct ibuf *wbuf;

		iremote.name_len = strlen(remotes[i].name);
		len += iremote.name_len;
		iremote.url_len = strlen(remotes[i].url);
		len += iremote.url_len;

		wbuf = imsg_create(ibuf, GOT_IMSG_GITCONFIG_REMOTE, 0, 0, len);
		if (wbuf == NULL)
			return got_error_from_errno(
			    "imsg_create GITCONFIG_REMOTE");

		if (imsg_add(wbuf, &iremote, sizeof(iremote)) == -1) {
			err = got_error_from_errno(
			    "imsg_add GIITCONFIG_REMOTE");
			ibuf_free(wbuf);
			return err;
		}

		if (imsg_add(wbuf, remotes[i].name, iremote.name_len) == -1) {
			err = got_error_from_errno(
			    "imsg_add GIITCONFIG_REMOTE");
			ibuf_free(wbuf);
			return err;
		}
		if (imsg_add(wbuf, remotes[i].url, iremote.url_len) == -1) {
			err = got_error_from_errno(
			    "imsg_add GIITCONFIG_REMOTE");
			ibuf_free(wbuf);
			return err;
		}

		wbuf->fd = -1;
		imsg_close(ibuf, wbuf);
		err = flush_imsg(ibuf);
		if (err)
			return err;
	}

	return NULL;
}

const struct got_error *
got_privsep_recv_gitconfig_remotes(struct got_remote_repo **remotes,
    int *nremotes, struct imsgbuf *ibuf)
{
	const struct got_error *err = NULL;
	struct imsg imsg;
	size_t datalen;
	struct got_imsg_remotes iremotes;
	struct got_imsg_remote iremote;

	*remotes = NULL;
	*nremotes = 0;

	err = got_privsep_recv_imsg(&imsg, ibuf, sizeof(iremotes));
	if (err)
		return err;
	datalen = imsg.hdr.len - IMSG_HEADER_SIZE;

	switch (imsg.hdr.type) {
	case GOT_IMSG_GITCONFIG_REMOTES:
		if (datalen != sizeof(iremotes)) {
			err = got_error(GOT_ERR_PRIVSEP_LEN);
			break;
		}
		memcpy(&iremotes, imsg.data, sizeof(iremotes));
		if (iremotes.nremotes == 0) {
			imsg_free(&imsg);
			return NULL;
		}
		break;
	default:
		err = got_error(GOT_ERR_PRIVSEP_MSG);
		break;
	}

	imsg_free(&imsg);

	*remotes = recallocarray(NULL, 0, iremotes.nremotes, sizeof(iremote));
	if (*remotes == NULL)
		return got_error_from_errno("recallocarray");

	while (*nremotes < iremotes.nremotes) {
		struct got_remote_repo *remote;
	
		err = got_privsep_recv_imsg(&imsg, ibuf, sizeof(iremote));
		if (err)
			break;
		datalen = imsg.hdr.len - IMSG_HEADER_SIZE;

		switch (imsg.hdr.type) {
		case GOT_IMSG_GITCONFIG_REMOTE:
			remote = &(*remotes)[*nremotes];
			if (datalen < sizeof(iremote)) {
				err = got_error(GOT_ERR_PRIVSEP_LEN);
				break;
			}
			memcpy(&iremote, imsg.data, sizeof(iremote));
			if (iremote.name_len == 0 || iremote.url_len == 0 ||
			    (sizeof(iremote) + iremote.name_len +
			    iremote.url_len) > datalen) {
				err = got_error(GOT_ERR_PRIVSEP_LEN);
				break;
			}
			remote->name = strndup(imsg.data + sizeof(iremote),
			    iremote.name_len);
			if (remote->name == NULL) {
				err = got_error_from_errno("strndup");
				break;
			}
			remote->url = strndup(imsg.data + sizeof(iremote) +
			    iremote.name_len, iremote.url_len);
			if (remote->url == NULL) {
				err = got_error_from_errno("strndup");
				free(remote->name);
				break;
			}
			(*nremotes)++;
			break;
		default:
			err = got_error(GOT_ERR_PRIVSEP_MSG);
			break;
		}

		imsg_free(&imsg);
		if (err)
			break;
	}

	if (err) {
		int i;
		for (i = 0; i < *nremotes; i++) {
			free((*remotes)[i].name);
			free((*remotes)[i].url);
		}
		free(*remotes);
		*remotes = NULL;
		*nremotes = 0;
	}
	return err;
}

const struct got_error *
got_privsep_unveil_exec_helpers(void)
{
	const char *helpers[] = {
	    GOT_PATH_PROG_READ_PACK,
	    GOT_PATH_PROG_READ_OBJECT,
	    GOT_PATH_PROG_READ_COMMIT,
	    GOT_PATH_PROG_READ_TREE,
	    GOT_PATH_PROG_READ_BLOB,
	    GOT_PATH_PROG_READ_TAG,
	    GOT_PATH_PROG_READ_GITCONFIG,
	};
	int i;

	for (i = 0; i < nitems(helpers); i++) {
		if (unveil(helpers[i], "x") == 0)
			continue;
		return got_error_from_errno2("unveil", helpers[i]);
	}

	return NULL;
}

void
got_privsep_exec_child(int imsg_fds[2], const char *path, const char *repo_path)
{
	if (close(imsg_fds[0]) != 0) {
		fprintf(stderr, "%s: %s\n", getprogname(), strerror(errno));
		_exit(1);
	}

	if (dup2(imsg_fds[1], GOT_IMSG_FD_CHILD) == -1) {
		fprintf(stderr, "%s: %s\n", getprogname(), strerror(errno));
		_exit(1);
	}
	if (closefrom(GOT_IMSG_FD_CHILD + 1) == -1) {
		fprintf(stderr, "%s: %s\n", getprogname(), strerror(errno));
		_exit(1);
	}

	if (execl(path, path, repo_path, (char *)NULL) == -1) {
		fprintf(stderr, "%s: %s: %s\n", getprogname(), path,
		    strerror(errno));
		_exit(1);
	}
}
