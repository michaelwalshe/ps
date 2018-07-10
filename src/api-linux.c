
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <Rinternals.h>

#include "common.h"

double psll_linux_boot_time = 0;
double psll_linux_clock_ticks = 0;

typedef struct {
  char state;
  int ppid, pgrp, session, tty_nr, tpgid;
  unsigned int flags;
  unsigned long minflt, cminflt, majflt, cmajflt, utime, stime;
  long int cutime, cstime, priority, nice, num_threads, itrealvalue;
  unsigned long long starttime;
} psl_stat_t;

#define PS__CHECK_STAT(stat, handle)					\
  if (psll_linux_boot_time + (stat).starttime / psll_linux_clock_ticks != \
      handle->create_time) {						\
    ps__no_such_process(handle->pid, 0);				\
    ps__throw_error();							\
  }

#define PS__CHECK_HANDLE(handle)			\
  do {							\
    psl_stat_t stat;					\
    if (psll__parse_stat_file(handle->pid, &stat, 0)) {	\
      ps__wrap_linux_error(handle);			\
      ps__throw_error();				\
    }							\
    PS__CHECK_STAT(stat, handle);			\
  } while (0)

#define PS__GET_STATUS(stat, result, error)		\
  switch(stat) {					\
  case 'R': result = mkString("running");      break;	\
  case 'S': result = mkString("sleeping");     break;	\
  case 'D': result = mkString("disk_sleep");   break;	\
  case 'T': result = mkString("stopped");      break;	\
  case 't': result = mkString("tracing_stop"); break;	\
  case 'Z': result = mkString("zombie");       break;	\
  case 'X': result = mkString("dead");         break;	\
  case 'x': result = mkString("dead");         break;	\
  case 'K': result = mkString("wake_kill");    break;	\
  case 'W': result = mkString("waking");       break;	\
  default: error;					\
  }

int ps__read_file(const char *path, char **buffer, size_t buffer_size);
void *ps__memmem(const void *haystack, size_t n1,
		 const void *needle, size_t n2);

void psll_finalizer(SEXP p) {
  ps_handle_t *handle = R_ExternalPtrAddr(p);
  if (handle) free(handle);
}

void ps__wrap_linux_error(ps_handle_t *handle) {
  char path[512];
  int ret;

  if (errno == ENOENT || errno == ESRCH) {
    /* no such file error; might be raised also if the */
    /* path actually exists for system processes with */
    /* low pids (about 0-20) */
    struct stat st;
    snprintf(path, sizeof(path), "/proc/%i", handle->pid);
    ret = lstat(path, &st);
    if (!ret) {
      /* process exists, other error */
      ps__set_error_from_errno();
    } else if (errno == ENOENT) {
      ps__no_such_process(handle->pid, 0);
    } else if (errno == EPERM || errno == EACCES)  {
      ps__access_denied("");
    } else {
      ps__set_error_from_errno();
    }
  } else {
    ps__set_error_from_errno();
  }

  ps__throw_error();
}

int psll__readlink(const char *path, char **linkname) {
  size_t size = 1024;
  ssize_t r;
  SEXP result;

  *linkname = R_alloc(size, 1);

  while (1) {

    r = readlink(path, *linkname, size - 1);

    if (r == (ssize_t) -1) {
      return -1;

    } else if (r < (ssize_t)1) {
      errno = ENOENT;
      return -1;

    } else if (r < (ssize_t)(size - 1)) {
      break;
    }

    *linkname = S_realloc(*linkname, size + 1024, size, 1);
    size += 1024;
  }

  (*linkname)[r] = '\0';

  /* readlink() might return paths containing null bytes ('\x00')
     resulting in "TypeError: must be encoded string without NULL
     bytes, not str" errors when the string is passed to other
     fs-related functions (os.*, open(), ...).
     Apparently everything after '\x00' is garbage (we can have
     ' (deleted)', 'new' and possibly others), see:
     https://github.com/giampaolo/psutil/issues/717

     For us this is not a problem, because mkString uses the string
     up to the first zero byte, anyway.

     The path might still have a ' (deleted)' suffix, we handle
     this in R. */

  return 0;
}

