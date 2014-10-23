/*
 *  Copyright © 2006 Keith Packard <keithp@keithp.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or (at
 *  your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA.
 */
#define _XOPEN_SOURCE 700
#define _BSD_SOURCE

#include <limits.h>
#include <assert.h>
#include <stdlib.h>
#include <sys/types.h>
#include <ftw.h>
#include <time.h>

/*
 * Blob compression with zlib is not enabled by default because, (a) in general,
 * any repository large enough to hit a disk-space limit is likely to hit
 * a core limit on metadata sooner, and (b) compression costs time.  The
 * option has been left in place for unusual circumstances and can be enabled
 * from the Makefile.
 */
#ifdef ZLIB
#include <zlib.h>
#endif

#include "cvs.h"

/*
 * If a program has ever invoked pthreads, the GNU C library does extra
 * checking during stdio operations even if the program no longer has
 * active subthreads.  Foil this with a GNU extension.  Doing this nearly
 * doubled throughput on the benchmark repositories.
 *
 * The unlocked_stdio(3) manual pages claim that fputs_unlocked) and
 * fclose_unlocked exist, but they don't actually seem to.
 */
#ifdef __GLIBC__
#define fread	fread_unlocked
#define fwrite	fwrite_unlocked
#define putchar	putchar_unlocked
#define fputc   fputc_unlocked
#define feof    feof_unlocked
#endif /* __GLIBC__ */

/*
 * Below this byte-volume threshold, default to canonical order.
 * Above it, default to fast.  Note that this is total volume of
 * the CVS masters - it would be better to use total snapshot volume
 * but we don't have that at the time the check is done. This threshold is
 * mainly present for backward compatibility and is somewhat arbitrary.
 */
#define SMALL_REPOSITORY	1000000

/*
 * This code is somewhat complex because the natural order of operations
 * generated by the file-traversal operations in the rest of the code is
 * not even remotely like the canonical order generated by git-fast-export.
 * We want to emulate the latter in order to make regression-testing and
 * comparisons with other tools as easy as possible.
 */

static serial_t *markmap;
static serial_t mark;
static volatile int seqno;
static char blobdir[PATH_MAX];

static export_stats_t export_stats;

static int seqno_next(void)
/* Returns next sequence number, starting with 1 */
{
    ++seqno;

    if (seqno >= MAX_SERIAL_T)
	fatal_error("snapshot sequence number too large, widen serial_t");

    return seqno;
}

/*
 * GNU CVS default ignores.  We omit from this things that CVS ignores
 * by default but which are highly unlikely to turn up outside an
 * actual CVS repository and should be conspicuous if they do: RCS
 * SCCS CVS CVS.adm RCSLOG cvslog.*
 */
#define CVS_IGNORES "# CVS default ignores begin\ntags\nTAGS\n.make.state\n.nse_depinfo\n*~\n#*\n.#*\n,*\n_$*\n*$\n*.old\n*.bak\n*.BAK\n*.orig\n*.rej\n.del-*\n*.a\n*.olb\n*.o\n*.obj\n*.so\n*.exe\n*.Z\n*.elc\n*.ln\ncore\n# CVS default ignores end\n"

static char *fileop_name(const char *rectified, char *path)
{
    size_t rlen = strlen(rectified);

    strncpy(path, rectified, PATH_MAX-1);

    if (rlen >= 10 && strcmp(path + rlen - 10, ".cvsignore") == 0) {
	path[rlen - 9] = 'g';
	path[rlen - 8] = 'i';
	path[rlen - 7] = 't';
    }

    return path;
}

static char *blobfile(const char *basename,
		      const int serial,
		      const bool create, char *path)
