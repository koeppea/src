/*	$OpenBSD: main.c,v 1.230 2019/07/03 16:33:06 espie Exp $ */
/*
 * Copyright (c) 2008-2012 Kristaps Dzonsons <kristaps@bsd.lv>
 * Copyright (c) 2010-2012, 2014-2019 Ingo Schwarze <schwarze@openbsd.org>
 * Copyright (c) 2010 Joerg Sonnenberger <joerg@netbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHORS DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/param.h>	/* MACHINE */
#include <sys/stat.h>
#include <sys/wait.h>

#include <assert.h>
#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "mandoc_aux.h"
#include "mandoc.h"
#include "mandoc_xr.h"
#include "roff.h"
#include "mdoc.h"
#include "man.h"
#include "mandoc_parse.h"
#include "tag.h"
#include "main.h"
#include "manconf.h"
#include "mansearch.h"

enum	outmode {
	OUTMODE_DEF = 0,
	OUTMODE_FLN,
	OUTMODE_LST,
	OUTMODE_ALL,
	OUTMODE_ONE
};

enum	outt {
	OUTT_ASCII = 0,	/* -Tascii */
	OUTT_LOCALE,	/* -Tlocale */
	OUTT_UTF8,	/* -Tutf8 */
	OUTT_TREE,	/* -Ttree */
	OUTT_MAN,	/* -Tman */
	OUTT_HTML,	/* -Thtml */
	OUTT_MARKDOWN,	/* -Tmarkdown */
	OUTT_LINT,	/* -Tlint */
	OUTT_PS,	/* -Tps */
	OUTT_PDF	/* -Tpdf */
};

struct	curparse {
	struct mparse	 *mp;
	struct manoutput *outopts;	/* output options */
	void		 *outdata;	/* data for output */
	char		 *os_s;		/* operating system for display */
	int		  wstop;	/* stop after a file with a warning */
	enum mandoc_os	  os_e;		/* check base system conventions */
	enum outt	  outtype;	/* which output to use */
};


int			  mandocdb(int, char *[]);

static	void		  check_xr(void);
static	int		  fs_lookup(const struct manpaths *,
				size_t ipath, const char *,
				const char *, const char *,
				struct manpage **, size_t *);
static	int		  fs_search(const struct mansearch *,
				const struct manpaths *, int, char**,
				struct manpage **, size_t *);
static	int		  koptions(int *, char *);
static	void		  moptions(int *, char *);
static	void		  outdata_alloc(struct curparse *);
static	void		  parse(struct curparse *, int, const char *);
static	void		  passthrough(const char *, int, int);
static	pid_t		  spawn_pager(struct tag_files *);
static	int		  toptions(struct curparse *, char *);
static	void		  usage(enum argmode) __attribute__((__noreturn__));
static	int		  woptions(struct curparse *, char *);

static	const int sec_prios[] = {1, 4, 5, 8, 6, 3, 7, 2, 9};
static	char		  help_arg[] = "help";
static	char		 *help_argv[] = {help_arg, NULL};