int psll__parse_stat_file(long pid, psl_stat_t *stat, char **name) {
  char path[512];
  int ret;
  char *buf;
  char *l, *r;

  ret = snprintf(path, sizeof(path), "/proc/%ld/stat", pid);
  if (ret >= sizeof(path)) {
    ps__set_error("Cannot read proc, path buffer too small");
    return -1;
  } else if (ret < 0) {
    return -1;
  }

  ret = ps__read_file(path, &buf, /* buffer= */ 2048);
  if (ret == -1) return -1;

  /* This removes the last character, but that's a \n anyway.
     At least we have a zero terminated string... */
  *(buf + ret - 1) = '\0';

  /* Find the first '(' and last ')', that's the end of the command */
  l = strchr(buf, '(');
  r = strrchr(buf, ')');
  if (!l || !r) {
    ps__set_error("Cannot parse stat file");
    ps__throw_error();
  }

  *r = '\0';
  if (name) *name = l + 1;

  ret = sscanf(r+2,
    "%c %d %d %d %d %d %u %lu %lu %lu %lu %lu %lu %ld %ld %ld %ld %ld %ld %llu",
    &stat->state, &stat->ppid, &stat->pgrp, &stat->session, &stat->tty_nr,
    &stat->tpgid, &stat->flags, &stat->minflt, &stat->cminflt,
    &stat->majflt, &stat->cmajflt, &stat->utime, &stat->stime,
    &stat->cutime, &stat->cstime, &stat->priority, &stat->nice,
    &stat->num_threads, &stat->itrealvalue, &stat->starttime);

  if (ret == -1) {
    return -1;
  } else if (ret != 20) {
    ps__set_error("Cannot parse stat file, parsed: %i/20 fields", ret);
    return -1;
  }

  return 0;
}

void ps__check_for_zombie(ps_handle_t *handle, int err) {
  psl_stat_t stat;

  if (!handle) error("Process pointer cleaned up already");

  if (psll__parse_stat_file(handle->pid, &stat, 0)) {
    ps__wrap_linux_error(handle);
    ps__throw_error();
  }

  if (psll_linux_boot_time + stat.starttime / psll_linux_clock_ticks !=
      handle->create_time) {
    ps__no_such_process(handle->pid, 0);
    err = 1;
  } else if (stat.state == 'Z') {
    ps__zombie_process(handle->pid);
    err = 1;
  } else {
    ps__set_error_from_errno();
  }

  if (err) ps__throw_error();
}

int psll_linux_get_boot_time() {
  int ret;
  char *buf;
  char *needle = "\nbtime ";
  size_t needle_len = strlen(needle);
  char *hit;
  unsigned long btime;

  ret = ps__read_file("/proc/stat", &buf, /* buffer= */ 2048);
  if (ret == -1) return -1;

  *(buf + ret - 1) = '\0';
  hit = ps__memmem(buf, ret, needle, needle_len);
  if (!hit) return -1;

  ret = sscanf(hit + needle_len, "%lu", &btime);
  if (ret != 1) return -1;
  psll_linux_boot_time = (double) btime;
  return 0;
}

int psll_linux_get_clock_ticks() {
  psll_linux_clock_ticks = sysconf(_SC_CLK_TCK);
  return 0;
}

int psll_linux_ctime(long pid, double *ctime) {
  psl_stat_t stat;
  int ret = psll__parse_stat_file(pid, &stat, 0);
  if (ret) return ret;

  if (!psll_linux_boot_time) {
    ret = psll_linux_get_boot_time();
    if (ret) return ret;
  }

  if (!psll_linux_clock_ticks) {
    ret = psll_linux_get_clock_ticks();
    if (ret) return ret;
  }

  *ctime = psll_linux_boot_time + stat.starttime / psll_linux_clock_ticks;

  return 0;
}