/* Random-access location of the blob corresponding to the specified serial */
{
    int m;

#ifdef FDEBUG
    (void)fprintf(stderr, "-> blobfile(%d, %d)...\n", serial, create);
#endif /* FDEBUG */
    (void)snprintf(path, PATH_MAX, "%s", blobdir);
    /*
     * FANOUT should be chosen to be the largest directory size that does not
     * cause slow secondary allocations.  It's something near 256 on ext4
     * (we think...)
     */
#define FANOUT	256
    for (m = serial;;)
    {
	int digit = m % FANOUT;
	if ((m = (m - digit) / FANOUT) == 0) {
	    (void)snprintf(path + strlen(path), PATH_MAX - strlen(path),
			   "/=%x", digit);
#ifdef FDEBUG
	    (void)fprintf(stderr, "path: %s\n", path);
#endif /* FDEBUG */
	    break;
	}
	else
	{
	    (void)snprintf(path + strlen(path), PATH_MAX - strlen(path),
			   "/%x", digit);
	    /* coverity[toctou] */
#ifdef FDEBUG
	    (void)fprintf(stderr, "directory: %s\n", path);
#endif /* FDEBUG */
	    if (create && access(path, R_OK) != 0) {
#ifdef FDEBUG
		(void)fprintf(stderr, "directory: %s\n", path);
#endif /* FDEBUG */
		errno = 0;
		if (mkdir(path,S_IRWXU | S_IRWXG) != 0 && errno != EEXIST)
		    fatal_error("blob subdir creation of %s failed: %s (%d)\n", path, strerror(errno), errno);
	    }
	}
    }
#undef FANOUT
#ifdef FDEBUG
    (void)fprintf(stderr, "<- ...returned path for %s %d = %s\n",
		  basename, serial, path);
#endif /* FDEBUG */
    return path;
}

static void export_blob(node_t *node, 
			void *buf, const size_t len,
			export_options_t *opts)
/* output the blob, or save where it will be available for random access */
{
    size_t extralen = 0;

    export_stats.snapsize += len;

    if (strcmp(node->commit->master->name, ".cvsignore") == 0) {
	extralen = sizeof(CVS_IGNORES) - 1;
    }

    node->commit->serial = seqno_next();
    if (opts->reportmode == fast) {
	markmap[node->commit->serial] = ++mark;
	printf("blob\nmark :%d\n", mark);
	fprintf(stdout, "data %zd\n", len + extralen);
	if (extralen > 0)
	    fwrite(CVS_IGNORES, extralen, sizeof(char), stdout);
	fwrite(buf, len, sizeof(char), stdout);
	fputc('\n', stdout);
    }
    else
    {
#ifdef ZLIB
	gzFile wfp;
#else
	FILE *wfp;
#endif
	char path[PATH_MAX];
	blobfile(node->commit->master->name, node->commit->serial, true, path);
#ifndef ZLIB
	wfp = fopen(path, "w");
#else
	/*
	 * Blobs are written compressed.  This costs a little compression time,
	 * but we get it back in reduced disk seeks.
	 */
	errno = 0;
	wfp = gzopen(path, "w");
#endif
	if (wfp == NULL)
	    fatal_error("blobfile open of %s: %s (%d)", 
			path, strerror(errno), errno);
#ifndef ZLIB
	fprintf(wfp, "data %zd\n", len + extralen);
	if (extralen > 0)
	    fwrite(CVS_IGNORES, extralen, sizeof(char), wfp);
	fwrite(buf, len, sizeof(char), wfp);
	fputc('\n', wfp);
	(void)fclose(wfp);
#else
	gzprintf(wfp, "data %zd\n", len + extralen);
	if (extralen > 0)
	    gzwrite(CVS_IGNORES, extralen, sizeof(char), wfp);
	gzwrite(wfp, buf, len);
	gzputc(wfp, '\n');
	(void)gzclose(wfp);
#endif
    }
}

static int unlink_cb(const char *fpath, 
		     const struct stat *sb, int typeflag, struct FTW *ftwbuf)
{
    int rv = remove(fpath);

    if (rv)
        perror(fpath);

    return rv;
}

