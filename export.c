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

#include <limits.h>
#include <assert.h>
#include "cvs.h"

struct mark {
    int external;
    bool emitted;
};
static struct mark *markmap;
static int seqno, mark;
static char blobdir[PATH_MAX];

void
export_init(void)
{
    seqno = mark = 0;
    snprintf(blobdir, sizeof(blobdir), "/tmp/cvs-fast-export-%d", getpid());
    mkdir(blobdir, 0770);
}

static char *
blobfile(int m)
{
    static char path[PATH_MAX];
    (void)snprintf(path, sizeof(path), "%s/%d", blobdir, m);
    return path;
}

void 
export_blob(Node *node, void *buf, unsigned long len)
{
    FILE *wfp;
    
    node->file->serial = ++seqno;

    wfp = fopen(blobfile(seqno), "w");
    assert(wfp);
    fprintf(wfp, "data %zd\n", len);
    fwrite(buf, len, sizeof(char), wfp);
    fputc('\n', wfp);
    (void)fclose(wfp);
}

static void 
drop_path_component(char *string, const char *drop)
{
    char *c;
	int  l, m;
	m = strlen(drop);
    while ((c = strstr (string, drop)) &&
	   (c == string || c[-1] == '/'))
    {
	l = strlen (c);
	memmove (c, c + m, l - m + 1);
    }
}

static char *
export_filename (rev_file *file, int strip)
{
    static char name[PATH_MAX];
    int	    len;
    
    if (strlen (file->name) - strip >= MAXPATHLEN)
    {
	fprintf(stderr, "File name %s\n too long\n", file->name);
	exit(1);
    }
    strcpy (name, file->name + strip);
	drop_path_component(name, "Attic/");
	drop_path_component(name, "RCS/");
    len = strlen (name);
    if (len > 2 && !strcmp (name + len - 2, ",v"))
	name[len-2] = '\0';

    if (strcmp(name, ".cvsignore") == 0)
    {
	name[1] = 'g';
	name[2] = 'i';
	name[3] = 't';
    }

    return name;
}

void export_wrap(void)
{
    (void)rmdir(blobdir);
}

static const char *utc_offset_timestamp(const time_t *timep, const char *tz)
{
    static char outbuf[BUFSIZ];

    struct tm *tm;
    char tzbuf[BUFSIZ];
    /* coverity[tainted_data] */
    char *oldtz = getenv("TZ");

    // make a copy in case original is clobbered
    if (oldtz != NULL)
	strncpy(tzbuf, oldtz, sizeof(tzbuf)-1);

    setenv("TZ", tz, 1);
    tzset();  // just in case ...

    tm = localtime(timep);
    strftime(outbuf, sizeof(outbuf), "%s %z", tm);

    if (oldtz != NULL)
	setenv("TZ", tzbuf, 1);
    else
	unsetenv("TZ");
    tzset();

    return outbuf;
}

static int export_total_commits;
static int export_current_commit;
static char *export_current_head;

#define STATUS	stderr
#define PROGRESS_LEN	20

static void
export_status (void)
{
    int	spot = export_current_commit * PROGRESS_LEN / export_total_commits;
    int	s;

    fprintf (STATUS, "\rSave: %35.35s ", export_current_head);
    for (s = 0; s < PROGRESS_LEN + 1; s++)
	putc (s == spot ? '*' : '.', STATUS);
    fprintf (STATUS, " %5d of %5d ", export_current_commit, export_total_commits);
    fflush (STATUS);
}