SEXP psll_handle(SEXP pid, SEXP time) {
  pid_t cpid = isNull(pid) ? getpid() : INTEGER(pid)[0];
  double ctime;
  ps_handle_t *handle;
  SEXP res;

  if (!isNull(time))  {
    ctime = REAL(time)[0];
  } else {
    if (psll_linux_ctime(cpid, &ctime)) ps__throw_error();
  }

  handle = malloc(sizeof(ps_handle_t));

  if (!handle) {
    ps__no_memory("");
    ps__throw_error();
  }

  handle->pid = cpid;
  handle->create_time = ctime;
  handle->gone = 0;

  PROTECT(res = R_MakeExternalPtr(handle, R_NilValue, R_NilValue));
  R_RegisterCFinalizerEx(res, psll_finalizer, /* onexit */ 0);
  setAttrib(res, R_ClassSymbol, mkString("ps_handle"));

  UNPROTECT(1);
  return res;
}

SEXP psll_format(SEXP p) {
  ps_handle_t *handle = R_ExternalPtrAddr(p);
  psl_stat_t stat;
  SEXP name, status, result;
  char *cname;

  if (!handle) error("Process pointer cleaned up already");

  if (psll__parse_stat_file(handle->pid, &stat, &cname)) {
    PROTECT(name = mkString("???"));
    PROTECT(status = mkString("terminated"));
  } else {
    PROTECT(name = ps__str_to_utf8(cname));
    PS__GET_STATUS(stat.state, status, status = mkString("unknown"));
    PROTECT(status);
  }

  PROTECT(result = ps__build_list("OldO", name, (long) handle->pid,
				  handle->create_time, status));

  /* We do not check that the pid is still valid here, because we want
     to be able to format & print processes that have finished already. */

  UNPROTECT(3);
  return result;
}

SEXP psll_parent(SEXP p) {
  ps_handle_t *handle = R_ExternalPtrAddr(p);
  psl_stat_t stat;
  SEXP ppid, parent;

  if (!handle) error("Process pointer cleaned up already");

  if (psll__parse_stat_file(handle->pid, &stat, 0)) {
    ps__wrap_linux_error(handle);
    ps__throw_error();
  }
  PS__CHECK_STAT(stat, handle);

  /* TODO: this is a race condition, because the parent process might
     have just quit, so psll_handle() might fail. If this happens, then
     we should try to query the ppid again. */

  PROTECT(ppid = ScalarInteger(stat.ppid));
  PROTECT(parent = psll_handle(ppid, R_NilValue));

  UNPROTECT(2);
  return parent;
}

SEXP psll_ppid(SEXP p) {
  ps_handle_t *handle = R_ExternalPtrAddr(p);
  psl_stat_t stat;

  if (!handle) error("Process pointer cleaned up already");

  if (psll__parse_stat_file(handle->pid, &stat, 0)) {
    ps__wrap_linux_error(handle);
    ps__throw_error();
  }
  PS__CHECK_STAT(stat, handle);

  return ScalarInteger(stat.ppid);
}

SEXP psll_is_running(SEXP p) {
  ps_handle_t *handle = R_ExternalPtrAddr(p);
  double ctime;
  int ret;

  if (!handle) error("Process pointer cleaned up already");

  ret = psll_linux_ctime(handle->pid, &ctime);
  if (ret) return ScalarLogical(0);

  return ScalarLogical(ctime == handle->create_time);
}

SEXP psll_name(SEXP p) {
  ps_handle_t *handle = R_ExternalPtrAddr(p);
  psl_stat_t stat;
  char *name;

  if (!handle) error("Process pointer cleaned up already");

  if (psll__parse_stat_file(handle->pid, &stat, &name)) {
    ps__wrap_linux_error(handle);
    ps__throw_error();
  }
  PS__CHECK_STAT(stat, handle);

  return ps__str_to_utf8(name);
}