static const char *utc_offset_timestamp(const time_t *timep, const char *tz)
{
    static char outbuf[BUFSIZ];
    struct tm *tm;
    char tzbuf[BUFSIZ];
    /* coverity[tainted_string_return_content] */
    char *oldtz = getenv("TZ");

    // make a copy in case original is clobbered
    if (oldtz != NULL)
	strncpy(tzbuf, oldtz, sizeof(tzbuf)-1);

    setenv("TZ", tz, 1);
    tzset();  // just in case ...

    tm = localtime(timep);
#ifndef __CYGWIN__
    strftime(outbuf, sizeof(outbuf), "%s %z", tm);
#else
		// Cygdwin doesn't have %s for strftime
    int x = sprintf(outbuf, "%li", *timep);
    strftime(outbuf + x, sizeof(outbuf) - x, " %z", tm);
#endif
    if (oldtz != NULL)
	setenv("TZ", tzbuf, 1);
    else
	unsetenv("TZ");
    tzset();

    return outbuf;
}

struct fileop {
    char op;
    mode_t mode;
    cvs_commit *rev;
    const char *path;
};

static int fileop_sort(const void *a, const void *b)
/* sort fileops as git fast-export does */
{
    /* As it says, 'Handle files below a directory first, in case they are
     * all deleted and the directory changes to a file or symlink.'
     * Because this doesn't have to handle renames, just sort lexicographically
     * We append a sentinel to make sure "a/b/c" < "a/b" < "a".
     */
    struct fileop *oa = (struct fileop *)a;
    struct fileop *ob = (struct fileop *)b;

    return path_deep_compare(oa->path, ob->path);
}

/*
 * The magic number 100000 avoids generating forced UTC times that might be
 * negative in some timezone, while producing a sequence easy to read.
 */
#define display_date(c, m, f)	(f ? (100000 + (m) * commit_time_window * 2) : ((c)->date + RCS_EPOCH))

/*
 * An iterator structure over the sorted files in a git_commit
 */
typedef struct _file_iter {
    rev_dir * const *dir;
    rev_dir * const *dirmax;
    cvs_commit **file;
    cvs_commit **filemax;
} file_iter;

static cvs_commit *
file_iter_next(file_iter *pos) {
    if (pos->dir == pos->dirmax)
        return NULL;
again:
    if (pos->file != pos->filemax)
	return *pos->file++;
    ++pos->dir;
    if (pos->dir == pos->dirmax)
        return NULL;
    pos->file = (*pos->dir)->files;
    pos->filemax = pos->file + (*pos->dir)->nfiles;
    goto again;
}

static void
file_iter_start(file_iter *pos, const git_commit *commit) {
    pos->dir = commit->dirs;
    pos->dirmax = commit->dirs + commit->ndirs;
    if (pos->dir != pos->dirmax) {
        pos->file = (*pos->dir)->files;
        pos->filemax = pos->file + (*pos->dir)->nfiles;
    } else {
        pos->file = pos->filemax = NULL;
    }
}

static void compute_parent_links(const git_commit *commit)
/* create reciprocal link pairs between file refs in a commit and its parent */
{
    const git_commit *parent = commit->parent;
    file_iter commit_iter, parent_iter;
    cvs_commit *cf, *pf;
    unsigned nparent, ncommit, maxmatch;

    ncommit = 0;
    file_iter_start(&commit_iter, commit);
    while ((cf = file_iter_next(&commit_iter))) {
	++ncommit;
        cf->other = NULL;
    }

    nparent = 0;
    file_iter_start(&parent_iter, parent);
    while ((pf = file_iter_next(&parent_iter))) {
	++nparent;
        pf->other = NULL;
    }

    maxmatch = (nparent < ncommit) ? nparent : ncommit;

    file_iter_start(&commit_iter, commit);
    file_iter_start(&parent_iter, parent);
    while ((cf = file_iter_next(&commit_iter))) {
	file_iter it;
	const bloom_t *bloom = atom_bloom(cf->master->name);
	unsigned k;

	for (k = 0; k < BLOOMLENGTH; ++k) {
	    if (bloom->el[k] & parent->bloom.el[k]) {
	        goto next;
	    }
	}

	/* Because the commit file lists are sorted,
	 * we can restart the iterator after the
	 * last successful match */
	it = parent_iter;
	while ((pf = file_iter_next(&it))) {
	    if (cf->master->name == pf->master->name) {
		cf->other = pf;
		pf->other = cf;
		if (--maxmatch == 0)
		    return;
		parent_iter = it;
		break;
	    }
	}

        next:;
    }
}