int
main(int argc, char *argv[])
{
	struct manconf	 conf;
	struct mansearch search;
	struct curparse	 curp;
	struct winsize	 ws;
	struct tag_files *tag_files;
	struct manpage	*res, *resp;
	const char	*progname, *sec, *thisarg;
	char		*conf_file, *defpaths, *auxpaths;
	char		*oarg, *tagarg;
	unsigned char	*uc;
	size_t		 i, sz, ssz;
	int		 prio, best_prio;
	enum outmode	 outmode;
	int		 fd, startdir;
	int		 show_usage;
	int		 options;
	int		 use_pager;
	int		 status, signum;
	int		 c;
	pid_t		 pager_pid, tc_pgid, man_pgid, pid;

	progname = getprogname();
	mandoc_msg_setoutfile(stderr);
	if (strncmp(progname, "mandocdb", 8) == 0 ||
	    strncmp(progname, "makewhatis", 10) == 0)
		return mandocdb(argc, argv);

	if (pledge("stdio rpath tmppath tty proc exec", NULL) == -1)
		err((int)MANDOCLEVEL_SYSERR, "pledge");

	/* Search options. */

	memset(&conf, 0, sizeof(conf));
	conf_file = defpaths = NULL;
	auxpaths = NULL;

	memset(&search, 0, sizeof(struct mansearch));
	search.outkey = "Nd";
	oarg = NULL;

	if (strcmp(progname, "man") == 0)
		search.argmode = ARG_NAME;
	else if (strncmp(progname, "apropos", 7) == 0)
		search.argmode = ARG_EXPR;
	else if (strncmp(progname, "whatis", 6) == 0)
		search.argmode = ARG_WORD;
	else if (strncmp(progname, "help", 4) == 0)
		search.argmode = ARG_NAME;
	else
		search.argmode = ARG_FILE;

	/* Parser and formatter options. */

	memset(&curp, 0, sizeof(struct curparse));
	curp.outtype = OUTT_LOCALE;
	curp.outopts = &conf.output;
	options = MPARSE_SO | MPARSE_UTF8 | MPARSE_LATIN1;

	use_pager = 1;
	tag_files = NULL;
	show_usage = 0;
	outmode = OUTMODE_DEF;

	while ((c = getopt(argc, argv,
	    "aC:cfhI:iK:klM:m:O:S:s:T:VW:w")) != -1) {
		if (c == 'i' && search.argmode == ARG_EXPR) {
			optind--;
			break;
		}
		switch (c) {
		case 'a':
			outmode = OUTMODE_ALL;
			break;
		case 'C':
			conf_file = optarg;
			break;
		case 'c':
			use_pager = 0;
			break;
		case 'f':
			search.argmode = ARG_WORD;
			break;
		case 'h':
			conf.output.synopsisonly = 1;
			use_pager = 0;
			outmode = OUTMODE_ALL;
			break;
		case 'I':
			if (strncmp(optarg, "os=", 3)) {
				warnx("-I %s: Bad argument", optarg);
				return (int)MANDOCLEVEL_BADARG;
			}
			if (curp.os_s != NULL) {
				warnx("-I %s: Duplicate argument", optarg);
				return (int)MANDOCLEVEL_BADARG;
			}
			curp.os_s = mandoc_strdup(optarg + 3);
			break;
		case 'K':
			if ( ! koptions(&options, optarg))
				return (int)MANDOCLEVEL_BADARG;
			break;
		case 'k':
			search.argmode = ARG_EXPR;
			break;
		case 'l':
			search.argmode = ARG_FILE;
			outmode = OUTMODE_ALL;
			break;
		case 'M':
			defpaths = optarg;
			break;
		case 'm':
			auxpaths = optarg;
			break;
		case 'O':
			oarg = optarg;
			break;
		case 'S':
			search.arch = optarg;
			break;
		case 's':
			search.sec = optarg;
			break;
		case 'T':
			if ( ! toptions(&curp, optarg))
				return (int)MANDOCLEVEL_BADARG;
			break;
		case 'W':
			if ( ! woptions(&curp, optarg))
				return (int)MANDOCLEVEL_BADARG;
			break;
		case 'w':
			outmode = OUTMODE_FLN;
			break;
		default:
			show_usage = 1;
			break;
		}
	}

	if (show_usage)
		usage(search.argmode);

	/* Postprocess options. */

	if (outmode == OUTMODE_DEF) {
		switch (search.argmode) {
		case ARG_FILE:
			outmode = OUTMODE_ALL;
			use_pager = 0;
			break;
		case ARG_NAME:
			outmode = OUTMODE_ONE;
			break;
		default:
			outmode = OUTMODE_LST;
			break;
		}
	}

	if (oarg != NULL) {
		if (outmode == OUTMODE_LST)
			search.outkey = oarg;
		else {
			while (oarg != NULL) {
				if (manconf_output(&conf.output,
				    strsep(&oarg, ","), 0) == -1)
					return (int)MANDOCLEVEL_BADARG;
			}
		}
	}

	if (curp.outtype != OUTT_TREE || !curp.outopts->noval)
		options |= MPARSE_VALIDATE;

	if (outmode == OUTMODE_FLN ||
	    outmode == OUTMODE_LST ||
	    !isatty(STDOUT_FILENO))
		use_pager = 0;

	if (use_pager &&
	    (conf.output.width == 0 || conf.output.indent == 0) &&
	    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != -1 &&
	    ws.ws_col > 1) {
		if (conf.output.width == 0 && ws.ws_col < 79)
			conf.output.width = ws.ws_col - 1;
		if (conf.output.indent == 0 && ws.ws_col < 66)
			conf.output.indent = 3;
	}

	if (!use_pager)
		if (pledge("stdio rpath", NULL) == -1)
			err((int)MANDOCLEVEL_SYSERR, "pledge");

	/* Parse arguments. */

	if (argc > 0) {
		argc -= optind;
		argv += optind;
	}
	resp = NULL;

	/*
	 * Quirks for help(1)
	 * and for a man(1) section argument without -s.
	 */

	if (search.argmode == ARG_NAME) {
		if (*progname == 'h') {
			if (argc == 0) {
				argv = help_argv;
				argc = 1;
			}
		} else if (argc > 1 &&
		    ((uc = (unsigned char *)argv[0]) != NULL) &&
		    ((isdigit(uc[0]) && (uc[1] == '\0' ||
		      isalpha(uc[1]))) ||
		     (uc[0] == 'n' && uc[1] == '\0'))) {
			search.sec = (char *)uc;
			argv++;
			argc--;
		}
		if (search.arch == NULL)
			search.arch = getenv("MACHINE");
		if (search.arch == NULL)
			search.arch = MACHINE;
	}

	/*
	 * Use the first argument for -O tag in addition to
	 * using it as a search term for man(1) or apropos(1).
	 */

	if (conf.output.tag != NULL && *conf.output.tag == '\0') {
		tagarg = argc > 0 && search.argmode == ARG_EXPR ?
		    strchr(*argv, '=') : NULL;
		conf.output.tag = tagarg == NULL ? *argv : tagarg + 1;
	}

	/* man(1), whatis(1), apropos(1) */

	if (search.argmode != ARG_FILE) {
		if (search.argmode == ARG_NAME &&
		    outmode == OUTMODE_ONE)
			search.firstmatch = 1;

		/* Access the mandoc database. */

		manconf_parse(&conf, conf_file, defpaths, auxpaths);
		if ( ! mansearch(&search, &conf.manpath,
		    argc, argv, &res, &sz))
			usage(search.argmode);

		if (sz == 0 && search.argmode == ARG_NAME)
			fs_search(&search, &conf.manpath,
			    argc, argv, &res, &sz);

		if (search.argmode == ARG_NAME) {
			for (c = 0; c < argc; c++) {
				if (strchr(argv[c], '/') == NULL)
					continue;
				if (access(argv[c], R_OK) == -1) {
					warn("%s", argv[c]);
					continue;
				}
				res = mandoc_reallocarray(res,
				    sz + 1, sizeof(*res));
				res[sz].file = mandoc_strdup(argv[c]);
				res[sz].names = NULL;
				res[sz].output = NULL;
				res[sz].bits = 0;
				res[sz].ipath = SIZE_MAX;
				res[sz].sec = 10;
				res[sz].form = FORM_SRC;
				sz++;
			}
		}

		if (sz == 0) {
			if (search.argmode != ARG_NAME)
				warnx("nothing appropriate");
			mandoc_msg_setrc(MANDOCLEVEL_BADARG);
			goto out;
		}

		/*
		 * For standard man(1) and -a output mode,
		 * prepare for copying filename pointers
		 * into the program parameter array.
		 */

		if (outmode == OUTMODE_ONE) {
			argc = 1;
			best_prio = 40;
		} else if (outmode == OUTMODE_ALL)
			argc = (int)sz;

		/* Iterate all matching manuals. */

		resp = res;
		for (i = 0; i < sz; i++) {
			if (outmode == OUTMODE_FLN)
				puts(res[i].file);
			else if (outmode == OUTMODE_LST)
				printf("%s - %s\n", res[i].names,
				    res[i].output == NULL ? "" :
				    res[i].output);
			else if (outmode == OUTMODE_ONE) {
				/* Search for the best section. */
				sec = res[i].file;
				sec += strcspn(sec, "123456789");
				if (sec[0] == '\0')
					continue; /* No section at all. */
				prio = sec_prios[sec[0] - '1'];
				if (search.sec != NULL) {
					ssz = strlen(search.sec);
					if (strncmp(sec, search.sec, ssz) == 0)
						sec += ssz;
				} else
					sec++; /* Prefer without suffix. */
				if (*sec != '/')
					prio += 10; /* Wrong dir name. */
				if (search.sec != NULL &&
				    (strlen(sec) <= ssz  + 3 ||
				     strcmp(sec + strlen(sec) - ssz,
				      search.sec) != 0))
					prio += 20; /* Wrong file ext. */
				if (prio >= best_prio)
					continue;
				best_prio = prio;
				resp = res + i;
			}
		}

		/*
		 * For man(1), -a and -i output mode, fall through
		 * to the main mandoc(1) code iterating files
		 * and running the parsers on each of them.
		 */

		if (outmode == OUTMODE_FLN || outmode == OUTMODE_LST)
			goto out;
	}

	/* mandoc(1) */

	if (use_pager) {
		if (pledge("stdio rpath tmppath tty proc exec", NULL) == -1)
			err((int)MANDOCLEVEL_SYSERR, "pledge");
	} else {
		if (pledge("stdio rpath", NULL) == -1)
			err((int)MANDOCLEVEL_SYSERR, "pledge");
	}

	if (search.argmode == ARG_FILE)
		moptions(&options, auxpaths);

	mchars_alloc();
	curp.mp = mparse_alloc(options, curp.os_e, curp.os_s);

	if (argc < 1) {
		if (use_pager) {
			tag_files = tag_init();
			if (tag_files != NULL)
				tag_files->tagname = conf.output.tag;
		}
		thisarg = "<stdin>";
		mandoc_msg_setinfilename(thisarg);
		parse(&curp, STDIN_FILENO, thisarg);
		mandoc_msg_setinfilename(NULL);
	}

	/*
	 * Remember the original working directory, if possible.
	 * This will be needed if some names on the command line
	 * are page names and some are relative file names.
	 * Do not error out if the current directory is not
	 * readable: Maybe it won't be needed after all.
	 */
	startdir = open(".", O_RDONLY | O_DIRECTORY);

	while (argc > 0) {

		/*
		 * Changing directories is not needed in ARG_FILE mode.
		 * Do it on a best-effort basis.  Even in case of
		 * failure, some functionality may still work.
		 */
		if (resp != NULL) {
			if (resp->ipath != SIZE_MAX)
				(void)chdir(conf.manpath.paths[resp->ipath]);
			else if (startdir != -1)
				(void)fchdir(startdir);
			thisarg = resp->file;
		} else
			thisarg = *argv;

		fd = mparse_open(curp.mp, thisarg);
		if (fd != -1) {
			if (use_pager) {
				use_pager = 0;
				tag_files = tag_init();
				if (tag_files != NULL)
					tag_files->tagname = conf.output.tag;
			}

			mandoc_msg_setinfilename(thisarg);
			if (resp == NULL || resp->form == FORM_SRC)
				parse(&curp, fd, thisarg);
			else
				passthrough(resp->file, fd,
				    conf.output.synopsisonly);
			mandoc_msg_setinfilename(NULL);

			if (ferror(stdout)) {
				if (tag_files != NULL) {
					warn("%s", tag_files->ofn);
					tag_unlink();
					tag_files = NULL;
				} else
					warn("stdout");
				mandoc_msg_setrc(MANDOCLEVEL_SYSERR);
				break;
			}

			if (argc > 1 && curp.outtype <= OUTT_UTF8) {
				if (curp.outdata == NULL)
					outdata_alloc(&curp);
				terminal_sepline(curp.outdata);
			}
		} else
			mandoc_msg(MANDOCERR_FILE, 0, 0,
			    "%s: %s", thisarg, strerror(errno));

		if (curp.wstop && mandoc_msg_getrc() != MANDOCLEVEL_OK)
			break;

		if (resp != NULL)
			resp++;
		else
			argv++;
		if (--argc)
			mparse_reset(curp.mp);
	}
	if (startdir != -1) {
		(void)fchdir(startdir);
		close(startdir);
	}

	if (curp.outdata != NULL) {
		switch (curp.outtype) {
		case OUTT_HTML:
			html_free(curp.outdata);
			break;
		case OUTT_UTF8:
		case OUTT_LOCALE:
		case OUTT_ASCII:
			ascii_free(curp.outdata);
			break;
		case OUTT_PDF:
		case OUTT_PS:
			pspdf_free(curp.outdata);
			break;
		default:
			break;
		}
	}
	mandoc_xr_free();
	mparse_free(curp.mp);
	mchars_free();

out:
	if (search.argmode != ARG_FILE) {
		manconf_free(&conf);
		mansearch_free(res, sz);
	}

	free(curp.os_s);

	/*
	 * When using a pager, finish writing both temporary files,
	 * fork it, wait for the user to close it, and clean up.
	 */

	if (tag_files != NULL) {
		fclose(stdout);
		tag_write();
		man_pgid = getpgid(0);
		tag_files->tcpgid = man_pgid == getpid() ?
		    getpgid(getppid()) : man_pgid;
		pager_pid = 0;
		signum = SIGSTOP;
		for (;;) {

			/* Stop here until moved to the foreground. */

			tc_pgid = tcgetpgrp(tag_files->ofd);
			if (tc_pgid != man_pgid) {
				if (tc_pgid == pager_pid) {
					(void)tcsetpgrp(tag_files->ofd,
					    man_pgid);
					if (signum == SIGTTIN)
						continue;
				} else
					tag_files->tcpgid = tc_pgid;
				kill(0, signum);
				continue;
			}

			/* Once in the foreground, activate the pager. */

			if (pager_pid) {
				(void)tcsetpgrp(tag_files->ofd, pager_pid);
				kill(pager_pid, SIGCONT);
			} else
				pager_pid = spawn_pager(tag_files);

			/* Wait for the pager to stop or exit. */

			while ((pid = waitpid(pager_pid, &status,
			    WUNTRACED)) == -1 && errno == EINTR)
				continue;

			if (pid == -1) {
				warn("wait");
				mandoc_msg_setrc(MANDOCLEVEL_SYSERR);
				break;
			}
			if (!WIFSTOPPED(status))
				break;

			signum = WSTOPSIG(status);
		}
		tag_unlink();
	}
	return (int)mandoc_msg_getrc();
}

