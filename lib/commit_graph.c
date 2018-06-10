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

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/queue.h>
#include <sys/stdint.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sha1.h>
#include <zlib.h>
#include <ctype.h>

#include "got_error.h"
#include "got_object.h"
#include "got_commit_graph.h"

#include "got_lib_delta.h"
#include "got_lib_zbuf.h"
#include "got_lib_object.h"
#include "got_lib_object_idset.h"

struct got_commit_graph_node {
	struct got_object_id id;

	/*
	 * Each graph node corresponds to a commit object.
	 * Graph vertices are modelled with two separate adjacency lists:
	 * Adjacencies of a graph node are either parent (older) commits,
	 * and child (younger) commits.
	 */
	struct got_commit_object *commit; /* contains list of parents */
	int nchildren;
	SIMPLEQ_HEAD(, got_parent_id) child_ids;

	/* Used during graph iteration. */
	TAILQ_ENTRY(got_commit_graph_node) entry;
};

struct got_commit_graph {
	/* The set of all commits we have traversed. */
	struct got_object_idset *node_ids;

	/* The commit at which traversal began (youngest commit in node_ids). */
	struct got_commit_graph_node *head_node;

	/*
	 * A set of object IDs of known parent commits which we have not yet
	 * traversed. Each commit ID in this set represents a branch in commit
	 * history: Either the first-parent branch of the head node, or another
	 * branch corresponding to a traversed merge commit for which we have
	 * not traversed a branch point commit yet.
	 *
	 * Whenever we add a commit with a matching ID to the graph, we remove
	 * its corresponding element from this set, and add new elements for
	 * each of that commit's parent commits which were not traversed yet.
	 *
	 * When API users ask us to fetch more commits, we fetch commits from
	 * all currently open branches. This allows API users to process
	 * commits in linear order even though the history contains branches.
	 */
	struct got_object_idset *open_branches;

	/* The next commit to return when the API user asks for one. */
	struct got_commit_graph_node *iter_node;

	TAILQ_HEAD(, got_commit_graph_node) iter_candidates;
};

static struct got_commit_graph *
alloc_graph(void)
{
	struct got_commit_graph *graph;

	graph = calloc(1, sizeof(*graph));
	if (graph == NULL)
		return NULL;

	graph->node_ids = got_object_idset_alloc();
	if (graph->node_ids == NULL) {
		free(graph);
		return NULL;
	}

	graph->open_branches = got_object_idset_alloc();
	if (graph->open_branches == NULL) {
		got_object_idset_free(graph->node_ids);
		free(graph);
		return NULL;
	}

	TAILQ_INIT(&graph->iter_candidates);
	return graph;
}

#if 0
static int
is_head_node(struct got_commit_graph_node *node)
{
	return node->nchildren == 0;
}

static int
is_merge_point(struct got_commit_graph_node *node)
{
	return node->commit->nparents > 1;
}

int
is_branch_point(struct got_commit_graph_node *node)
{
	return node->nchildren > 1;
}
#endif

static int
is_root_node(struct got_commit_graph_node *node)
{
	return node->commit->nparents == 0;
}

static const struct got_error *
parse_commit_time(int64_t *time, struct got_commit_object *commit)
{
	const struct got_error *err = NULL;
	const char *errstr;
	char *committer, *space;

	*time = 0;

	committer = strdup(commit->committer);
	if (committer == NULL)
		return got_error_from_errno();

	/* Strip off trailing timezone indicator. */
	space = strrchr(committer, ' ');
	if (space == NULL) {
		err = got_error(GOT_ERR_BAD_OBJ_DATA);
		goto done;
	}
	*space = '\0';

	/* Timestamp is separated from committer name + email by space. */
	space = strrchr(committer, ' ');
	if (space == NULL) {
		err = got_error(GOT_ERR_BAD_OBJ_DATA);
		goto done;
	}

	*time = strtonum(space + 1, 0, INT64_MAX, &errstr);
	if (errstr)
		err = got_error(GOT_ERR_BAD_OBJ_DATA);

done:
	free(committer);
	return err;
}

static const struct got_error *
compare_commits(int *cmp, struct got_commit_object *c1,
    struct got_commit_object *c2)
{
	const struct got_error *err;
	int64_t t1, t2;

	err = parse_commit_time(&t1, c1);
	if (err)
		return err;
	err = parse_commit_time(&t2, c2);
	if (err)
		return err;

	if (t1 == t2)	
		*cmp = 0;
	else if (t1 < t2)
		*cmp = -1;
	else
		*cmp = 1;

	return NULL;
}

static const struct got_error *
add_iteration_candidate(struct got_commit_graph *graph,
    struct got_commit_graph_node *node)
{
	struct got_commit_graph_node *n;
	
	if (TAILQ_EMPTY(&graph->iter_candidates)) {
		TAILQ_INSERT_TAIL(&graph->iter_candidates, node, entry);
		return NULL;
	}

