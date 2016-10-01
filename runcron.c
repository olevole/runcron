// Враппер для запуска крона, пресекающий дубляж и алертящий, если такое происходит
// 1) Указывать путь к лок файлу ненадо, он генерируется на основе argv[]
// и создается в /tmp/<md5>.flock
// 2) Когда лок уже поставлен, делаем нотифи через в syslog
// 3) В конце отработки пишем суммарную инфу о старте-стопе и времени работы (todo: можно rusage впихнуть)
// 4) Если указан -a , запускаем скрипт -a <path> с аргументом кода ошибки
// 5) -c <cwd> - chdir before command execution
// Запуск:
//     runcron <cmd> ...
//     runcron -a action-hook.sh -s nobody@nowhere.here vmstat 10
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sysexits.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <string.h>
#include <openssl/md5.h>
#include <unistd.h>
#include <syslog.h>
#include <limits.h>

#include <sys/types.h>
#include <sys/wait.h>

#define LOCKDIR "/tmp"
#define LOCKFILE_POSTFIX ".flock"

static int acquire_lock(const char *name, int flags);
static void cleanup(void);
static void killed(int sig);
static void usage(void);
static void wait_for_lock(const char *name);

//static const char *lockname;
static int lockfd = -1;
static int keep;
static volatile sig_atomic_t timed_out;

struct timeval before;
struct timeval after;

char *cmd = NULL;
char *md5str = NULL;
char *lockname = NULL;
char *mailto = NULL;
char action_script[PATH_MAX];
char *action = NULL;

int status = 0;

int showtime(FILE *, struct timeval *, struct timeval *);
//static const char default_format[] = "%a %b %d %Y";
static const char default_format[] = "%Y-%m-%d %H:%M:%S";

#define MAX_COMMAND     1000    /* max length of internally generated cmd */
#define MAXHOSTNAMELEN 256
#define MAILCMD "/usr/sbin/sendmail"
#define MAILARGS "%s -FCronDaemon -odi -oem -oi -t"
#define MAILTO "root"
#define MYVERSION "0.9"

int send_lock_alert()
{
	register char   **env;
	auto char       mailcmd[MAX_COMMAND];
	auto char       hostname[MAXHOSTNAMELEN];
	FILE *mail;

	if (gethostname(hostname, MAXHOSTNAMELEN) == -1)
		hostname[0] = '\0';

	hostname[sizeof(hostname) - 1] = '\0';
	(void) snprintf(mailcmd, sizeof(mailcmd), MAILARGS, MAILCMD);

	if (!(mail = popen(mailcmd, "w"))) {
		syslog(LOG_INFO, "Can't popen %s\n", mailcmd);
		return 1;
	}

	syslog(LOG_INFO,"send alert %s",mailcmd);

	fprintf(mail, "From: Cron Daemon <root@%s>\n", hostname);

	fprintf(mail, "To: %s\n", mailto);
	fprintf(mail, "Subject: [Cron] Cron Lock Detected <%s> %s\n",hostname, cmd);

	fprintf(mail, "\n");
	fprintf(mail, "\n");
	fprintf(mail, "Cron lock detected on %s for %s.", hostname, cmd);
	fprintf(mail, "\n");
	pclose(mail);

	return 0;
}

int send_errcode_alert()
{
	register char   **env;
	auto char       mailcmd[MAX_COMMAND];
	auto char       hostname[MAXHOSTNAMELEN];
	FILE *mail;

	if (gethostname(hostname, MAXHOSTNAMELEN) == -1)
		hostname[0] = '\0';

	hostname[sizeof(hostname) - 1] = '\0';
	(void) snprintf(mailcmd, sizeof(mailcmd), MAILARGS, MAILCMD);

	if (!(mail = popen(mailcmd, "w"))) {
		syslog(LOG_INFO, "Can't popen %s\n", mailcmd);
		return 1;
	}

	syslog(LOG_INFO,"send alert %s",mailcmd);

	fprintf(mail, "From: Cron Daemon <root@%s>\n", hostname);

	fprintf(mail, "To: %s\n", mailto);
	fprintf(mail, "Subject: [Cron] Cron Errcode != 0 <%s> %s\n",hostname, cmd);

	fprintf(mail, "\n");
	fprintf(mail, "\n");
	fprintf(mail, "Cron job ended with %d error code on %s for %s.", status, hostname, cmd);
	fprintf(mail, "\n");
	pclose(mail);

	return 0;
}


