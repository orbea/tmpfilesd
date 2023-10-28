#define _XOPEN_SOURCE 700

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <getopt.h>
#include <err.h>
#include <dirent.h>
#include <errno.h>
#include <ctype.h>
#include <sys/time.h>
#include <sys/types.h>
#include <pwd.h>
#include <grp.h>
#include <unistd.h>
#include <sys/utsname.h>
#include <glob.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <stdbool.h>
#include <limits.h>

#include "config.h"
#include "util.h"

typedef enum { 
	CREAT_FILE,
	TRUNC_FILE,
	WRITE_ARG,
	MKDIR,
	MKDIR_RMF,
	CREATE_SVOL,
	CREATE_SVOL2,
	CREATE_PIPE,
	CREATE_SYM,
	CREATE_CHAR,
	CREATE_BLK,
	COPY,
	IGN,
	IGNR,
	RM,
	RMRF,
	CHMOD,
	CHMODR,
	CHATTR,
	CHATTRR,
	ACL,
	ACLR,
	ADJUST,
	CREATE_SVOL3,
	LINUXATTR,
	LINUXATTRR 
} actions_t;

#define MAX(a, b) (a < b ? b : a)

#define DEF_FILE (S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH)
#define DEF_FOLD (DEF_FILE|S_IXUSR|S_IXGRP|S_IXOTH)

#define MOD_BOOT_ONLY    (1<<0)
#define MOD_NO_ERR       (1<<1)
#define MOD_NOMATCH_RM   (1<<2)
#define MOD_BASE64       (1<<3)
#define MOD_SERVICE_CRED (1<<4)
#define MOD_PLUS         (1<<5)

/* long_opt values */
static int do_create=0, do_clean=0, do_remove=0, do_boot=0;
static int do_help=0, do_version=0, debug=0, debug_unlink=0; 

static char *opt_prefix = NULL, *opt_exclude = NULL, *opt_root = NULL;
static char **config_files = NULL;
static int num_config_files = 0;

/* cache responses to varies system lookups in these */
static char *hostname = NULL;
static char *machineid = NULL;
static char *kernelrel = NULL;
static char *bootid = NULL;

typedef struct ignent {
	char path[PATH_MAX];
	size_t length;
	bool contents;
} ignent_t;

static ignent_t *ignores = NULL;
static int ignores_size = 0;

static void show_version(void)
{
	printf("tmpfilesd %s\n", VERSION);
}

static void show_help(void)
{
	printf(
			"Usage: tmpfilesd [OPTIONS]... [CONFIGURATION FILE]...\n"
			"Manage tmpfiles entries\n\n"
			"  -h, --help                 show help\n"
			"      --version              show version number\n"
			"      --create               create or write to files\n"
			"      --clean                clean up files or folders\n"
			"      --remove               remove directories or filse\n"
			"      --boot                 also execute lines with a !\n"
			"      --prefix=PATH          only apply rules with a matching path\n"
			"      --exclude-prefix=PATH  ignores rules with paths that match\n"
			"      --root=ROOT            all paths including config will be prefixed\n"
			"\n"
		  );
}

__attribute__((nonnull))
static int validate_type(const char *raw, char *type)
{
	const char *tmp;
	int ret;

	ret = 0;

	*type = raw[0];

	for (tmp = (raw + 1); *tmp; tmp++)
		switch (*tmp)
		{
			case '+': ret |= MOD_PLUS;       break;
			case '~': ret |= MOD_BASE64;     break;
			case '-': ret |= MOD_NO_ERR;     break;
			case '!': ret |= MOD_BOOT_ONLY;  break;
			case '=': ret |= MOD_NOMATCH_RM; break;

			case '^': 
			default:
				warnx("type modifier '%c' is unsupported", isprint(*tmp) ? *tmp : '?');
				return -1;
		}

	return ret;
}

/*
 * If omitted or - use 0 unless z/Z then leave UID alone
 */
__attribute__((nonnull, warn_unused_result))
static uid_t vet_uid(const char **t, uid_t *defuid)
{
	if (!*t || **t == '-') {
		*defuid = 1;
		return (uid_t)-1;
	}

	*defuid = 0;

	if (isnumber(*t))
		return atol(*t);

	struct passwd *pw;

	if ( (pw = getpwnam(*t)) == NULL) {
		warn("getpwnam");
		return -1;
	}

	return pw->pw_uid;
}

/*
 * If omitted or - use 0 unless z/Z then leave GID alone
 */
__attribute__((nonnull, warn_unused_result))
static gid_t vet_gid(const char **t, gid_t *defgid)
{
	if (!*t || **t == '-') {
		*defgid = 1;
		return (gid_t)-1;
	}

	*defgid = 0;

	if (isnumber(*t))
		return atol(*t);

	struct group *gr;

	if ( (gr = getgrnam(*t)) == NULL) {
		warn("getgrnam");
		return -1;
	}

	return gr->gr_gid;
}