SEXP psll_exe(SEXP p) {
  ps_handle_t *handle = R_ExternalPtrAddr(p);
  char path[512];
  int ret;
  char *buf;
  char *linkname;
  SEXP result;

  if (!handle) error("Process pointer cleaned up already");

  ret = snprintf(path, sizeof(path), "/proc/%i/exe", handle->pid);
  if (ret < 0) ps__throw_error();

  ret = psll__readlink(path, &linkname);
  if (ret) {
    if (errno == ENOENT || errno == ESRCH) {
      /* no such file error; might be raised also if the */
      /* path actually exists for system processes with */
      /* low pids (about 0-20) */
      struct stat st;
      snprintf(path, sizeof(path), "/proc/%i", handle->pid);
      ret = lstat(path, &st);
      if (!ret) {
	/* process exists, but can't get exe */
	result = ScalarString(NA_STRING);
      } else if (errno == ENOENT) {
	ps__no_such_process(handle->pid, 0);
	ps__throw_error();
      } else {
	/* some other error, anything is possible here... */
	result = ScalarString(NA_STRING);
      }
    } else if (errno == EPERM || errno == EACCES)  {
      ps__access_denied("");
      ps__throw_error();
    } else {
      ps__set_error_from_errno();
      ps__throw_error();
    }
    ps__check_for_zombie(handle, 1);

  } else {
    result = ps__str_to_utf8(linkname);
  }

  return result;
}

SEXP psll_cmdline(SEXP p) {
  ps_handle_t *handle = R_ExternalPtrAddr(p);
  char path[512];
  int ret;
  char *buf, *ptr, *end, *prev;
  char sep = '\0';
  int nstr = 0;
  SEXP result;

  if (!handle) error("Process pointer cleaned up already");

  ret = snprintf(path, sizeof(path), "/proc/%d/cmdline", handle->pid);
  if (ret >= sizeof(path)) {
    ps__set_error("Cannot read proc, path buffer too small");
    ps__throw_error();
  } else if (ret < 0) {
    ps__set_error_from_errno();
    ps__throw_error();
  }

  ret = ps__read_file(path, &buf, 1024);
  if (ret <= 0) {
    ps__check_for_zombie(handle, 1);
  }

  PS__CHECK_HANDLE(handle);

  /* 'man proc' states that args are separated by null bytes '\0' */
  /* and last char is supposed to be a null byte. Nevertheless */
  /* some processes may change their cmdline after being started */
  /* (via setproctitle() or similar), they are usually not */
  /* compliant with this rule and use spaces instead. Google */
  /* Chrome process is an example. See: */
  /* https://github.com/giampaolo/psutil/issues/1179 */

  if (buf[ret - 1] != '\0') sep = ' ';

  /* Count number of vars first, then convert to strings */
  for (ptr = buf, end = buf + ret; ptr < end; ptr++) {
    if (*ptr == sep) nstr++;
  }

  PROTECT(result = allocVector(STRSXP, nstr));
  for (ptr = prev = buf, nstr = 0; ptr < end; ptr++) {
    if (!*ptr) {
      SET_STRING_ELT(result, nstr++, mkCharLen(prev, ptr - prev));
      prev = ptr + 1;
    }
  }

  UNPROTECT(1);
  return result;
}

SEXP psll_status(SEXP p) {
  ps_handle_t *handle = R_ExternalPtrAddr(p);
  psl_stat_t stat;
  SEXP result;

  if (!handle) error("Process pointer cleaned up already");

  if (psll__parse_stat_file(handle->pid, &stat, 0)) {
    ps__wrap_linux_error(handle);
    ps__throw_error();
  }

  PS__CHECK_STAT(stat, handle);

  PS__GET_STATUS(stat.state, result, error("Unknown process status"));

  return  result;
}

SEXP psll_username(SEXP p) {
  SEXP ids, ruid, pw, result;

  PROTECT(ids = psll_uids(p));
  PROTECT(ruid = ScalarInteger(INTEGER(ids)[0]));
  PROTECT(pw = psp__get_pw_uid(ruid));
  PROTECT(result = VECTOR_ELT(pw, 0));

  UNPROTECT(4);
  return result;
}