#ifdef ORDERDEBUG
static void dump_file(const cvs_commit *cvs_commit, FILE *fp)
{
    char buf[CVS_MAX_REV_LEN + 1];
    fprintf(fp, "   file name: %s %s\n", cvs_commit->master->name, 
	    cvs_number_string(&cvs_commit->number, buf, sizeof(buf)));
 }

static void dump_dir(const rev_dir *rev_dir, FILE *fp)
{
    int i;

    fprintf(fp, "   file count: %d\n", rev_dir->nfiles);
    for (i = 0; i < rev_dir->nfiles; i++)
	dump_file(rev_dir->files[i], fp);
}

static void dump_commit(const git_commit *commit, FILE *fp)
{
    int i;
    fprintf(fp, "commit %p seq %d mark %d nfiles: %d, ndirs = %d\n", 
	    commit, commit->serial, markmap[commit->serial], commit->nfiles, commit->ndirs);
    for (i = 0; i < commit->ndirs; i++)
	dump_dir(commit->dirs[i], fp);
}
#endif /* ORDERDEBUG */

static void export_commit(git_commit *commit,
			  const char *branch,
			  const bool report,
			  const export_options_t *opts)
/* export a commit(and the blobs it is the first to reference) */
{
#define OP_CHUNK	32
    cvs_author *author;
    const char *full;
    const char *email;
    const char *timezone;
    char *revpairs = NULL;
    size_t revpairsize = 0;
    time_t ct;
    cvs_commit	*cc;
    int		i, j;
    struct fileop *operations, *op, *op2;
    int noperations;
    serial_t here;
    static const char *s_gitignore;

    if (!s_gitignore) s_gitignore = atom(".gitignore");

    if (opts->reposurgeon || opts->revision_map != NULL || opts->embed_ids)
    {
	revpairs = xmalloc((revpairsize = 1024), "revpair allocation");
	revpairs[0] = '\0';
    }

    /*
     * Precompute mutual parent-child pointers.
     */
    if (commit->parent) 
	compute_parent_links(commit);

    noperations = OP_CHUNK;
    op = operations = xmalloc(sizeof(struct fileop) * noperations, "fileop allocation");
    for (i = 0; i < commit->ndirs; i++) {
	rev_dir	*dir = commit->dirs[i];
	
	for (j = 0; j < dir->nfiles; j++) {
	    char *stripped;
	    bool present, changed;
	    char converted[PATH_MAX];
	    op->rev = cc = dir->files[j];
	    stripped = fileop_name(cc->master->name, converted);
	    present = false;
	    changed = false;
	    if (commit->parent) {
		present = (cc->other != NULL);
		changed = present && (cc->serial != cc->other->serial);
	    }
	    if (!present || changed) {

		op->op = 'M';
		// git fast-import only supports 644 and 755 file modes
		if (cc->master->mode & 0100)
		    op->mode = 0755;
		else
		    op->mode = 0644;
		op->path = atom(stripped);
		op++;
		if (op == operations + noperations)
		{
		    noperations += OP_CHUNK;
		    operations = xrealloc(operations,
					  sizeof(struct fileop) * noperations, __func__);
		    // realloc can move operations
		    op = operations + noperations - OP_CHUNK;
		}

		if (opts->revision_map != NULL || opts->reposurgeon || opts->embed_ids) {
		    char fr[BUFSIZ];
		    int xtr = opts->embed_ids?10:2;
		    stringify_revision(cc->master->name, 
				  " ", &cc->number, fr, sizeof fr);
		    if (strlen(revpairs) + strlen(fr) + xtr > revpairsize)
		    {
			revpairsize *= 2;
			revpairs = xrealloc(revpairs, revpairsize, "revpair allocation");
		    }
		    if (opts->embed_ids)
			strcat(revpairs, "CVS-ID: ");
		    strcat(revpairs, fr);
		    strcat(revpairs, "\n");
		}
	    }
	}
    }

    if (commit->parent)
    {
	for (i = 0; i < commit->parent->ndirs; i++) {
	    rev_dir	*dir = commit->parent->dirs[i];

	    for (j = 0; j < dir->nfiles; j++) {
		bool present;
		cc = dir->files[j];
		present = (cc->other != NULL);
		if (!present) {
		    char converted[PATH_MAX];
		    op->op = 'D';
		    op->path = atom(fileop_name(cc->master->name, converted));
		    op++;
		    if (op == operations + noperations)
		    {
			noperations += OP_CHUNK;
			operations = xrealloc(operations,
					      sizeof(struct fileop) * noperations,
					      __func__);
			// realloc can move operations
			op = operations + noperations - OP_CHUNK;
		    }
		}
	    }
	}
    }

    for (op2 = operations; op2 < op; op2++)
    {
	if (op2->op == 'M' && !op2->rev->emitted)
	{
	    if (opts->reportmode == canonical)
		markmap[op2->rev->serial] = ++mark;
	    if (report && opts->reportmode == canonical) {
		char path[PATH_MAX];
		char *fn = blobfile(op2->path, op2->rev->serial, false, path);
#ifndef ZLIB
		FILE *rfp = fopen(fn, "r");
#else
		gzFile rfp = gzopen(fn, "r");
#endif
		if (rfp)
		{
#ifndef ZLIB
		    char buf[BUFSIZ];
#else
		    int c;
#endif
		    printf("blob\nmark :%d\n", mark);
#ifndef ZLIB
		    while (!feof(rfp)) {
			size_t len = fread(buf, 1, sizeof(buf), rfp);
			(void)fwrite(buf, 1, len, stdout);
		    }
#else
		    while ((c = gzgetc(rfp)) != EOF)
			putchar(c);
#endif
		    (void) unlink(fn);
		    op2->rev->emitted = true;
#ifndef ZLIB
		    (void)fclose(rfp);
#else
		    (void)gzclose(rfp);
#endif
		}
	    }
	}
    }

    /* sort operations into canonical order */
    qsort((void *)operations, op - operations, sizeof(struct fileop), fileop_sort); 

    author = fullname(commit->author);
    if (!author) {
	full = commit->author;
	email = commit->author;
	timezone = "UTC";
    } else {
	full = author->full;
	email = author->email;
	timezone = author->timezone ? author->timezone : "UTC";
    }

    if (report)
	printf("commit %s%s\n", opts->branch_prefix, branch);
    commit->serial = ++seqno;
    here = markmap[commit->serial] = ++mark;
#ifdef ORDERDEBUG2
    /* can't move before mark is updated */
    dump_commit(commit, stderr);
#endif /* ORDERDEBUG2 */
    if (report)
	printf("mark :%d\n", mark);
    if (report) {
	static bool need_ignores = true;
	const char *ts;
	ct = display_date(commit, mark, opts->force_dates);
	ts = utc_offset_timestamp(&ct, timezone);
	//printf("author %s <%s> %s\n", full, email, ts);
	printf("committer %s <%s> %s\n", full, email, ts);
	if (!opts->embed_ids)
	    printf("data %zd\n%s\n", strlen(commit->log), commit->log);
	else
	    printf("data %zd\n%s\n%s\n", strlen(commit->log) + strlen(revpairs) + 1,
		commit->log, revpairs);
	if (commit->parent)
	    printf("from :%d\n", markmap[commit->parent->serial]);

	for (op2 = operations; op2 < op; op2++)
	{
	    assert(op2->op == 'M' || op2->op == 'D');
	    if (op2->op == 'M')
		printf("M 100%o :%d %s\n", 
		       op2->mode, 
		       markmap[op2->rev->serial], 
		       op2->path);
	    if (op2->op == 'D')
		printf("D %s\n", op2->path);
	    /*
	     * If there's a .gitignore in the first commit, don't generate one.
	     * export_blob() will already have prepended them.
	     */
	    if (need_ignores && op2->path == s_gitignore)
		need_ignores = false;
	}
	if (need_ignores) {
	    need_ignores = false;
	    printf("M 100644 inline .gitignore\ndata %zd\n%s\n",
		   sizeof(CVS_IGNORES)-1, CVS_IGNORES);
	}
	if (revpairs != NULL && strlen(revpairs) > 0)
	{
	    if (opts->revision_map) {
		char *cp;
		for (cp = revpairs; *cp; cp++) {
		    if (*cp == '\n')
			fprintf(opts->revision_map, " :%d", here);
		    fputc(*cp, opts->revision_map);
		}
	    }
	    if (opts->reposurgeon)
	    {
		if (report)
		    printf("property cvs-revision %zd %s", strlen(revpairs), revpairs);
	    }
	}
    }
    free(revpairs);
    free(operations);

    if (report)
	printf("\n");
#undef OP_CHUNK
}