static const char *getbootid(void)
{
	if (bootid)
		return bootid;

	FILE *fp = NULL;
	size_t ign = 0;

	if ( (fp = fopen("/proc/sys/kernel/random/boot_id", "r")) == NULL ) {
		warn("fopen");
		return NULL;
	}

	if ( getline(&bootid, &ign, fp) < 36 ) {
		if (bootid) {
			free(bootid);
			bootid = NULL;
		}
		warnx("getline");
	}
	bootid = trim(bootid);
	fclose(fp);
	return bootid;
}


static const char *getkernelrelease(void)
{
	if (kernelrel)
		return kernelrel;

	struct utsname *un;

	if ( !(un = calloc(1, sizeof(struct utsname))) ) {
		warn("calloc");
		return NULL;
	}

	if ( uname(un) ) {
		warn("uname");
	} else {
		kernelrel = strdup(un->release);
	}

	free(un);
	return kernelrel;
}

static const char *gethost(void)
{
	if (hostname)
		return hostname;

	if ((hostname = calloc(1, HOST_NAME_MAX + 1)) == NULL)
		return NULL;

	if (gethostname(hostname, HOST_NAME_MAX)) {
		warn("gethostname");
		free(hostname);
		hostname = NULL;
	}

	return hostname;
}

static const char *getmachineid(void)
{
	if (machineid)
		return machineid;

	FILE *fp = NULL;
	size_t ign = 0;

	if ( !(fp = fopen("/etc/machine-id", "r")) ) {
		warn("getmachineid");
		return NULL;
	}

	if ( getline(&machineid, &ign, fp) < 32 ) {
		if (machineid) {
			free(machineid);
			machineid = NULL;
		}
		warnx("getline");
	}
	machineid = trim(machineid);
	fclose(fp);
	return machineid;
}

// FIXME implement '~'

/* if NULL/- files are 0644 and folders are 0755 except for z/Z where this
 * means mode will not be touched
 *
 * If prefixed with "~" this is masked on the already set bits.
 */
__attribute__((nonnull))
static int vet_mode(const char **t, int *mask, int *defmode)
{
	if (!*t || **t == '-') {
		*defmode = 1;
		return -1;
	}

	*defmode = 0;

	const char *mod = *t;

	if (*mod == '~') {
		*mask = 1;
		mod++;
		return -1;
	} else
		*mask = 0;

	if (!isnumber(mod)) {
		errno = EINVAL;
		warn("vet_mode(%s)",mod);
		return -1;
	}

	// FIXME this is wrong :-(
	return strtol(mod, NULL, 8);
}

/* TODO stop doing free(path) as pointer may be reused by callers */
__attribute__((nonnull))
static char *expand_path(char *path)
{
	const int buf_len = 1024;

	char *buf;
	char *ptr = path;
	const char *cpy;
	char tmp;
	int spos = 0, dpos = 0;

	if (!*path)
		return path;

	if ((buf = calloc(1, buf_len)) == NULL)
		err(EXIT_FAILURE, "expand_path: calloc");

	ptr = path;

	while((tmp = ptr[spos]) && dpos < buf_len)
	{
		if (tmp != '%') {
			buf[dpos++] = ptr[spos++];
			continue;
		}

		tmp = ptr[++spos];
		if (dpos >= buf_len || !tmp) 
			continue;

		switch (tmp)
		{
			case '%':
				buf[dpos++] = ptr[spos];
				break;
			case 'b':
				if ( !(cpy = getbootid()) ) 
					continue;
				strncpy(buf + dpos, cpy, buf_len - dpos);
				dpos += strlen(cpy); 
				break;
			case 'm':
				if ( !(cpy = getmachineid()) ) 
					continue;
				strncpy(buf + dpos, cpy, buf_len - dpos);
				dpos += strlen(cpy);
				break;
			case 'H':
				if ( !(cpy = gethost()) ) 
					continue;
				strncpy(buf + dpos, cpy, buf_len - dpos);
				dpos += strlen(cpy);
				break;
			case 'v':
				if ( !(cpy = getkernelrelease()) ) 
					continue;
				strncpy(buf + dpos, cpy, buf_len - dpos);
				dpos += strlen(cpy);
				break;
			default:
				warnx("Unhandled expansion <%c>\n", isprint(tmp) ? tmp : '?');
				break;
		}

		spos++;
	}

	free(path);
	return buf;
}

/*
 * %m - Machine ID (machine-id(5))
 * %b - Boot ID
 * %H - Host name
 * %v - Kernel release (uname -r)
 * %% - %
 */
__attribute__((nonnull))
static char *vet_path(char *path)
{
	if (strchr(path, '%'))
		path = expand_path(path);

	return path;
}

/*
 * If an integer is given without a unit, s is assumed.
 *
 * When 0, cleaning is unconditional.
 *
 * If the age field starts with a tilde character "~", the clean-up is only 
 * applied to files and directories one level inside the directory specified,
 * but not the files and directories immediately inside it.
 */