static void
usage(enum argmode argmode)
{

	switch (argmode) {
	case ARG_FILE:
		fputs("usage: mandoc [-ac] [-I os=name] "
		    "[-K encoding] [-mdoc | -man] [-O options]\n"
		    "\t      [-T output] [-W level] [file ...]\n", stderr);
		break;
	case ARG_NAME:
		fputs("usage: man [-acfhklw] [-C file] [-M path] "
		    "[-m path] [-S subsection]\n"
		    "\t   [[-s] section] name ...\n", stderr);
		break;
	case ARG_WORD:
		fputs("usage: whatis [-afk] [-C file] "
		    "[-M path] [-m path] [-O outkey] [-S arch]\n"
		    "\t      [-s section] name ...\n", stderr);
		break;
	case ARG_EXPR:
		fputs("usage: apropos [-afk] [-C file] "
		    "[-M path] [-m path] [-O outkey] [-S arch]\n"
		    "\t       [-s section] expression ...\n", stderr);
		break;
	}
	exit((int)MANDOCLEVEL_BADARG);
}

static int
fs_lookup(const struct manpaths *paths, size_t ipath,
	const char *sec, const char *arch, const char *name,
	struct manpage **res, size_t *ressz)
{
	struct stat	 sb;
	glob_t		 globinfo;
	struct manpage	*page;
	char		*file;
	int		 globres;
	enum form	 form;