static int export_ncommit(const git_repo *rl)
/* return a count of converted commits */
{
    rev_ref	*h;
    git_commit	*c;
    int		n = 0;
    
    for (h = rl->heads; h; h = h->next) {
	if (h->tail)
	    continue;
	/* PUNNING: see the big comment in cvs.h */ 
	for (c = (git_commit *)h->commit; c; c = c->parent) {
	    n++;
	    if (c->tail)
		break;
	}
    }
    return n;
}

struct commit_seq {
    git_commit *commit;
    rev_ref *head;
    bool realized;
};

static int compare_commit(const git_commit *ac, const git_commit *bc)
/* attempt the mathemaically impossible total ordering on the DAG */
{
    time_t timediff;
    int cmp;
    
    timediff = ac->date - bc->date;
    if (timediff != 0)
	return timediff;
    timediff = ac->date - bc->date;
    if (timediff != 0)
	return timediff;
    if (bc == ac->parent || (ac->parent != NULL && bc == ac->parent->parent))
	return 1;
    if (ac == bc->parent || (bc->parent != NULL && ac == bc->parent->parent))
	return -1;

    /* 
     * Any remaining tiebreakers would be essentially arbitrary,
     * inserted just to have as few cases where the threaded scheduler
     * is random as posssible.
     */
    cmp = strcmp(ac->author, bc->author);
    if (cmp != 0)
	return cmp;
    cmp = strcmp(ac->log, bc->log);
    if (cmp != 0)
	return cmp;
    
    return 0;
}