__attribute__((nonnull))
static struct timeval *vet_age(const char **t, int *subonly)
{
	if (!*t || **t == '-') {
		errno = EINVAL;
		return NULL;
	}

	uint64_t val;
	int read, ret;
	char *tmp = NULL; 
	const char *src = *t;

	if (*src == '~') {
		*subonly = 1;
		src++;
	} else 
		*subonly = 0;

	read = sscanf(src, "%u%ms", &ret, &tmp);

	if (read == 0 || read > 2) {
		if (tmp) free(tmp);
		warnx("invalid age: %s\n", *t);
		return NULL;
	}

	if ( !tmp || !*tmp )
		val = (uint64_t)ret * 1000000;
	else if ( !strcmp(tmp, "ms") )
		val = (uint64_t)ret * 1000;
	else if ( !strcmp(tmp, "s") )
		val = (uint64_t)ret * 1000000;
	else if ( !strcmp(tmp, "m") || !strcmp(tmp, "min") )
		val = (uint64_t)ret * 1000000 * 60;
	else if ( !strcmp(tmp, "h") )
		val = (uint64_t)ret * 1000000 * 60 * 60;
	else if ( !strcmp(tmp, "d") ) {
		val = (uint64_t)ret * 1000000 * 60 * 60 * 24;
	} else if ( !strcmp(tmp, "w") )
		val = (uint64_t)ret * 1000000 * 60 * 60 * 24 * 7;
	else {
		if (tmp) free(tmp);
		warnx("invalid age: %s\n", *t);
		return NULL;
	}

	struct timeval *tv = calloc(1, sizeof(struct timeval));

	if (!tv) {
		warn("calloc");
		if (tmp) free(tmp);
		return NULL;
	}

	tv->tv_sec = (time_t)(val / 1000000);
	tv->tv_usec = (suseconds_t)(val % 1000000);

	if (tmp)
		free(tmp);

	return(tv);
}

__attribute__((nonnull, warn_unused_result))
static int glob_file(const char *path, char ***matches, size_t *count,
		glob_t **pglob)
{
	int r;

	errno = 0;

	if (*pglob == NULL && (*pglob = calloc(1, sizeof(glob_t))) == NULL) {
		warn("glob_file: calloc");
		return -1;
	}

	if ((r = glob(path, GLOB_NOSORT, NULL, *pglob))) {
		if (r != GLOB_NOMATCH) {
			warnx("glob returned %u", r);
			if (r == GLOB_NOSPACE)
				errno = ENOMEM;
			else if (r == GLOB_ABORTED)
				errno = EIO;
		} else
			errno = ENOENT;
		
		globfree(*pglob);

		*pglob = NULL;
		*matches = NULL;
		*count = 0;
		r = -1;
	} else {
		*matches = (**pglob).gl_pathv;
		*count = (**pglob).gl_pathc;
		r = 0;
	}

	return r;
}

/* a wrapper function around unlink(3) that checks for ignored paths */
__attribute__((nonnull,warn_unused_result))
static int unlink_wrapper(const char *pathname, bool check_ignores)
{
	if (check_ignores) {
		for (int i = 0; i < ignores_size; i++)
			/* where contents is true, we check as a prefix otherwise the entire path */
			if (ignores[i].contents && !strncmp(pathname, ignores[i].path, ignores[i].length))
				return 0;
			else if (!strcmp(pathname, ignores[i].path))
				return 0;
	}

	if (!strcmp("/", pathname) || !strcmp(".", pathname) || !strcmp("..", pathname))
		errx(EXIT_FAILURE, "unlink: attempt to remove protected file");

	if (debug) {
		printf("DEBUG: unlink(%s)\n", pathname);
		if (debug_unlink)
			return 0;
	}

	return unlink(pathname);
}

__attribute__((nonnull,warn_unused_result))
static int rm_if_old(const char *path, const struct timeval *tv, bool check_ignores)
{
	struct stat sb;
	time_t now;

	if (lstat(path, &sb) == -1 ) {
		warn("rm_if_old: lstat(%s)", path);
		return -1;
	}

	now = time(NULL);

#ifdef DEBUG
	printf("%s mtime=%lu now=%lu age=%lu diff=%lu\n",
			path,
			sb.st_mtime,
			time(0),
			tv->tv_sec,
			time(0) - sb.st_mtime);
#endif

	if (S_ISDIR(sb.st_mode)) {
		errno = EISDIR;
		warn("rm_if_old: folder(%s)", path);
		return -1;
	} else if ((now - sb.st_mtime) > tv->tv_sec) {
		return unlink_wrapper(path, check_ignores);
	}

	return 0;
}