	form = FORM_SRC;
	mandoc_asprintf(&file, "%s/man%s/%s.%s",
	    paths->paths[ipath], sec, name, sec);
	if (stat(file, &sb) != -1)
		goto found;
	free(file);

	mandoc_asprintf(&file, "%s/cat%s/%s.0",
	    paths->paths[ipath], sec, name);
	if (stat(file, &sb) != -1) {
		form = FORM_CAT;
		goto found;
	}
	free(file);

	if (arch != NULL) {
		mandoc_asprintf(&file, "%s/man%s/%s/%s.%s",
		    paths->paths[ipath], sec, arch, name, sec);
		if (stat(file, &sb) != -1)
			goto found;
		free(file);
	}

	mandoc_asprintf(&file, "%s/man%s/%s.[01-9]*",
	    paths->paths[ipath], sec, name);
	globres = glob(file, 0, NULL, &globinfo);
	if (globres != 0 && globres != GLOB_NOMATCH)
		warn("%s: glob", file);
	free(file);
	if (globres == 0)
		file = mandoc_strdup(*globinfo.gl_pathv);
	globfree(&globinfo);
	if (globres == 0) {
		if (stat(file, &sb) != -1)
			goto found;
		free(file);
	}
	if (res != NULL || ipath + 1 != paths->sz)
		return 0;