static int sort_by_date(const void *ap, const void *bp)
/* return > 0 if ap newer than bp, < 0 if bp newer than ap */
{
    git_commit *ac = ((struct commit_seq *)ap)->commit;
    git_commit *bc = ((struct commit_seq *)bp)->commit;

    /* older parents drag tied commits back in time (in effect) */ 
    for (;;) {
	int cmp;
	if (ac == bc)
	    return 0;
	cmp = compare_commit(ac, bc);
	if (cmp != 0)
	    return cmp;
	if (ac->parent != NULL && bc->parent != NULL) {
	    ac = ac->parent;
	    bc = bc->parent;
	    continue;
	}
	return 0;
    }
}

static struct commit_seq *canonicalize(git_repo *rl)
/* copy/sort merged commits into git-fast-export order */
{
    /*
     * Dump in canonical (strict git-fast-export) order.
     *
     * Commits are in reverse order on per-branch lists.  The branches
     * have to ship in their current order, otherwise some marks may not 
     * be resolved.
     *
     * Dump them all into a common array because (a) we're going to
     * need to ship them back to front, and (b) we'd prefer to ship
     * them in canonical order by commit date rather than ordered by
     * branches.
     *
     * But there's a hitch; the branches themselves need to be dumped
     * in forward order, otherwise not all ancestor marks will be defined.
     * Since the branch commits need to be dumped in reverse, the easiest
     * way to arrange this is to reverse the branches in the array, fill
     * the array in forward order, and dump it forward order.
     */
    struct commit_seq *history;
    int n;
    int branchbase;
    rev_ref *h;
    git_commit *c;

    history = (struct commit_seq *)xcalloc(export_stats.export_total_commits, 
					   sizeof(struct commit_seq),
					   "export");
#ifdef ORDERDEBUG
    fputs("Export phase 1:\n", stderr);
#endif /* ORDERDEBUG */
    branchbase = 0;
    for (h = rl->heads; h; h = h->next) {
	if (!h->tail) {
	    int i = 0, branchlength = 0;
	    /* PUNNING: see the big comment in cvs.h */ 
	    for (c = (git_commit *)h->commit; c; c = (c->tail ? NULL : c->parent))
		branchlength++;
	    /* PUNNING: see the big comment in cvs.h */ 
	    for (c = (git_commit *)h->commit; c; c = (c->tail ? NULL : c->parent)) {
		/* copy commits in reverse order into this branch's span */
		n = branchbase + branchlength - (i + 1);
		history[n].commit = c;
		history[n].head = h;
		i++;
#ifdef ORDERDEBUG
		fprintf(stderr, "At n = %d, i = %d\n", n, i);
		dump_commit(c, stderr);
#endif /* ORDERDEBUG */
	    }
	    branchbase += branchlength;
	}
    }

    return history;
}