	TAILQ_FOREACH(n, &graph->iter_candidates, entry) {
		const struct got_error *err;
		int cmp;
		err = compare_commits(&cmp, node->commit, n->commit);
		if (err)
			return err;
		if (cmp < 0)
			continue;
		TAILQ_INSERT_BEFORE(n, node, entry);
		break;
	}

	return NULL;
}

static const struct got_error *
add_node(struct got_commit_graph_node **new_node,
    struct got_commit_graph *graph, struct got_object_id *commit_id,
    struct got_commit_object *commit, struct got_object_id *child_commit_id)
{
	const struct got_error *err = NULL;
	struct got_commit_graph_node *node, *existing_node;

	*new_node = NULL;

	node = calloc(1, sizeof(*node));
	if (node == NULL)
		return got_error_from_errno();

	memcpy(&node->id, commit_id, sizeof(node->id));
	node->commit = commit;
	SIMPLEQ_INIT(&node->child_ids);

	err = got_object_idset_add((void **)(&existing_node),
	    graph->node_ids, &node->id, node);
	if (err == NULL) {
		struct got_parent_id *pid;

		err = add_iteration_candidate(graph, node);
		if (err)
			return err;

		err = got_object_idset_remove(graph->open_branches, commit_id);
		if (err && err->code != GOT_ERR_NO_OBJ)
			return err;
		SIMPLEQ_FOREACH(pid, &commit->parent_ids, entry) {
			if (got_object_idset_get(graph->node_ids, pid->id))
				continue; /* parent already traversed */
			err = got_object_idset_add(NULL, graph->open_branches,
			    pid->id, node);
			if (err && err->code != GOT_ERR_OBJ_EXISTS)
				return err;
		}
		*new_node = node;
	} else if (err->code == GOT_ERR_OBJ_EXISTS) {
		err = NULL;
		free(node);
		node = existing_node;
	} else {
		free(node);
		return err;
	}

	if (child_commit_id) {
		struct got_parent_id *child, *cid;

		/* Prevent linking to self. */
		if (got_object_id_cmp(commit_id, child_commit_id) == 0)
			return got_error(GOT_ERR_BAD_OBJ_ID);

		/* Prevent double-linking to the same child. */
		SIMPLEQ_FOREACH(cid, &node->child_ids, entry) {
			if (got_object_id_cmp(cid->id, child_commit_id) == 0)
				return got_error(GOT_ERR_BAD_OBJ_ID);
		}

		child = calloc(1, sizeof(*child));
		if (child == NULL)
			return got_error_from_errno();
		child->id = got_object_id_dup(child_commit_id);
		if (child->id == NULL) {
			err = got_error_from_errno();
			free(child);
			return err;
		}
		SIMPLEQ_INSERT_TAIL(&node->child_ids, child, entry);
		node->nchildren++;
	}

	return err;
}

const struct got_error *
got_commit_graph_open(struct got_commit_graph **graph,
    struct got_object_id *commit_id, struct got_repository *repo)
{
	const struct got_error *err = NULL;
	struct got_object *obj;
	struct got_commit_object *commit;

	*graph = NULL;

	err = got_object_open(&obj, repo, commit_id);
	if (err)
		return err;
	if (got_object_get_type(obj) != GOT_OBJ_TYPE_COMMIT) {
		err = got_error(GOT_ERR_OBJ_TYPE);
		got_object_close(obj);
		return err;
	}

	err = got_object_commit_open(&commit, repo, obj);
	got_object_close(obj);
	if (err)
		return err;

	*graph = alloc_graph();
	if (*graph == NULL) {
		got_object_commit_close(commit);
		return got_error_from_errno();
	}

	err = add_node(&(*graph)->head_node, *graph, commit_id, commit, NULL);
	if (err) {
		got_commit_graph_close(*graph);
		*graph = NULL;
		return err;
	}
	
	return NULL;
}

static const struct got_error *
open_commit(struct got_commit_object **commit, struct got_object_id *id,
    struct got_repository *repo)
{
	const struct got_error *err;
	struct got_object *obj;

	err = got_object_open(&obj, repo, id);
	if (err)
		return err;
	if (got_object_get_type(obj) != GOT_OBJ_TYPE_COMMIT) {
		err = got_error(GOT_ERR_OBJ_TYPE);
		goto done;
	}

	err = got_object_commit_open(commit, repo, obj);
done:
	got_object_close(obj);
	return err;
}

struct got_commit_graph_branch {
	struct got_object_id parent_id;
	struct got_commit_graph_node *node;
};

struct gather_branches_arg {
	struct got_commit_graph_branch *branches;
	int nbranches;
};

static void
gather_branches(struct got_object_id *id, void *data, void *arg)
{
	struct gather_branches_arg *a = arg;
	memcpy(&a->branches[a->nbranches].parent_id, id, sizeof(*id));
	a->branches[a->nbranches].node = data;
	a->nbranches++;
}