	mandoc_asprintf(&file, "%s.%s", name, sec);
	globres = stat(file, &sb);
	free(file);
	return globres != -1;

found:
	warnx("outdated mandoc.db lacks %s(%s) entry, run makewhatis %s",
	    name, sec, paths->paths[ipath]);
	if (res == NULL) {
		free(file);
		return 1;
	}
	*res = mandoc_reallocarray(*res, ++*ressz, sizeof(struct manpage));
	page = *res + (*ressz - 1);
	page->file = file;
	page->names = NULL;
	page->output = NULL;
	page->bits = NAME_FILE & NAME_MASK;
	page->ipath = ipath;
	page->sec = (*sec >= '1' && *sec <= '9') ? *sec - '1' + 1 : 10;
	page->form = form;
	return 1;
}

static int
fs_search(const struct mansearch *cfg, const struct manpaths *paths,
	int argc, char **argv, struct manpage **res, size_t *ressz)
{
	const char *const sections[] =
	    {"1", "8", "6", "2", "3", "5", "7", "4", "9", "3p"};
	const size_t nsec = sizeof(sections)/sizeof(sections[0]);

	size_t		 ipath, isec, lastsz;

	assert(cfg->argmode == ARG_NAME);

	if (res != NULL)
		*res = NULL;
	*ressz = lastsz = 0;
	while (argc) {
		for (ipath = 0; ipath < paths->sz; ipath++) {
			if (cfg->sec != NULL) {
				if (fs_lookup(paths, ipath, cfg->sec,
				    cfg->arch, *argv, res, ressz) &&
				    cfg->firstmatch)
					return 1;
			} else for (isec = 0; isec < nsec; isec++)
				if (fs_lookup(paths, ipath, sections[isec],
				    cfg->arch, *argv, res, ressz) &&
				    cfg->firstmatch)
					return 1;
		}
		if (res != NULL && *ressz == lastsz &&
		    strchr(*argv, '/') == NULL) {
			if (cfg->arch != NULL &&
			    arch_valid(cfg->arch, MANDOC_OS_OPENBSD) == 0)
				warnx("Unknown architecture \"%s\".",
				    cfg->arch);
			else if (cfg->sec == NULL)
				warnx("No entry for %s in the manual.",
				    *argv);
			else
				warnx("No entry for %s in section %s "
				    "of the manual.", *argv, cfg->sec);
		}
		lastsz = *ressz;
		argv++;
		argc--;
	}
	return 0;
}