__attribute__((nonnull(1), warn_unused_result))
static int rm_rf(const char *path, const struct timeval *tv, 
		bool check_ignores, bool follow_symlinks)
{
	/* protect some obvious errors */
	if (!strcmp("/", path) || !strcmp(".", path) || !strcmp("..", path))
		errx(EXIT_FAILURE, "rm_rf: attempt to remove protected file");

	char *buf = NULL;
	struct stat sb;
	DIR *d;
	struct dirent *ent;
	bool descend;

	if (lstat(path, &sb) == -1) {
		warn("rm_rf: fstat(%s)", path);
		return -1;
	}

	/* if the target is:
	 * a directory: opendir & rm_rf() each entry
	 * a symlink:   rm_rf() the symlink
	 * otherwise:   rm_rf() the file
	 */

	if (!S_ISLNK(sb.st_mode) && S_ISDIR(sb.st_mode)) {
		descend = true;
	} else
		descend = false;
	
	if (descend) {
		if ((d = opendir(path)) == NULL) {
			warn("rm_rf: opendir");
			return -1;
		}

		while ( (ent = readdir(d)) )
		{
			if (is_dot(ent->d_name))
				continue;

			if ( (buf = pathcat(path, ent->d_name)) ) 
			{
				if (lstat(buf, &sb) == -1)
					continue;

				if (rm_rf(buf, tv, check_ignores, follow_symlinks))
					warnx("rm_rf: rm_rf(%s)", buf);

				free(buf);
			}
		}

		closedir(d);

		if (errno) 
			return -1;
		return 0;
	} else {
	/* is not a folder */
		return rm_if_old(path, tv, check_ignores);
	}

	/* FIXME check how age checking on symbolic links should be handled */
}