/*
 * Execute an arbitrary command while holding a file lock.
 */
int
main(int argc, char **argv)
{
	int ch, flags, silent, waitsec, i=0, cmdsize=1;
	pid_t child=1;

	if (argc==1) usage();

	mailto=NULL;
	cmd=NULL;
	action=NULL;

	cmd=malloc(1);

	silent = keep = 0;
	flags = O_CREAT;
	waitsec = -1;	/* Infinite. */

	while ((ch = getopt(argc, argv, "+a:+c:+m:")) != -1) {
		switch (ch) {
			case 'a':
					action = malloc(strlen(optarg) + 1);
					memset(action, 0, strlen(optarg) + 1);
					strcpy(action, optarg);
					break;
			case 'c':
					chdir(optarg);
					break;
			case 'm':
					mailto = malloc(strlen(optarg) + 1);
					memset(mailto, 0, strlen(optarg) + 1);
					strcpy(mailto, optarg);
					break;
			case '?':
			default:
				break;
		}
	}

	//shift $0
	argc -= optind;
	argv += optind;
	bzero(cmd,sizeof(cmd));

	if (!mailto) {
		mailto = malloc(strlen(MAILTO));
		memset(mailto, 0, strlen(MAILTO));
		strcpy(mailto, MAILTO);
	}

	if (child != 0) {
		// this is parent
		(void)gettimeofday(&before, NULL);

		void * tmp = NULL;

		for (i=0; i< argc; i++) {
			cmdsize+=strlen(argv[i])+1;
			tmp = realloc(cmd, cmdsize);
			if ( tmp == NULL)
			{
				err(EX_OSERR,"realloc error");
			}
			else
			{
				cmd = tmp;
			}

			strcat(cmd,argv[i]);
			strcat(cmd," ");
		}
		tmp = realloc(md5str,MD5_DIGEST_LENGTH * 2 + 2); //2 bytes from one MD5_Final + end string

		if ( tmp == NULL)
			{
				err(EX_OSERR,"realloc error");
			}
		else
			{
				md5str = tmp;
		}

		unsigned char out[MD5_DIGEST_LENGTH];
		MD5_CTX c;

		MD5_Init(&c);
		MD5_Update(&c, cmd, cmdsize);
		MD5_Final(out, &c);
		for(i=0; i<MD5_DIGEST_LENGTH; i++) {
			sprintf(md5str+(i*2),"%02x",out[i]);
		}

		lockname=malloc(strlen(md5str)+strlen(LOCKDIR)+strlen(LOCKFILE_POSTFIX)+2); // +2 "/ between dir/file"
		sprintf(lockname,"%s/%s%s",LOCKDIR,md5str,LOCKFILE_POSTFIX);
	}

	lockfd = acquire_lock(lockname, flags | O_NONBLOCK);
	while (lockfd == -1 && !timed_out && waitsec != 0) {
		if (keep)
			lockfd = acquire_lock(lockname, flags);
		else {
			wait_for_lock(lockname);
			lockfd = acquire_lock(lockname, flags | O_NONBLOCK);
		}
	}
	if (waitsec > 0)
		alarm(0);
	if (lockfd == -1) {		/* We failed to acquire the lock. */
		syslog(LOG_INFO, "Already locked (%s) %s\n", lockname, cmd);
		send_lock_alert();
		free(cmd);
		free(md5str);
		if (silent)
			exit(EX_TEMPFAIL);
		errx(EX_TEMPFAIL, "%s: already locked", lockname);
	}
	/* At this point, we own the lock. */
	if (atexit(cleanup) == -1)
		err(EX_OSERR, "atexit failed");
	if ((child = fork()) == -1)
		err(EX_OSERR, "cannot fork");

	if (child == 0) {	/* The child process. */
		close(lockfd);
		execvp(argv[0], argv);
		(void)gettimeofday(&after, NULL);
		warn("%s", argv[0]);
		_exit(1);
	}
	/* This is the parent process. */
	signal(SIGINT, SIG_IGN);
	signal(SIGQUIT, SIG_IGN);
	signal(SIGTERM, killed);
	if (waitpid(child, &status, 0) == -1)
		err(EX_OSERR, "waitpid failed");

	status=WEXITSTATUS(status);

	return (WIFEXITED(status) ? WEXITSTATUS(status) : EX_SOFTWARE);
}