static void
parse(struct curparse *curp, int fd, const char *file)
{
	struct roff_meta *meta;

	/* Begin by parsing the file itself. */

	assert(file);
	assert(fd >= 0);

	mparse_readfd(curp->mp, fd, file);
	if (fd != STDIN_FILENO)
		close(fd);

	/*
	 * With -Wstop and warnings or errors of at least the requested
	 * level, do not produce output.
	 */

	if (curp->wstop && mandoc_msg_getrc() != MANDOCLEVEL_OK)
		return;

	if (curp->outdata == NULL)
		outdata_alloc(curp);
	else if (curp->outtype == OUTT_HTML)
		html_reset(curp);

	mandoc_xr_reset();
	meta = mparse_result(curp->mp);

	/* Execute the out device, if it exists. */

	if (meta->macroset == MACROSET_MDOC) {
		switch (curp->outtype) {
		case OUTT_HTML:
			html_mdoc(curp->outdata, meta);
			break;
		case OUTT_TREE:
			tree_mdoc(curp->outdata, meta);
			break;
		case OUTT_MAN:
			man_mdoc(curp->outdata, meta);
			break;
		case OUTT_PDF:
		case OUTT_ASCII:
		case OUTT_UTF8:
		case OUTT_LOCALE:
		case OUTT_PS:
			terminal_mdoc(curp->outdata, meta);
			break;
		case OUTT_MARKDOWN:
			markdown_mdoc(curp->outdata, meta);
			break;
		default:
			break;
		}
	}
	if (meta->macroset == MACROSET_MAN) {
		switch (curp->outtype) {
		case OUTT_HTML:
			html_man(curp->outdata, meta);
			break;
		case OUTT_TREE:
			tree_man(curp->outdata, meta);
			break;
		case OUTT_MAN:
			mparse_copy(curp->mp);
			break;
		case OUTT_PDF:
		case OUTT_ASCII:
		case OUTT_UTF8:
		case OUTT_LOCALE:
		case OUTT_PS:
			terminal_man(curp->outdata, meta);
			break;
		default:
			break;
		}
	}
	if (mandoc_msg_getmin() < MANDOCERR_STYLE)
		check_xr();
}

static void
check_xr(void)
{
	static struct manpaths	 paths;
	struct mansearch	 search;
	struct mandoc_xr	*xr;
	size_t			 sz;

	if (paths.sz == 0)
		manpath_base(&paths);

	for (xr = mandoc_xr_get(); xr != NULL; xr = xr->next) {
		if (xr->line == -1)
			continue;
		search.arch = NULL;
		search.sec = xr->sec;
		search.outkey = NULL;
		search.argmode = ARG_NAME;
		search.firstmatch = 1;
		if (mansearch(&search, &paths, 1, &xr->name, NULL, &sz))
			continue;
		if (fs_search(&search, &paths, 1, &xr->name, NULL, &sz))
			continue;
		if (xr->count == 1)
			mandoc_msg(MANDOCERR_XR_BAD, xr->line,
			    xr->pos + 1, "Xr %s %s", xr->name, xr->sec);
		else
			mandoc_msg(MANDOCERR_XR_BAD, xr->line,
			    xr->pos + 1, "Xr %s %s (%d times)",
			    xr->name, xr->sec, xr->count);
	}
}