__attribute__((nonnull))
static void process_line(const char *line)
{
	char *rawtype = NULL, *tmppath = NULL, *path = NULL; 
	char *modet = NULL, *dest = NULL;
	char *uidt = NULL, *gidt = NULL, *aget = NULL, *arg = NULL;
	char type;
	int boot_only = 0, subonly = 0;
	int fields = 0, defmode = 0;
	char **globs = NULL;
	size_t nglobs = 0;
	glob_t *fileglob = NULL;
	int fd = -1;
	int ret;
	actions_t act;

	uid_t uid = -1; uid_t defuid = 1;
	gid_t gid = -1; gid_t defgid = 1;
	mode_t mode = 0; int mask = 0;
	dev_t dev = 0;
	int mod = 0;

	struct timeval *age = NULL;

	fields = sscanf(line, 
			"%ms %ms %ms %ms %ms %ms %m[^\n]s",
			&rawtype, &tmppath, &modet, &uidt, &gidt, &aget, &arg);

	if ( fields < 2 ) {
		warnx("bad line: %s\n", line);
		goto cleanup;
	} 

	if ( opt_prefix && strncmp(opt_prefix, tmppath, strlen(opt_prefix)) )
		goto cleanup;

	if ( opt_exclude && !strncmp(opt_exclude, tmppath, strlen(opt_exclude)) )
		goto cleanup;

	if ( (mod = validate_type(rawtype, &type)) == -1 ) {
		warnx("process_line: bad type: %s\n", line);
		goto cleanup;
	} 

	switch(type)
	{
		case 'f':	act = CREAT_FILE;	break;
		case 'F':	act = TRUNC_FILE;	break;
		case 'w':	act = WRITE_ARG;	break;
		case 'd':	act = MKDIR;		break;
		case 'D':	act = MKDIR_RMF;	break;
		case 'e':   act = ADJUST;       break;
		case 'v':	act = CREATE_SVOL;	break;
		case 'q':	act = CREATE_SVOL2;	break;
		case 'Q':	act = CREATE_SVOL3;	break;
		case 'p':	act = CREATE_PIPE;	break;
		case 'L':	act = CREATE_SYM;	break;
		case 'c':	act = CREATE_CHAR;	break;
		case 'b':	act = CREATE_BLK;	break;
		case 'C':	act = COPY;			break;
		case 'x':	act = IGNR;			break;
		case 'X':	act = IGN;			break;
		case 'r':	act = RM;			break;
		case 'R':	act = RMRF;			break;
		case 'z':	act = CHMOD;		break;
		case 'Z':	act = CHMODR;		break;
		case 't':	act = CHATTR;		break;
		case 'T':	act = CHATTRR;		break;
		case 'h':   act = LINUXATTR;    break;
		case 'H':   act = LINUXATTRR;   break;
		case 'a':	act = ACL;			break;
		case 'A':	act = ACLR;			break;

		default:
			warnx("unknown type: <%s>", line);
			goto cleanup;
	}

	path = pathcat(opt_root, tmppath);
	tmppath = NULL;
	free(tmppath);

	if (uidt) uid   = vet_uid((const char **)&uidt, &defuid);
	if (gidt) gid   = vet_gid((const char **)&gidt, &defgid);
	if (modet) mode = vet_mode((const char **)&modet, &mask, &defmode);
	// FIXME handle '~'
	if (aget) age   = vet_age((const char **)&aget, &subonly);
	if (path) path  = vet_path(path);

	if (path == NULL)
		return;

	int i;

	if ( (do_boot && boot_only) || !boot_only ) {

		switch(act)
		{

			/* w - Write the argument parameter to a file
			 *
			 * Argument:
			 * For f, F, and w may be used to specify a short string
			 * that is written to the file, suffixed by a newline
			 */
			case WRITE_ARG:
				if (glob_file(path, &globs, &nglobs, &fileglob)) {
					warn("WRITE_ARG: glob_file");
					break;
				}
				if (do_create || (do_clean && age))
				{
					dest = pathcat(opt_root, arg);
					for (i=0; i<(int)nglobs; i++) {
						/* TODO */
						if (debug) {
							printf("DEBUG: write %s=%s %s%s", path, dest, 
									do_clean ? "clean " : "",
									do_create ? "create " : "");

							if (do_clean && age)
								printf("age=%lu", age->tv_sec);
							puts("\n");
						}
					}
				}
				break;

				/* r - Remove a file or directory if it exists (empty only) [remove]
				 * R - Recursively remove a path and all its subdirectories [remove]
				 *
				 * Mode: ignored
				 * UID, GID: ignored
				 * Age: ignored
				 */
			case RM:
			case RMRF:
				if (!do_remove) 
					break;

				if (glob_file(path, &globs, &nglobs, &fileglob)) {
					if (errno != ENOENT)
						warn("RM: glob_file");
					break;
				}

				for (i = 0; i < (int)nglobs; i++)
				{
					if (act == RMRF) {
						if (rm_rf(globs[i], NULL, false, false))
							warn("RMRF: rmrf(%s)",globs[i]);
					} else
						if (unlink_wrapper(globs[i], false) && errno != ENOENT)
							warn("RM: unlink(%s):", globs[i]);

				}
				break;

				/* x - Ignore a path during cleaning (plus contents)
				 * X - Ignore a path during cleaning (ignores contents)
				 *
				 * Mode: ignored
				 * UID, GID: ignored
				 */
			case IGN:
			case IGNR:
				if (glob_file(path, &globs, &nglobs, &fileglob)) {
					if (errno != ENOENT)
						warn("IGN: glob_file");
					break;
				}

				for (i=0; i<(int)nglobs; i++)
				{
					ignores = realloc( ignores, (sizeof(ignent_t) * (ignores_size+1)) );
					if (!ignores) {
						warn("realloc");
						break;
					}

					strncpy(ignores[ignores_size].path, globs[i], PATH_MAX - 1);
					ignores[ignores_size].contents = (act == IGN) ? true : false;
					ignores[ignores_size].length   = strlen(ignores[ignores_size].path);
					ignores_size++;

					if (debug)
						printf("DEBUG: ignore/r %s\n", globs[i]);
				}
				break;

				/* z - Adjust the access mode, group and user, and restore the 
				 *     SELinux security context (if it exists)
				 * Z - As above, recursively.
				 *
				 * Mode: NULL/- means do not change
				 * UID, GID: NULL/- means do not change
				 */
			case CHMOD:
			case CHMODR:
				if (glob_file(path, &globs, &nglobs, &fileglob) && errno) {
					if (errno != ENOENT)
						warn("CHMOD: glob_file");
					break;
				}

				struct stat sb;

				if (do_create) {
					mode_t mmode = mode;

					for (i=0; i<(int)nglobs; i++) {

						if (defmode) {
							if (stat(globs[i], &sb) == -1)
								warn("stat(%s)", globs[i]);
							else {
								if (S_ISDIR(sb.st_mode)) 
									mmode = DEF_FOLD;
								else
									mmode = DEF_FILE;
							}
						} 

						if (mask) {
							errno = ENOSYS;
							warn("chmod(%s,%s)", globs[i], modet);
						} else {
							if (debug)
								printf("DEBUG: chmod/r %s,%u", globs[i], mmode);

							if (chmod(globs[i], mmode))
								warn("chmod(%s,%s)", globs[i], modet);
						}
						/* FIXME is the logic around -1 right here ? */
						if (lchown(globs[i], defuid ? (uid_t)-1 : uid, 
									defgid ? (gid_t)-1 : gid))
							warn("lchown(%s,%s,%s)", globs[i], uidt, gidt);
					}
				}
				break;

				/* t - Set extended attributes
				 * T - Set extended attributes, recursively
				 *
				 * Mode: ignored
				 * UID, GID: ignored
				 * Age: ignored
				 */
			case CHATTR:
			case CHATTRR:
				if (glob_file(path, &globs, &nglobs, &fileglob)) {
					if (errno != ENOENT)
						warn("CHATTR: glob_file");
					break;
				}

				if (do_create) {
					dest = pathcat(opt_root, arg);
					for (i=0; i<(int)nglobs; i++) {
						/* TODO */
						if (debug)
							printf("DEBUG: chattr/r path=%s dest=%s\n", globs[i], dest);
					}
				}
				break;

				/* a/a+ - Set POSIX ACLs. If suffixed with +, specified entries 
				 *        will be added to the existing set
				 * A/A+ - as above, but recursive.
				 *
				 * Mode: ignored
				 * UID, GID: ignored
				 * Age: ignored
				 */
			case ACL:
			case ACLR:
				if (glob_file(path, &globs, &nglobs, &fileglob)) {
					if (errno != ENOENT)
						warn("ACL: glob_file");
					break;
				}

				if (do_create) {
					dest = pathcat(opt_root, arg);
					for (i=0; i<(int)nglobs; i++) {
						/* TODO */
						if (debug)
							printf("DEBUG: acl/r path=%s dest=%s\n", globs[i], dest);
					}
				}
				break;

				/* v - create subvolume, or behave as d if not supported 
				*/
			case CREATE_SVOL:
				/* TODO */
				break;

				/* d - create a directory (if does not exist)
				 * D - create a direcotry (delete contents if exists) [remove]
				 */
			case MKDIR:
			case MKDIR_RMF:
				if ( (do_clean && age) || (do_remove && act == MKDIR_RMF) ) {
					if (subonly) {
						DIR *dirp = opendir(path);
						struct dirent *dirent;
						char *buf;

						if (!dirp)
							goto mkdir_skip;

						while ( (dirent = readdir(dirp)) != NULL )
						{
							if ( is_dot(dirent->d_name) )
								continue;

							if ( (buf = pathcat(path, dirent->d_name)) )
							{
								if (do_clean && age) {
									if (rm_rf(buf, age, do_clean, true))
										warn("MKDIR: rm_rf(%s)", buf);
								} else if (unlink_wrapper(buf, do_clean) && errno != ENOENT)
									warn("MKDIR: unlink(%s)", buf);
								free(buf);
							}

						}

					} else { /* !subonly */
						if (do_clean && age) {
							/* tmpfiles.d(5) is ambiguous if d/D follow symlinks */
							if (rm_rf(path, age, do_clean, false)) 
								warn("MKDIR: rm_rf(%s)", path);
							else if (debug)
								printf("DEBUG: CLEAN: mkdir/r: %s\n", path);

						} else if (do_remove) {
							if (unlink_wrapper(path, false) && errno != ENOENT) /* FIXME is false correct? */
								warn("MKDIR: unlink(%s)", path);
							else if (debug)
								printf("DEBUG: REMOVE: mkdir/r: %s\n", path);
						}
					}
				}
mkdir_skip:
				if (do_create) {
					/*
					   printf("MKDIR %s %s %s %s %s\n", path, modet, uidt, gidt, 
					   aget);
					   printf("MKDIR %s [%d] %u %u %u\n", path, defmode, 
					   (defmode ? DEF_FOLD : mode), uid, gid);
					   */
					fd = open(path, O_DIRECTORY|O_RDONLY);

					if (fd == -1 && errno != ENOENT) 
						break;
					else if (fd == -1 && errno == ENOENT) {
						/* OK */ 
					} else if (fd != -1 && !(act == MKDIR_RMF)) {
						if (debug)
							printf("DEBUG: SKIP: mkdir/r: %s\n", path);
						break;
					} else if (fd != -1 && rm_rf(path, NULL, false, false))
						warn("rmrf(%s)", path);

					if (fd != -1)
						close(fd);

					if (mkpath(path, (defmode ? DEF_FOLD : mode)) == -1)
						warn("mkpathr(%s)", path);
					else if (chown(path, uid, gid))
						warn("chown(%s)", path);

					if (debug)
						printf("DEBUG: mkdir/r: %s\n", path);
				}

				break;

				/* f - Create a file if it does not exist
				 * F - Create a file, truncate if exists
				 *
				 * Age: ignored
				 * Argument: written to the file, suffixed by \n
				 */
			case CREAT_FILE:
			case TRUNC_FILE:


				if (do_clean && age) {
					if (rm_if_old(path, age, true))
						warn("CREATE/TRUNC_FILE: rm_if_old %s", path);
					else if (debug)
						printf("DEBUG: CLEAN: %s\n", path);
				}

				if (do_remove) {
					if (unlink_wrapper(path, true) && errno != ENOENT)
						warn("CREATE/TRUNC_FILE: unlink_wrapper %s", path);
					else if (debug)
						printf("DEBUG: REMOVE: %s\n", path);
				}

				if (do_create) {
					struct stat sb;
					if (stat(path, &sb) == -1) {
						if (errno != ENOENT) {
							warn("CREATE/TRUNC_FILE: stat %s", path);
							break;
						}
					} else if (act == CREAT_FILE) {
						if (debug)
							printf("DEBUG: SKIP: create/trunc_file: %s\n", path);
						break;
					}

					if ((fd = open(path, 
							O_RDWR|O_CREAT|( (act == TRUNC_FILE) ? O_TRUNC : 0 ),
							(defmode ? DEF_FILE : mode)
							)) == -1) {
						warn("open(%s)", path);
						break;
					}

					if (fchown(fd, uid, gid))
						warn("CREATE/TRUNC_FILE: fchown(%s, %d, %d)", path, uid, gid);

					close(fd);

					if (debug)
						printf("DEBUG: create/trunc_file %s\n", path);

				}
				break;

				/* C - Recursively copy a file or directory, if the destination 
				 *     files or directories do not exist yet
				 *
				 * Argument: specifics the source folder/file. 
				 *           If blank uses /usr/share/factory/$NAME
				 */
			case COPY:
				if (do_create) {
					dest = pathcat(opt_root, arg);
					/* TODO */
					printf("src=%s\n", dest);
				}
				break;

				/* p - Create a pipe (FIFO) if it does not exist
				 * p+ - Remove and create a pipe (FIFO)
				 *
				 * Argument: ignored
				 */
			case CREATE_PIPE:
				if (do_clean && age) {
					/* TODO */
				}

				if (do_create) {
					fd = open(path, O_RDONLY);
					if (fd == -1 && errno != ENOENT) 
						break;
					else if (fd != -1 && (mod & MOD_PLUS) && unlink_wrapper(path, false)) {
						warn("CREATE_PIPE: unlink_wrapper");
						break;
					}

					if (fd != -1)
						close(fd);

					if ( mkfifo(path, mode) ) {
						warn("mkfifo(%s)", path);
						break;
					}
					
					if (chown(path, uid, gid))
						warn("chown(%s)", path);

					if (debug)
						printf("DEBUG: create_pipe %s\n", path);
				}
				break;

				/* L - Create a symlink if it does not exist
				 * L+ - Unlink and then create
				 *
				 * Mode: ignored
				 * UID/GID: ignored
				 * Argument: if empty, symlink to /usr/share/factory/$NAME
				 */
			case CREATE_SYM: // FIXME handle NULL dest => /usr/share/factory
				if (do_clean && age) {
					/* TODO */
				}

				if (do_create) {
					if (strncmp("../", arg, 3) )
						dest = pathcat(opt_root, arg);
					else
						dest = strdup(arg);

					if (dest == NULL) {
						warn("CREATE_SYM: dest");
						break;
					}

					struct stat sb;
					ret = lstat(path, &sb);

					if (ret == -1 && errno != ENOENT) {
						/* failed to stat with a worrying error */
						warn("CREATE_SYM: open");
						break;
					} else if (ret == -1) { 
						/* must be ENOENT, so fine */
					} else if (!S_ISLNK(sb.st_mode) && (mod & MOD_PLUS)) {
						/* if the existing file is NOT a symlink, we have a problem */
						warnx("CREATE_SYM: existing file is not a symlink: %s", path);
						break;
					} else if (!(mod & MOD_PLUS)) {
						/* file exists so ignore */
						if (debug)
							printf("DEBUG: SKIP: symlink dest=%s path=%s\n", dest, path);
						break;
					} else if ((mod & MOD_PLUS) && unlink_wrapper(path, false)) {
						/* file exists, but we had a problem removing it first */
						warn("unlink_wrapper(%s)", path);
						break;
					}

					if ( symlink(dest, path) == -1 ) {
						warn("symlink(%s, %s)", dest, path);
						break;
					}
					
					if (debug)
						printf("DEBUG: symlink dest=%s path=%s\n", dest, path);
				}
				break;

				/* c - Create a character file if it does not exist
				 * c+ - Remove and create a character file
				 *
				 * Argument: ignored
				 */
			case CREATE_CHAR:
				if (do_clean && age) {
					/* TODO */
				}

				if (do_create) {
					struct stat sb;
					ret = stat(path, &sb);

					if (ret == -1 && errno != ENOENT) {
						/* failed to stat with unknown error */
						warn("CREATE_CHAR: lstat");
						break;
					} else if (ret == -1) {
						/* NOENT: OK */
					} else if (ret != -1 && !(mod & MOD_PLUS)) {
						/* file exists, but not c+ */
						if (debug)
							printf("DEBUG: SKIP: create_char %s\n", path);
						break;
					} else if (ret != -1 && (mod & MOD_PLUS) && unlink_wrapper(path, false)) {
						warn("CREATE_CHAR: unlink_wrapper(%s)", path);
						break;
					}

					if ( mknod(path, (defmode ?  DEF_FILE : mode)|S_IFCHR, dev)) {
						warn("mknod(%s)", path);
						break;
					}
					
					if (chown(path, uid, gid))
						warn("chown(%s)", path);

					if (debug)
						printf("DEBUG: create_char %s\n", path);
				}
				break;

				/* b - Create a block device node if it does not exist
				 * b+ - Remove and create
				 *
				 * Argument: ignored
				 */
			case CREATE_BLK:
				if (do_clean && age) {
					/* TODO */
				}

				if (do_create) {
					/* TODO */
					dest = pathcat(opt_root, arg);
				}
				break;
			default:
				break;
		}
	}

cleanup:

	if (fd != -1) 
		close(fd);
	if (rawtype) 
		free(rawtype);
	if (path) 
		free(path);
	if (modet) 
		free(modet);
	if (uidt) 
		free(uidt);
	if (gidt)
		free(gidt);
	if (aget) 
		free(aget);
	if (arg) 
		free(arg);
	if (dest)
		free(dest);
	if (fileglob) 
		globfree(fileglob);
}