SEXP psll_cwd(SEXP p) {
  ps_handle_t *handle = R_ExternalPtrAddr(p);
  char path[512];
  int ret;
  char *linkname;

  if (!handle) error("Process pointer cleaned up already");

  ret = snprintf(path, sizeof(path), "/proc/%d/cwd", handle->pid);
  if (ret >= sizeof(path)) {
    ps__set_error("Cannot read proc, path buffer too small");
    ps__throw_error();
  } else if (ret < 0) {
    ps__set_error_from_errno();
    ps__throw_error();
  }

  ret = psll__readlink(path, &linkname);
  if (ret) {
    ps__check_for_zombie(handle, 1);
  }

  PS__CHECK_HANDLE(handle);

  return ps__str_to_utf8(linkname);
}

SEXP psll__ids(SEXP p, const char *needle) {
  ps_handle_t *handle = R_ExternalPtrAddr(p);
  char path[512];
  int ret;
  char *buf;
  size_t needle_len  = strlen(needle);
  char *hit;
  unsigned long real, eff, saved;
  SEXP result, names;

  if (!handle) error("Process pointer cleaned up already");

  ret = snprintf(path, sizeof(path), "/proc/%i/status", handle->pid);
  if (ret >= sizeof(path)) {
    ps__set_error("Cannot read proc, path buffer too small");
    ps__throw_error();
  } else if (ret < 0) {
    ps__set_error_from_errno();
    ps__throw_error();
  }

  ret = ps__read_file(path, &buf, /* buffer= */ 2048);
  if (ret == -1) ps__check_for_zombie(handle, 1);

  *(buf + ret - 1) = '\0';
  hit = ps__memmem(buf, ret, needle, needle_len);
  if (!hit) {
    ps__set_error("Cannot read process status file");
    ps__throw_error();
  }

  ret = sscanf(hit + needle_len, " %lu %lu %lu", &real, &eff, &saved);
  if (ret != 3) {
    ps__set_error("Cannot read process status file");
    ps__throw_error();
  }

  PS__CHECK_HANDLE(handle);

  PROTECT(result = allocVector(INTSXP, 3));
  INTEGER(result)[0] = real;
  INTEGER(result)[1] = eff;
  INTEGER(result)[2] = saved;
  PROTECT(names = ps__build_string("real", "effective", "saved", NULL));
  setAttrib(result, R_NamesSymbol, names);

  UNPROTECT(2);
  return result;
}

SEXP psll_uids(SEXP p) {
  return psll__ids(p, "\nUid:");
}

SEXP psll_gids(SEXP p) {
  return psll__ids(p, "\nGid:");
}

SEXP psll_terminal(SEXP p) {
  ps_handle_t *handle = R_ExternalPtrAddr(p);
  psl_stat_t stat;

  if (!handle) error("Process pointer cleaned up already");

  if (psll__parse_stat_file(handle->pid, &stat, 0)) {
    ps__wrap_linux_error(handle);
    ps__throw_error();
  }
  PS__CHECK_STAT(stat, handle);

  if (stat.tty_nr == 0) {
    return ScalarInteger(NA_INTEGER);
  } else {
    return ScalarInteger(stat.tty_nr);
  }
}