static void
outdata_alloc(struct curparse *curp)
{
	switch (curp->outtype) {
	case OUTT_HTML:
		curp->outdata = html_alloc(curp->outopts);
		break;
	case OUTT_UTF8:
		curp->outdata = utf8_alloc(curp->outopts);
		break;
	case OUTT_LOCALE:
		curp->outdata = locale_alloc(curp->outopts);
		break;
	case OUTT_ASCII:
		curp->outdata = ascii_alloc(curp->outopts);
		break;
	case OUTT_PDF:
		curp->outdata = pdf_alloc(curp->outopts);
		break;
	case OUTT_PS:
		curp->outdata = ps_alloc(curp->outopts);
		break;
	default:
		break;
	}
}

static void
passthrough(const char *file, int fd, int synopsis_only)
{
	const char	 synb[] = "S\bSY\bYN\bNO\bOP\bPS\bSI\bIS\bS";
	const char	 synr[] = "SYNOPSIS";

	FILE		*stream;
	const char	*syscall;
	char		*line, *cp;
	size_t		 linesz;
	ssize_t		 len, written;
	int		 print;

	line = NULL;
	linesz = 0;

	if (fflush(stdout) == EOF) {
		syscall = "fflush";
		goto fail;
	}

	if ((stream = fdopen(fd, "r")) == NULL) {
		close(fd);
		syscall = "fdopen";
		goto fail;
	}

	print = 0;
	while ((len = getline(&line, &linesz, stream)) != -1) {
		cp = line;
		if (synopsis_only) {
			if (print) {
				if ( ! isspace((unsigned char)*cp))
					goto done;
				while (isspace((unsigned char)*cp)) {
					cp++;
					len--;
				}
			} else {
				if (strcmp(cp, synb) == 0 ||
				    strcmp(cp, synr) == 0)
					print = 1;
				continue;
			}
		}
		for (; len > 0; len -= written) {
			if ((written = write(STDOUT_FILENO, cp, len)) != -1)
				continue;
			fclose(stream);
			syscall = "write";
			goto fail;
		}
	}

	if (ferror(stream)) {
		fclose(stream);
		syscall = "getline";
		goto fail;
	}

done:
	free(line);
	fclose(stream);
	return;

fail:
	free(line);
	warn("%s: SYSERR: %s", file, syscall);
	mandoc_msg_setrc(MANDOCLEVEL_SYSERR);
}

static int
koptions(int *options, char *arg)
{

	if ( ! strcmp(arg, "utf-8")) {
		*options |=  MPARSE_UTF8;
		*options &= ~MPARSE_LATIN1;
	} else if ( ! strcmp(arg, "iso-8859-1")) {
		*options |=  MPARSE_LATIN1;
		*options &= ~MPARSE_UTF8;
	} else if ( ! strcmp(arg, "us-ascii")) {
		*options &= ~(MPARSE_UTF8 | MPARSE_LATIN1);
	} else {
		warnx("-K %s: Bad argument", arg);
		return 0;
	}
	return 1;
}

static void
moptions(int *options, char *arg)
{

	if (arg == NULL)
		return;
	if (strcmp(arg, "doc") == 0)
		*options |= MPARSE_MDOC;
	else if (strcmp(arg, "an") == 0)
		*options |= MPARSE_MAN;
}

static int
toptions(struct curparse *curp, char *arg)
{

	if (0 == strcmp(arg, "ascii"))
		curp->outtype = OUTT_ASCII;
	else if (0 == strcmp(arg, "lint")) {
		curp->outtype = OUTT_LINT;
		mandoc_msg_setoutfile(stdout);
		mandoc_msg_setmin(MANDOCERR_BASE);
	} else if (0 == strcmp(arg, "tree"))
		curp->outtype = OUTT_TREE;
	else if (0 == strcmp(arg, "man"))
		curp->outtype = OUTT_MAN;
	else if (0 == strcmp(arg, "html"))
		curp->outtype = OUTT_HTML;
	else if (0 == strcmp(arg, "markdown"))
		curp->outtype = OUTT_MARKDOWN;
	else if (0 == strcmp(arg, "utf8"))
		curp->outtype = OUTT_UTF8;
	else if (0 == strcmp(arg, "locale"))
		curp->outtype = OUTT_LOCALE;
	else if (0 == strcmp(arg, "ps"))
		curp->outtype = OUTT_PS;
	else if (0 == strcmp(arg, "pdf"))
		curp->outtype = OUTT_PDF;
	else {
		warnx("-T %s: Bad argument", arg);
		return 0;
	}

	return 1;
}