const struct got_error *
fetch_commits_from_open_branches(int *ncommits,
    struct got_commit_graph *graph, struct got_repository *repo)
{
	const struct got_error *err;
	struct got_commit_graph_branch *branches;
	struct gather_branches_arg arg;
	int i;

	*ncommits = 0;

	arg.nbranches = got_object_idset_num_elements(graph->open_branches);
	if (arg.nbranches == 0)
		return NULL;

	/*
	 * Adding nodes to the graph might change the graph's open
	 * branches state. Create a local copy of the current state.
	 */
	branches = calloc(arg.nbranches, sizeof(*branches));
	if (branches == NULL)
		return got_error_from_errno();
	arg.nbranches = 0; /* reset; gather_branches() will increment */
	arg.branches = branches;
	got_object_idset_for_each(graph->open_branches, gather_branches, &arg);

	for (i = 0; i < arg.nbranches; i++) {
		struct got_object_id *commit_id;
		struct got_commit_graph_node *child_node, *new_node;
		struct got_commit_object *commit;

		commit_id = &branches[i].parent_id;
		child_node = branches[i].node;

		err = open_commit(&commit, commit_id, repo);
		if (err)
			break;

		err = add_node(&new_node, graph, commit_id, commit,
		    &child_node->id);
		if (err) {
			if (err->code != GOT_ERR_OBJ_EXISTS)
				break;
			err = NULL;
		}
		if (new_node)
			(*ncommits)++;
		else
			got_object_commit_close(commit);
	}

	free(branches);
	return err;
}

const struct got_error *
got_commit_graph_fetch_commits(int *nfetched, struct got_commit_graph *graph,
    int limit, struct got_repository *repo)
{
	const struct got_error *err;
	int total = 0, ncommits;

	*nfetched = 0;

	while (total < limit) {
		err = fetch_commits_from_open_branches(&ncommits, graph, repo);
		if (err)
			return err;
		if (ncommits == 0)
			break;
		total += ncommits;
	}

	*nfetched = total;
	return NULL;
}

static void
free_graph_node(struct got_object_id *id, void *data, void *arg)
{
	struct got_commit_graph_node *node = data;
	got_object_commit_close(node->commit);
	while (!SIMPLEQ_EMPTY(&node->child_ids)) {
		struct got_parent_id *child = SIMPLEQ_FIRST(&node->child_ids);
		SIMPLEQ_REMOVE_HEAD(&node->child_ids, entry);
		free(child);
	}
	free(node);
}

void
got_commit_graph_close(struct got_commit_graph *graph)
{
	got_object_idset_free(graph->open_branches);
	got_object_idset_for_each(graph->node_ids, free_graph_node, NULL);
	got_object_idset_free(graph->node_ids);
	free(graph);
}

const struct got_error *
got_commit_graph_iter_start(struct got_commit_graph *graph,
    struct got_object_id *id)
{
	struct got_commit_graph_node *start_node, *node;
	struct got_parent_id *pid;

	start_node = got_object_idset_get(graph->node_ids, id);
	if (start_node == NULL)
		return got_error(GOT_ERR_NO_OBJ);

	graph->iter_node = start_node;

	while (!TAILQ_EMPTY(&graph->iter_candidates)) {
		node = TAILQ_FIRST(&graph->iter_candidates);
		TAILQ_REMOVE(&graph->iter_candidates, node, entry);
	}

	/* Put all known parents of this commit on the candidate list. */
	SIMPLEQ_FOREACH(pid, &start_node->commit->parent_ids, entry) {
		node = got_object_idset_get(graph->node_ids, pid->id);
		if (node) {
			const struct got_error *err;
			err = add_iteration_candidate(graph, node);
			if (err)
				return err;
		}
	}

	return NULL;
}

const struct got_error *
got_commit_graph_iter_next(struct got_commit_object **commit,
    struct got_object_id **id, struct got_commit_graph *graph)
{
	struct got_commit_graph_node *node;

	if (graph->iter_node == NULL) {
		/* We are done interating, or iteration was not started. */
		*commit = NULL;
		*id = NULL;
		return NULL;
	}

	if (TAILQ_EMPTY(&graph->iter_candidates)) {
		if (is_root_node(graph->iter_node) &&
		    got_object_idset_num_elements(graph->open_branches) == 0) {
			*commit = graph->iter_node->commit;
			*id = &graph->iter_node->id;
			/* We are done interating. */
			graph->iter_node = NULL;
			return NULL;
		}
		return got_error(GOT_ERR_ITER_NEED_MORE);
	}

	*commit = graph->iter_node->commit;
	*id = &graph->iter_node->id;
	node = TAILQ_FIRST(&graph->iter_candidates);
	TAILQ_REMOVE(&graph->iter_candidates, node, entry);
	graph->iter_node = node;
	return NULL;
}

const struct got_commit_object *
got_commit_graph_get_commit(struct got_commit_graph *graph,
    struct got_object_id *id)
{
	struct got_commit_graph_node *node;
	node = got_object_idset_get(graph->node_ids, id);
	return node ? node->commit : NULL;
}