static void
export_commit(rev_commit *commit, char *branch, int strip)
{
#define OP_CHUNK	32
    cvs_author *author;
    char *full;
    char *email;
    char *timezone;
    char *revpairs = NULL;
    size_t revpairsize = 0;
    const char *ts;
    time_t ct;
    rev_file	*f, *f2;
    int		i, j, i2, j2;
    struct fileop {
	char op;
	mode_t mode;
	int serial;
	char path[PATH_MAX];
    };
    struct fileop *operations, *op, *op2;
    int noperations;

    if (reposurgeon)
    {
	revpairs = xmalloc((revpairsize = 1024));
	revpairs[0] = '\0';
    }

    noperations = OP_CHUNK;
    op = operations = xmalloc(sizeof(struct fileop) * noperations);
    for (i = 0; i < commit->ndirs; i++) {
	rev_dir	*dir = commit->dirs[i];
	
	for (j = 0; j < dir->nfiles; j++) {
	    char *stripped;
	    bool present, changed;
	    f = dir->files[j];
	    stripped = export_filename(f, strip);
	    present = false;
	    changed = false;
	    if (commit->parent) {
		for (i2 = 0; i2 < commit->parent->ndirs; i2++) {
		    rev_dir	*dir2 = commit->parent->dirs[i2];
		    for (j2 = 0; j2 < dir2->nfiles; j2++) {
			f2 = dir2->files[j2];
			if (strcmp(f->name, f2->name) == 0) {
			    present = true;
			    changed = (f->serial != f2->serial);
			}
		    }
		}
	    }
	    if (!present || changed) {

		op->op = 'M';
		// git fast-import only supports 644 and 755 file modes
		if (f->mode & 0100)
			op->mode = 0755;
		else
			op->mode = 0644;
		op->serial = f->serial;
		(void)strncpy(op->path, stripped, PATH_MAX-1);
		op++;
		if (op > operations + noperations)
		{
		    noperations += OP_CHUNK;
		    operations = realloc(operations, sizeof(struct fileop) * noperations);
		}

		if (revision_map || reposurgeon) {
		    char *fr = stringify_revision(stripped, " ", &f->number);
		    if (revision_map)
			fprintf(revision_map, "%s :%d\n", fr, markmap[f->serial].external);
		    if (reposurgeon)
		    {
			if (strlen(revpairs) + strlen(fr) + 2 > revpairsize)
			{
			    revpairsize += strlen(fr) + 2;
			    revpairs = xrealloc(revpairs, revpairsize);
			}
			strcat(revpairs, fr);
			strcat(revpairs, "\n");
		    }
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
		f = dir->files[j];
		present = false;
		for (i2 = 0; i2 < commit->ndirs; i2++) {
		    rev_dir	*dir2 = commit->dirs[i2];
		    for (j2 = 0; j2 < dir2->nfiles; j2++) {
			f2 = dir2->files[j2];
			if (strcmp(f->name, f2->name) == 0) {
			    present = true;
			}
		    }
		}
		if (!present) {
		    op->op = 'D';
		    (void)strncpy(op->path, 
				  export_filename(f, strip),
				  PATH_MAX-1);
		    op++;
		    if (op > operations + noperations)
		    {
			noperations += OP_CHUNK;
			operations = realloc(operations, sizeof(struct fileop) * noperations);
		    }
		}
	    }
	}
    }

    for (op2 = operations; op2 < op; op2++)
    {
	//printf("OPVECTOR: %c %d %s %d\n", op2->op, op2->serial, op2->path, markmap[op2->serial].emitted);
	if (op2->op == 'M' && !markmap[op2->serial].emitted)
	{
	    char *fn = blobfile(op2->serial);
	    FILE *rfp = fopen(fn, "r");
	    char c;
	    if (rfp)
	    {
		markmap[op2->serial].external = ++mark; 
		printf("blob\nmark :%d\n", mark);
		while ((c = fgetc(rfp)) != EOF)
		    putchar(c);
		(void) unlink(fn);
		markmap[op2->serial].emitted = true;
	    }
	}
    }

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

    printf("commit %s%s\n", branch_prefix, branch);
    markmap[++seqno].external = ++mark;
    printf("mark :%d\n", mark);
    commit->serial = seqno;
    ct = force_dates ? seqno * commit_time_window * 2 : commit->date;
    ts = utc_offset_timestamp(&ct, timezone);
    printf("author %s <%s> %s\n", full, email, ts);
    printf("committer %s <%s> %s\n", full, email, ts);
    printf("data %zd\n%s\n", strlen(commit->log), commit->log);
    if (commit->parent)
	printf("from :%d\n", markmap[commit->parent->serial].external);

    for (op2 = operations; op2 < op; op2++)
    {
	assert(op2->op == 'M' || op2->op == 'D');
	if (op2->op == 'M')
	    printf("M 100%o :%d %s\n", 
		   op2->mode, 
		   markmap[op2->serial].external, 
		   op2->path);
	if (op2->op == 'D')
	    printf("D %s\n", op2->path);
    }
    free(operations);

    if (reposurgeon) 
    {
	printf("property cvs-revision %zd %s", strlen(revpairs), revpairs);
	free(revpairs);
    }

    printf ("\n");
#undef OP_CHUNK
    }

static int
export_ncommit (rev_list *rl)
{
    rev_ref	*h;
    rev_commit	*c;
    int		n = 0;
    
    for (h = rl->heads; h; h = h->next) {
	if (h->tail)
	    continue;
	for (c = h->commit; c; c = c->parent) {
	    n++;
	    if (c->tail)
		break;
	}
    }
    return n;
}

bool
export_commits (rev_list *rl, int strip)
{
    rev_ref *h;
    Tag *t;
    rev_commit *c;
    rev_commit **history;
    int alloc, n, i;
    size_t extent;

    export_total_commits = export_ncommit (rl);
    /* the +1 is because mark indices are 1-origin, slot 0 always empty */
    extent = sizeof(struct mark) * (seqno + export_total_commits + 1);
    markmap = xmalloc(extent);
    memset(markmap, '\0', extent);
    export_current_commit = 0;
    for (h = rl->heads; h; h = h->next) {
	export_current_head = h->name;
	if (!h->tail) {
	    // We need to export commits in reverse order; so first of all, we
	    // convert the linked-list given by h->commit into the array
	    // "history".
	    history = NULL;
	    alloc = 0;
	    for (c=h->commit, n=0; c; c=(c->tail ? NULL : c->parent), n++) {
		if (n >= alloc) {
		    alloc += 100;
		    history = (rev_commit **) realloc(history, alloc * sizeof(rev_commit*));
		}
		history[n] = c;
	    }

	    // Now walk the history array in reverse order and export the
	    // commits, along with any matching tags.
	    for (i=n-1; i>=0; i--) {
		++export_current_commit;
		export_status ();
		export_commit (history[i], h->name, strip);
		for (t = all_tags; t; t = t->next)
		    if (t->commit == history[i])
			printf("reset refs/tags/%s\nfrom :%d\n\n", t->name, markmap[history[i]->serial].external);
	    }

	    free(history);
	}
	fprintf(STATUS, "\n");
	fflush(STATUS);
	printf("reset %s%s\nfrom :%d\n\n", 
	       branch_prefix, 
	       h->name, 
	       markmap[h->commit->serial].external);
    }
    fprintf (STATUS, "\n");
    free(markmap);
    return true;
}

#define PROGRESS_LEN	20

void load_status (char *name)
{
    int	spot = load_current_file * PROGRESS_LEN / load_total_files;
    int	    s;
    int	    l;

    l = strlen (name);
    if (l > 35) name += l - 35;

    fprintf (STATUS, "\rLoad: %35.35s ", name);
    for (s = 0; s < PROGRESS_LEN + 1; s++)
	putc (s == spot ? '*' : '.', STATUS);
    fprintf (STATUS, " %5d of %5d ", load_current_file, load_total_files);
    fflush (STATUS);
}

void load_status_next (void)
{
    fprintf (STATUS, "\n");
    fflush (STATUS);
}

/* end */