__attribute__((nonnull(1)))
static void process_file(const char *file, const char *folder)
{
	char *in = NULL;
	int len = 0;
	char *line = NULL;
	ssize_t cnt = 0;
	size_t ignore = 0;

	//if (file == NULL) {
	//	warnx("file is NULL");
	//	return;
	//}

	if (folder) {
		if ( !(in = calloc(1, (len = strlen(file) + 
							strlen(folder) + 2))) ) {
			warn("calloc");
			return;
		}
		snprintf(in, len, "%s/%s", folder, file);
	} else {
		in = strdup(file);
	}

	//printf("processing %s\n", in);

	if (ignores) {
		ignores_size = 0;
		free(ignores);
		ignores = NULL;
	}

	FILE *fp;

	if ( (fp = fopen(in, "r")) != NULL) {
		while( (cnt = getline(&line, &ignore, fp)) != -1 )
		{
			if (line == NULL) continue;

			line = trim(line);
			if (cnt != 1 && line[0] != '#')
				process_line(line);

			free(line);
			line = NULL;
		}

		fclose(fp);
	} else
		warn("fopen");

	free(in);
}

#define CFG_EXT ".conf"
#define CFG_EXT_LEN sizeof(CFG_EXT)

__attribute__((nonnull))
static void process_folder(const char *folder)
{
	DIR *dirp;
	struct dirent *dirent;
	int len;

	//printf("checking folder: %s\n", folder);
	if ( !(dirp = opendir(folder)) ) {
		warn("opendir(%s)", folder);
		return;
	}

	while( (dirent = readdir(dirp)) )
	{
		if ( is_dot(dirent->d_name) )
			continue;
		if ( (len = strlen(dirent->d_name)) <= (int)CFG_EXT_LEN ) 
			continue;
		if ( strncmp(dirent->d_name + len - CFG_EXT_LEN + 1, CFG_EXT, 
					CFG_EXT_LEN) ) 
			continue;
		process_file(dirent->d_name, folder);
	}
}