void export_authors(forest_t *forest, export_options_t *opts)
/* dump a list of author IDs in the repository */
{
    const char **authors;
    int i, nauthors = 0;
    size_t alloc;
    authors = NULL;
    alloc = 0;
    export_stats.export_total_commits = export_ncommit(forest->head);
    struct commit_seq *hp, *history = canonicalize(forest->head);

    progress_begin("Finding authors...", NO_MAX);
    for (hp = history; hp < history + export_stats.export_total_commits; hp++) {
	for (i = 0; i < nauthors; i++) {
	    if (authors[i] == hp->commit->author)
		goto duplicate;
	}
	if (nauthors >= alloc) {
	    alloc += 1024;
	    authors = xrealloc(authors, sizeof(char*) * alloc, "author list");
	}
	authors[nauthors++] = hp->commit->author;
    duplicate:;
    }
    progress_end("done");

    for (i = 0; i < nauthors; i++)
	printf("%s\n", authors[i]);

    free(authors);
    free(history);
}

void export_commits(forest_t *forest, 
		    export_options_t *opts, export_stats_t *stats)
/* export a revision list as a git fast-import stream in canonical order */
{
    rev_ref *h;
    tag_t *t;
    git_commit *c;
    git_repo *rl = forest->head;
    generator_t *gp;
    int recount = 0;

    if (opts->fromtime > 0)
	opts->reportmode = canonical;
    else if (opts->reportmode == adaptive) {
	if (forest->textsize <= SMALL_REPOSITORY)
	    opts->reportmode = canonical;
        else
	    opts->reportmode = fast;
    }

    if (opts->reportmode == canonical)
    {
	char *tmp = getenv("TMPDIR");
	if (tmp == NULL) 
	    tmp = "/tmp";
	seqno = mark = 0;
	snprintf(blobdir, sizeof(blobdir), "%s/cvs-fast-export-XXXXXX", tmp);
	if (mkdtemp(blobdir) == NULL)
	    fatal_error("temp dir creation failed\n");
    }

    export_stats.export_total_commits = export_ncommit(rl);
    /* the +1 is because mark indices are 1-origin, slot 0 always empty */
    markmap = (serial_t *)xcalloc(sizeof(serial_t),
				  forest->total_revisions + export_stats.export_total_commits + 1,
				  "markmap allocation");

    /* export_blob() touches markmap when in fast mode */
    progress_begin("Generating snapshots...", forest->filecount);
    for (gp = forest->generators; 
	 gp < forest->generators + forest->filecount;
	 gp++) {
	generate_files(gp, opts, export_blob);
	generator_free(gp);
	progress_jump(++recount);
    }
    progress_end("done");

    if (progress)
    {
	static char msgbuf[100];
	snprintf(msgbuf, sizeof(msgbuf), "Saving in %s order: ",
		opts->reportmode == fast ? "fast" : "canonical");
	progress_begin(msgbuf, export_stats.export_total_commits);
    }

    if (opts->reportmode == fast) {
	/*
	 * Dump by branch order, not by commit date.  Slightly faster and
	 * less memory-intensive, but (a) incremental dump won't work, and
	 * (b) it's not git-fast-export  canonical form and cannot be 
	 * directly compared to the output of other tools.
	 */
	git_commit **history;
	int alloc, i;
	int n;

	for (h = rl->heads; h; h = h->next) {
	    if (!h->tail) {
		// We need to export commits in reverse order; so
		// first of all, we convert the linked-list given by
		// h->commit into the array "history".
		history = NULL;
		alloc = 0;
		/* PUNNING: see the big comment in cvs.h */ 
		for (c=(git_commit *)h->commit, n=0; c; c=(c->tail ? NULL : c->parent), n++) {
		    if (n >= alloc) {
			alloc += 1024;
			history = (git_commit **)xrealloc(history, alloc *sizeof(git_commit *), "export");
		    }
		    history[n] = c;
		}

		/*
		 * Now walk the history array in reverse order and export the
		 * commits, along with any matching tags.
		 */
		for (i=n-1; i>=0; i--) {
		    export_commit(history[i], h->ref_name, true, opts);
		    progress_step();
		    for (t = all_tags; t; t = t->next)
			if (t->commit == history[i] && display_date(history[i], markmap[history[i]->serial], opts->force_dates) > opts->fromtime)
			    printf("reset refs/tags/%s\nfrom :%d\n\n", t->name, markmap[history[i]->serial]);
		}

		free(history);
	    }
	}
    }
    else 
    {	
	struct commit_seq *history, *hp;
	bool sortable;

	history = canonicalize(rl);
 
#ifdef ORDERDEBUG2
	fputs("Export phase 2:\n", stderr);
	for (hp = history; hp < history + export_stats.export_total_commits; hp++)
	    dump_commit(hp->commit, stderr);
#endif /* ORDERDEBUG2 */

	/* 
	 * Check that the topo order is consistent with time order.
	 * If so, we can sort commits by date without worrying that
	 * we'll try to ship a mark before it's defined.
	 */
	sortable = true;
	for (hp = history; hp < history + export_stats.export_total_commits; hp++) {
	    if (hp->commit->parent && hp->commit->parent->date > hp->commit->date) {
		sortable = false;
		announce("some parent commits are younger than children.\n");
		break;
	    }
	}
	if (sortable)
	    qsort((void *)history, 
		  export_stats.export_total_commits, sizeof(struct commit_seq),
		  sort_by_date);

#ifdef ORDERDEBUG2
	fputs("Export phase 3:\n", stderr);
#endif /* ORDERDEBUG2 */
	for (hp = history; hp < history + export_stats.export_total_commits; hp++) {
	    bool report = true;
	    if (opts->fromtime > 0) {
		if (opts->fromtime >= display_date(hp->commit, mark+1, opts->force_dates)) {
		    report = false;
		} else if (!hp->realized) {
		    struct commit_seq *lp;
		    if (hp->commit->parent != NULL && display_date(hp->commit->parent, markmap[hp->commit->parent->serial], opts->force_dates) < opts->fromtime)
			(void)printf("from %s%s^0\n\n", opts->branch_prefix, hp->head->ref_name);
		    for (lp = hp; lp < history + export_stats.export_total_commits; lp++) {
			if (lp->head == hp->head) {
			    lp->realized = true;
			}
		    }
		}
	    }
	    progress_jump(hp - history);
	    export_commit(hp->commit, hp->head->ref_name, report, opts);
	    for (t = all_tags; t; t = t->next)
		if (t->commit == hp->commit && display_date(hp->commit, markmap[hp->commit->serial], opts->force_dates) > opts->fromtime)
		    printf("reset refs/tags/%s\nfrom :%d\n\n", t->name, markmap[hp->commit->serial]);
	}

	free(history);
    }

    for (h = rl->heads; h; h = h->next) {
	if (display_date(h->commit, markmap[h->commit->serial], opts->force_dates) > opts->fromtime)
	    printf("reset %s%s\nfrom :%d\n\n",
		   opts->branch_prefix,
		   h->ref_name,
		   markmap[h->commit->serial]);
    }
    free(markmap);

    progress_end("done");

    fputs("done\n", stdout);

    nftw(blobdir, unlink_cb, 64, FTW_DEPTH | FTW_PHYS);

    if (forest->skew_vulnerable > 0 && forest->filecount > 1 && !opts->force_dates) {
	time_t udate = forest->skew_vulnerable;
	announce("no commitids before %s.\n", cvstime2rfc3339(udate));
    }

    memcpy(stats, &export_stats, sizeof(export_stats_t));
}

/* end */