SEXP psll_environ(SEXP p) {
  ps_handle_t *handle = R_ExternalPtrAddr(p);
  char path[512];
  int ret;
  char *buf, *ptr, *end, *prev;
  SEXP result = R_NilValue;
  int nstr = 0;

  if (!handle) error("Process pointer cleaned up already");

  ret = snprintf(path, sizeof(path), "/proc/%d/environ", handle->pid);
  if (ret >= sizeof(path)) {
    ps__set_error("Cannot read proc, path buffer too small");
    ps__throw_error();
  } else if (ret < 0) {
    ps__set_error_from_errno();
    ps__throw_error();
  }

  ret = ps__read_file(path, &buf, /* buffer= */ 1024 * 32);
  if (ret <= 0) {
    ps__check_for_zombie(handle, 1);
  }

  *(buf + ret - 1) = '\0';

  /* Count number of vars first, then convert to strings */
  for (ptr = buf, end = buf + ret; ptr < end; ptr++) if (!*ptr) nstr++;

  PROTECT(result = allocVector(STRSXP, nstr));
  for (ptr = prev = buf, nstr = 0; ptr < end; ptr++) {
    if (!*ptr) {
      SET_STRING_ELT(result, nstr++, mkCharLen(prev, ptr - prev));
      prev = ptr + 1;
    }
  }

  UNPROTECT(1);
  return result;
}

SEXP psll_num_threads(SEXP p) {
  ps_handle_t *handle = R_ExternalPtrAddr(p);
  psl_stat_t stat;
  int ret;

  if (!handle) error("Process pointer cleaned up already");

  ret = psll__parse_stat_file(handle->pid, &stat, 0);
  ps__check_for_zombie(handle, ret < 0);

  PS__CHECK_STAT(stat, handle);

  return ScalarInteger(stat.num_threads);
}

SEXP psll_cpu_times(SEXP p) {
  ps_handle_t *handle = R_ExternalPtrAddr(p);
  psl_stat_t stat;
  SEXP result, names;
  int ret;

  if (!handle) error("Process pointer cleaned up already");

  ret = psll__parse_stat_file(handle->pid, &stat, 0);
  ps__check_for_zombie(handle, ret < 0);

  PS__CHECK_STAT(stat, handle);

  PROTECT(result = allocVector(REALSXP, 4));
  REAL(result)[0] = stat.utime / psll_linux_clock_ticks;
  REAL(result)[1] = stat.stime / psll_linux_clock_ticks;
  REAL(result)[2] = stat.cutime / psll_linux_clock_ticks;
  REAL(result)[3] = stat.cstime / psll_linux_clock_ticks;
  PROTECT(names = ps__build_string("user", "system", "childen_user",
				   "children_system", NULL));
  setAttrib(result, R_NamesSymbol, names);

  UNPROTECT(2);
  return result;
}

SEXP psll_memory_info(SEXP p) {
  ps_handle_t *handle = R_ExternalPtrAddr(p);
  unsigned long rss, vms, shared, text, lib, data, dirty;
  char path[512];
  char *buf;
  int ret;
  SEXP result, names;

  if (!handle) error("Process pointer cleaned up already");

  ret = snprintf(path, sizeof(path), "/proc/%d/statm", handle->pid);
  if (ret >= sizeof(path)) {
    ps__set_error("Cannot read statm, path buffer too small");
    ps__throw_error();
  } else if (ret < 0) {
    ps__set_error_from_errno();
    ps__throw_error();
  }

  ret = ps__read_file(path, &buf, /* buffer= */ 1024);
  ps__check_for_zombie(handle, ret <= 0);

  *(buf + ret - 1) = '\0';

  ret = sscanf(buf, "%lu %lu %lu %lu %lu %lu %lu", &rss, &vms, &shared,
	       &text, &lib, &data, &dirty);
  if (ret != 7) {
    ps__set_error_from_errno();
    ps__throw_error();
  }

  PS__CHECK_HANDLE(handle);

  PROTECT(result = allocVector(INTSXP, 7));
  INTEGER(result)[0] = rss;
  INTEGER(result)[1] = vms;
  INTEGER(result)[2] = shared;
  INTEGER(result)[3] = text;
  INTEGER(result)[4] = lib;
  INTEGER(result)[5] = data;
  INTEGER(result)[6] = dirty;
  PROTECT(names = ps__build_string("rss", "vms", "shared", "text", "lib",
				   "data", "dirty", NULL));
  setAttrib(result, R_NamesSymbol, names);

  UNPROTECT(2);
  return result;
}