static int
woptions(struct curparse *curp, char *arg)
{
	char		*v, *o;
	const char	*toks[11];

	toks[0] = "stop";
	toks[1] = "all";
	toks[2] = "base";
	toks[3] = "style";
	toks[4] = "warning";
	toks[5] = "error";
	toks[6] = "unsupp";
	toks[7] = "fatal";
	toks[8] = "openbsd";
	toks[9] = "netbsd";
	toks[10] = NULL;

	while (*arg) {
		o = arg;
		switch (getsubopt(&arg, (char * const *)toks, &v)) {
		case 0:
			curp->wstop = 1;
			break;
		case 1:
		case 2:
			mandoc_msg_setmin(MANDOCERR_BASE);
			break;
		case 3:
			mandoc_msg_setmin(MANDOCERR_STYLE);
			break;
		case 4:
			mandoc_msg_setmin(MANDOCERR_WARNING);
			break;
		case 5:
			mandoc_msg_setmin(MANDOCERR_ERROR);
			break;
		case 6:
			mandoc_msg_setmin(MANDOCERR_UNSUPP);
			break;
		case 7:
			mandoc_msg_setmin(MANDOCERR_MAX);
			break;
		case 8:
			mandoc_msg_setmin(MANDOCERR_BASE);
			curp->os_e = MANDOC_OS_OPENBSD;
			break;
		case 9:
			mandoc_msg_setmin(MANDOCERR_BASE);
			curp->os_e = MANDOC_OS_NETBSD;
			break;
		default:
			warnx("-W %s: Bad argument", o);
			return 0;
		}
	}
	return 1;
}

static pid_t
spawn_pager(struct tag_files *tag_files)
{
	const struct timespec timeout = { 0, 100000000 };  /* 0.1s */
#define MAX_PAGER_ARGS 16
	char		*argv[MAX_PAGER_ARGS];
	const char	*pager;
	char		*cp;
	size_t		 cmdlen;
	int		 argc, use_ofn;
	pid_t		 pager_pid;

	pager = getenv("MANPAGER");
	if (pager == NULL || *pager == '\0')
		pager = getenv("PAGER");
	if (pager == NULL || *pager == '\0')
		pager = "more -s";
	cp = mandoc_strdup(pager);

	/*
	 * Parse the pager command into words.
	 * Intentionally do not do anything fancy here.
	 */

	argc = 0;
	while (argc + 5 < MAX_PAGER_ARGS) {
		argv[argc++] = cp;
		cp = strchr(cp, ' ');
		if (cp == NULL)
			break;
		*cp++ = '\0';
		while (*cp == ' ')
			cp++;
		if (*cp == '\0')
			break;
	}

	/* For more(1) and less(1), use the tag file. */

	use_ofn = 1;
	if ((cmdlen = strlen(argv[0])) >= 4) {
		cp = argv[0] + cmdlen - 4;
		if (strcmp(cp, "less") == 0 || strcmp(cp, "more") == 0) {
			argv[argc++] = mandoc_strdup("-T");
			argv[argc++] = tag_files->tfn;
			if (tag_files->tagname != NULL) {
				argv[argc++] = mandoc_strdup("-t");
				argv[argc++] = tag_files->tagname;
				use_ofn = 0;
			}
		}
	}
	if (use_ofn)
		argv[argc++] = tag_files->ofn;
	argv[argc] = NULL;

	switch (pager_pid = fork()) {
	case -1:
		err((int)MANDOCLEVEL_SYSERR, "fork");
	case 0:
		break;
	default:
		(void)setpgid(pager_pid, 0);
		(void)tcsetpgrp(tag_files->ofd, pager_pid);
		if (pledge("stdio rpath tmppath tty proc", NULL) == -1)
			err((int)MANDOCLEVEL_SYSERR, "pledge");
		tag_files->pager_pid = pager_pid;
		return pager_pid;
	}

	/* The child process becomes the pager. */

	if (dup2(tag_files->ofd, STDOUT_FILENO) == -1)
		err((int)MANDOCLEVEL_SYSERR, "pager stdout");
	close(tag_files->ofd);
	assert(tag_files->tfd == -1);

	/* Do not start the pager before controlling the terminal. */

	while (tcgetpgrp(STDOUT_FILENO) != getpid())
		nanosleep(&timeout, NULL);

	execvp(argv[0], argv);
	err((int)MANDOCLEVEL_SYSERR, "exec %s", argv[0]);
}