/*
 * Try to acquire a lock on the given file, creating the file if
 * necessary.  The flags argument is O_NONBLOCK or 0, depending on
 * whether we should wait for the lock.  Returns an open file descriptor
 * on success, or -1 on failure.
 */
static int
acquire_lock(const char *name, int flags)
{
	int fd;

//	if ((fd = open(name, O_RDONLY|O_EXLOCK|flags, 0666)) == -1) {
	if ((fd = open(name, O_RDONLY|flags, 0666)) == -1) {
		if (errno == EAGAIN || errno == EINTR)
			return (-1);
		err(EX_CANTCREAT, "cannot open %s", name);
	}
	if (flock(fd, LOCK_EX|LOCK_NB) == -1) {
		send_lock_alert();
		syslog(LOG_INFO, "Already locked (%s) %s\n", lockname, cmd);
		err(EX_CANTCREAT,"lock failed");
	}
	return (fd);
}

/*
 * Remove the lock file.
 */
static void
cleanup(void)
{
	(void)gettimeofday(&after, NULL);
	showtime(stdout, &before, &after);

	if (status!=0) send_errcode_alert();

	free(cmd);
	free(md5str);

	if (keep)
		flock(lockfd, LOCK_UN);
	else
		unlink(lockname);
}

/*
 * Signal handler for SIGTERM.  Cleans up the lock file, then re-raises
 * the signal.
 */
static void
killed(int sig)
{

	cleanup();
	signal(sig, SIG_DFL);
	if (kill(getpid(), sig) == -1)
		err(EX_OSERR, "kill failed");
}

static void
usage(void)
{
	fprintf(stderr,"version %s, usage: runcron [-a post-action-file] [-c chdir] [-m mailto] command [arguments]\n",MYVERSION);
	exit(EX_USAGE);
}

/*
 * Wait until it might be possible to acquire a lock on the given file.
 * If the file does not exist, return immediately without creating it.
 */
static void
wait_for_lock(const char *name)
{
	int fd;

	if ((fd = open(name, O_RDONLY, 0666)) == -1) {
		if (errno == ENOENT || errno == EINTR)
			return;
		err(EX_CANTCREAT, "cannot open %s", name);
	}
	if (flock(fd, LOCK_EX|LOCK_NB) == -1) {
		err(EX_CANTCREAT,"lock failed");
	}
	close(fd);
}


int showtime(FILE *out, struct timeval *before, struct timeval *after)
{
	char *bt=ctime(&before->tv_sec);
	char *at=ctime(&after->tv_sec);
	int diff=0;
	int i=0;
	const char *format = default_format;
	struct tm after_dt;
	struct tm before_dt;
	char res1[32];
	char res2[32];

	diff=(int)after->tv_sec - (int)before->tv_sec;

	(void) localtime_r(&after->tv_sec, &after_dt);
	(void) localtime_r(&before->tv_sec, &before_dt);


	if (strftime(res1, sizeof(res1), format, &before_dt) == 0) {
		syslog(LOG_INFO,  "strftime(3): cannot format supplied date/time into buffer of size using: '%s'\n", format);
		return 1;
	}

	if (strftime(res2, sizeof(res2), format, &after_dt) == 0) {
		syslog(LOG_INFO,  "strftime(3): cannot format supplied date/time into buffer of size using: '%s'\n", format);
		return 1;
	}

	syslog(LOG_INFO,"Started: [%s], Ended: [%s], Took: %d sec. MD5: [%s], mailto [%s], status: %d, CMD: %s\n",res1,res2, diff, md5str, mailto, status, cmd);
	fprintf(out,"Started: [%s], Ended: [%s], Took: %d sec. MD5: [%s], mailto [%s], status: %d, CMD: %s\n",res1,res2, diff, md5str, mailto, status, cmd);

	if (action) {
		memset(action_script,0,sizeof(action_script));
		sprintf(action_script,"%s %d",action,status);
		system(action_script);
	}


	return 0;
}