#undef CFG_EXT
#undef CFG_EXT_LEN

static const struct option long_options[] = {

	{"create",			no_argument,		&do_create,		true},
	{"clean",			no_argument,		&do_clean,		true},
	{"remove",			no_argument,		&do_remove,		true},
	{"boot",			no_argument,		&do_boot,		true},
	{"prefix",			required_argument,	0,				'p'},
	{"exclude-prefix",	required_argument,	0,				'e'},
	{"root",			required_argument,	0,				'r'},
	{"help",			no_argument,		&do_help,		true},
	{"version",			no_argument,		&do_version,	true},
	{"debug",           no_argument,        &debug,         true},
	{"debug-unlink",    no_argument,        &debug_unlink,  true},

	{0,0,0,0}
};


int main(int argc, char *argv[])
{
	int c, fail = 0;

	while (1)
	{
		int option_index;

		c = getopt_long(argc, argv, "h", long_options, &option_index);

		if (c == -1)
			break;

		switch (c)
		{
			case 'p':
				opt_prefix = strdup(optarg);
				break;
			case 'e':
				opt_exclude = strdup(optarg);
				break;
			case 'r':
				opt_root = strdup(optarg);
				break;
			case 'h':
				do_help = 1;
				break;
			case '?':
				fail = 1;
				break;
			case 0:
				break;
			default:
				break;
		}
	}

	if (optind < argc) {
		if ((config_files = (char **)calloc(argc - optind, sizeof(char *))) == NULL)
			err(EXIT_FAILURE, "main: calloc");

		while (optind < argc)
			config_files[num_config_files++] = strdup(argv[optind++]);
	}

	if (fail) {
		show_help();
		exit(EXIT_FAILURE);
	}

	if (do_help) {
		show_help();
		exit(EXIT_SUCCESS);
	}

	if (do_version) {
		show_version();
		exit(EXIT_SUCCESS);
	}

	if (!opt_root)
		opt_root = "";

#ifdef DEBUG
	printf("tmpfilesd running\ndo_create=%d,do_clean=%d,"
			"do_remove=%d,do_boot=%d\nroot=%s\n",
			do_create, do_clean, do_remove, do_boot,
			root);
#endif

	/* TODO move these to constants somewhere e.g. config.h */
	process_folder(pathcat(opt_root, "/etc/tmpfiles.d"));
	process_folder(pathcat(opt_root, "/run/tmpfiles.d"));
	process_folder(pathcat(opt_root, "/usr/lib/tmpfiles.d"));

	for (int i = 0; i < num_config_files; i++)
		process_file(pathcat(opt_root, config_files[i]), NULL);

	exit(EXIT_SUCCESS);
}
