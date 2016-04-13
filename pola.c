#define _POSIX_C_SOURCE 199309L

#include <asm/errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <glob.h>
#include <libgen.h>
#include <locale.h>
#include <errno.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <wait.h>

#include "config.h"
#include "types.h"
#include "util/util.h"



#define ANSI_COLOR_BRIGHT_RED "\x1b[1;31m"
#define ANSI_COLOR_RED "\x1b[31m"
#define ANSI_COLOR_BRIGHT_GREEN "\x1b[1;32m"
#define ANSI_COLOR_GREEN "\x1b[32m"
#define ANSI_COLOR_BRIGHT_YELLOW "\x1b[1;33m"
#define ANSI_COLOR_YELLOW "\x1b[33m"
#define ANSI_COLOR_BRIGHT_BLUE "\x1b[1;34m"
#define ANSI_COLOR_BLUE "\x1b[34m"
#define ANSI_COLOR_BRIGHT_MAGENTA "\x1b[1;35m"
#define ANSI_COLOR_MAGENTA "\x1b[35m"
#define ANSI_COLOR_BRIGHT_CYAN "\x1b[1;36m"
#define ANSI_COLOR_CYAN "\x1b[36m"
#define ANSI_COLOR_RESET "\x1b[0m"
#define DEFAULT_LOCALE "en_US.UTF-8"


static config_t config;
static app_t current_app;
static char ** sys_argv;

void signal_handlers(int sig);
void read_output(const int fd);
void children_io(int * fds, size_t count);


int pid_alive(const pid_t pid)
{
	int p = kill(pid, 0);

	if (p == 0) {
		return 1;
	}

	if (p == -1 && errno == ESRCH) {
		return 0;
	}

	perror("check pid_alive");
	perror(strerror(errno));
	exit(1);
}


void ensure_locale(char * locale)
{
	char * lc_all = getenv("LC_ALL");
	char * lang = getenv("LANG");
	char * p;

	if (lc_all) {
		setlocale(LC_ALL, lc_all);
		setenv("LC_ALL", lc_all, 1);
		printf("[%s] setlocal to %s\n", sys_argv[0], lc_all);
		p = lc_all;
	}
	else if (lang) {
		setlocale(LC_ALL, lang);
		setenv("LC_ALL", lang, 1);
		printf("[%s] setlocal to %s\n", sys_argv[0], lang);
		p = lang;
	}
	else {
		setlocale(LC_ALL, DEFAULT_LOCALE);
		setenv("LC_ALL", DEFAULT_LOCALE, 1);
		printf("[%s] setlocal to %s\n", sys_argv[0], DEFAULT_LOCALE);
		p = DEFAULT_LOCALE;
	}

	snprintf(locale, strlen(p) + 8, "LC_ALL=%s", p);
}


/*
 * getpwnam, setgid, setuid and set env var array
 */
void switch_user(const char * user, char ** env)
{

	if (user == NULL || !strcmp("", user))
		return;

	struct passwd *p;
	if ((p = getpwnam(user)) == NULL) {
		fprintf(stderr, "cannot find user: %s\n", user);
		exit(1);
	}

	uid_t uid = getuid();

	if (uid == p->pw_uid) return;

	if (uid != 0) {
		fprintf(stderr, "cannot switch user without root");
		exit(1);
	}

	if (setgid(p->pw_gid) == -1) {
		fprintf(stderr, "cannot setgid -> %d\n", p->pw_gid);
		exit(1);
	}
	printf("[%s] setgid to %d\n", sys_argv[0], p->pw_gid);

	if (setuid(p->pw_uid) == -1) {
		fprintf(stderr, "cannot setuid -> %d\n", p->pw_uid);
		exit(1);
	};
	printf("[%s] setuid to %d\n", sys_argv[0], p->pw_uid);

	snprintf(env[0], strlen(p->pw_name) + 6, "USER=%s", p->pw_name);
	snprintf(env[1], strlen(p->pw_dir) + 6, "HOME=%s", p->pw_dir);
	snprintf(env[2], strlen(p->pw_name) + 7, "LOGIN=%s", p->pw_name);
}

void redirect_stdio(const char * out, const char * err) {
	int logfd = open(out, O_WRONLY | O_CREAT | O_APPEND, 0755);
	int error_logfd = -1;
	int nullfd = open("/dev/null", O_RDONLY, 0);

	if (err != NULL && strcmp("", err)) {
		error_logfd = open(err, O_WRONLY | O_CREAT | O_APPEND, 0755);
	}

	if (logfd == -1) {
		perror("redirect stdout open()");
		exit(1);
	}

	if (nullfd == -1) {
		perror("redirect stdin open()");
		exit(1);
	}

	if (err != NULL && strcmp("", err)) {
		close(2);
		dup2(error_logfd, 2);
		close(error_logfd);
	}
	else {
		dup2(logfd, 2);
	}

	close(0);
	dup2(nullfd, 0);
	close(1);
	dup2(logfd, 1);
	close(logfd);
}

void signal_handler(int sig) {
	if (sig == SIGHUP) {
		signal(SIGHUP, signal_handler);
		printf("HUP dectectd!\n");
		redirect_stdio(current_app.out_file, current_app.err_file);
	}
	if (sig == SIGTERM) {
		signal(SIGTERM, signal_handler);
		printf("TERM dectectd!\n");

		pid_t pid = getpid();
		/* stopsig */
		kill(-pid, SIGTERM);
		int status = 0;
		waitpid(0, &status, 0);
		exit(0);
	}
}


void handle_signals() {
	signal(SIGHUP, signal_handler);
	signal(SIGTERM, signal_handler);
}


void clear_signal() {
	signal(SIGHUP, SIG_DFL);
	signal(SIGTERM, SIG_DFL);
}


void run_child(const char * command, const char * user, process_t * p)
{
	int fd[2];
	if (pipe(fd) == -1) {
		perror("run_child pipe()");
		exit(1);
	}

	pid_t pid = fork();

	if (pid == -1) {
		perror("fork error\n");
	}
	else if (pid == 0) {
		/* child */

		clear_signal();

		char locale[32] = {'\0'};

		ensure_locale(locale);

		dup2(fd[1], 1);
		dup2(fd[1], 2);
		close(fd[0]);
		close(fd[1]);

		char * env[5];
		char env_user[128] = {'\0'};
		char env_login[128] = {'\0'};
		char env_home[1024] = {'\0'};
		env[0] = env_user;
		env[1] = env_login;
		env[2] = env_home;
		env[3] = locale;
		env[4] = NULL;

		switch_user(user, env);

		execle("/bin/sh", "sh", "-c", command, (char *) 0, env);
		perror("exec error\n");
		exit(1);
	}

	close(fd[1]);

	int flags = fcntl(fd[0], F_GETFL, 0);
	fcntl(fd[0], F_SETFL, flags | O_NONBLOCK);

	p->fd = fd[0];
	p->pid = pid;
}

void get_pid_filename(app_t app, char * result)
{
	char filename[4096] = {'\0'};
	snprintf(filename, strlen(app.name) + 5, "%s.pid", app.name);
	path_join((char *)POLA_DIR, filename, result);
}

void read_output(int fd)
{
	char buf[8192] = {'\0'};
	int f = 0;

	while (1) {
		f = readline(fd, buf, sizeof(buf));

		switch (f) {
		case 0 :
			/* end */
			return;
			break;
		case -1:
			usleep(1000);
			return;

		default:
			;
			if (strlen(buf) <= 0) return;
			printf("%s", buf);
			break;
		}
	}
}

void children_io(int * fds, size_t count)
{

	/* detect, use epoll */
	fd_set readfds;
	FD_ZERO(&readfds);

	int max_fd = 0;

	for (size_t i = 0; i < count; i++) {
		if (fds[i] > 0) {
			FD_SET(fds[i], &readfds);
		}

		if (fds[i] > max_fd) max_fd = fds[i];
	}

	if (max_fd == 0) return;

	struct timeval timeout = {0, 100};

	int rc = select(
		max_fd + 1,
		&readfds,
		NULL,
		NULL,
		&timeout);

	if (rc == 0) {
		/* timeout */
		return;
	}

	if (rc == -1) {
		perror("children_io");
		return;
	}

	for (size_t i = 0; i < count; i++) {
		if (FD_ISSET(fds[i], &readfds)) {
			read_output(fds[i]);
		}
	}
}


void spawn_missing_children(const app_t app, process_t * children, int * fds)
{
	for (size_t i = 0; i < app.proc_num; i++) {
		if (children[i].pid > 0) continue;
		memset(&children[i], 0, sizeof(process_t));
		run_child(app.command, app.user, &children[i]);
		fds[i] = children[i].fd;
		sleep(app.interval * 1000); /* milliseconds */
		break;
	}
}

void reap_children(const app_t app, process_t * children, int * fds)
{
	int p_status = 0;
	int p = waitpid(0, &p_status, WNOHANG);

	if (p == -1) {
		perror("waitpid()");
	}
	else if (p == 0) {
		sleep(1);
	}
	else {
		for (unsigned int i = 0; i < app.proc_num; i++) {
			if (p != children[i].pid) continue;
			printf("child %d exited\n", p);
			children[i].fd = 0;
			children[i].pid = 0;
			fds[i] = 0;
		}
	}
}


void master_loop(const app_t app)
{

	int fds[app.proc_num];
	process_t children[app.proc_num];

	/* init fds and children*/
	for (unsigned int i = 0; i < app.proc_num; i++) {
		fds[i] = 0;
		memset(&children[i], 0, sizeof(process_t));
		children[i].pid = 0;
	}

	while (1) {
		spawn_missing_children(app, children, fds);
		children_io(fds, app.proc_num);
		reap_children(app, children, fds);
	}
}


void read_config(const char * path)
{
	if (access(path, R_OK) == -1) {
		perror("read_config access()");
		exit(1);
	}

	FILE *fh = fopen(path, "r");

	if (ferror(fh)) {
		perror("read_config fopen()");
		exit(1);
	}

	char * line = NULL;
	char buf[8192] = {'\0'};
	char key[1024] = {'\0'};
	char value[8192] = {'\0'};


	/* default interval */
	config.interval = 100;

	while (fgets(buf, sizeof(buf), fh)) {
		line = trim(buf);
		if (line == NULL || !strcmp("", line))
			continue;

		sscanf(line, "%[^= ] = %[^\n]", key, value);

		if (!strcmp("dir", key)) {
			char *s = trim(value);
			snprintf(config.dir, strlen(s) + 1, "%s", s);
			continue;
		}
		if (!strcmp("log_dir", key)) {
			char *s = trim(value);
			snprintf(config.log_dir, strlen(s) + 1, "%s", s);
			continue;
		}
		if (!strcmp("apps", key)) {
			char *s = trim(value);
			snprintf(config.apps, strlen(s) + 1, "%s", s);
			continue;
		}
		if (!strcmp("interval", key)) {
			unsigned int interval = 100;
			sscanf(value, "%u", &interval);
			config.interval = interval;
		}
	}

	fclose(fh);

	if (!strcmp("", config.dir)) {
		snprintf(config.dir, strlen(POLA_DIR) + 1, "%s", POLA_DIR);
	}
	if (!strcmp("", config.log_dir)) {
		snprintf(
			config.dir, strlen(POLA_LOG_DIR) + 1,
			"%s", POLA_LOG_DIR);
	}
	if (!strcmp("", config.apps)) {
		snprintf(
			config.apps, strlen(POLA_APPS) + 1,
			"%s", POLA_APPS);
	}

}

void read_app_config(const char * path, app_t * app)
{
	char * f = basename((char *) path);
	snprintf(app->name, strlen(f) - 4, "%s", f);

	app->interval = 0;
	app->proc_num = 1;
	snprintf(app->user, 1, "%s", "");

	if (access(path, R_OK) == -1) {
		char err_msg[1024] = {'\0'};
		snprintf(err_msg, strlen(path) + 20,
				 "cannot read config %s", f);
		perror(err_msg);
	}

	FILE *fh = fopen(path, "r");

	char * line = NULL;
	char buf[8192] = {'\0'};
	while (fgets(buf, sizeof(buf), fh)) {
		line = trim(buf);
		if (line == NULL || !strcmp("", line))
			continue;

		char key[1024] = {'\0'};
		char value[8192] = {'\0'};

		sscanf(line, "%[^= ] = %[^\n]", key, value);

		if (!strcmp("command", key)) {
			char *s = trim(value);
			snprintf(app->command, strlen(s) + 1, "%s", s);
			continue;
		}
		if (!strcmp("stdout", key)) {
			char *s = trim(value);
			snprintf(app->out_file, strlen(s) + 1, "%s", s);
			continue;
		}
		if (!strcmp("stderr", key)) {
			char *s = trim(value);
			snprintf(app->err_file, strlen(s) + 1, "%s", s);
			continue;
		}
		if (!strcmp("user", key)) {
			char *s = trim(value);
			snprintf(app->user, strlen(s) + 1, "%s", s);
			continue;
		}
		if (!strcmp("proc_num", key)) {
			unsigned int proc_num = 1;
			sscanf(value, "%u", &proc_num);
			app->proc_num = proc_num;
			continue;
		}
		if (!strcmp("interval", key)) {
			unsigned int interval = 100;
			sscanf(value, "%u", &interval);
			app->interval = interval;
			continue;
		}

	}
	fclose(fh);

	if (!strcmp("", app->out_file)) {
		char filename[2048] = {'\0'};
		snprintf(filename, strlen(app->name) + 5, "%s.out", app->name);
		path_join((char *) POLA_LOG_DIR, filename, app->out_file);
	}

	if (app->interval == 0) {
		app->interval = config.interval;
	}

	/*
	  if (!strcmp("", app->err_file)) {
	  char filename[2048] = {'\0'};
	  snprintf(filename, strlen(app->name) + 5, "%s.err", app->name);
	  path_join((char *) POLA_LOG_DIR, filename, app->err_file);
	  }
	*/
}


pid_t read_pidfile(const char * filename)
{
	off_t size;
	struct stat s;

	if (access(filename, R_OK) < 0) {
		return -1;
	}

	if (stat(filename, &s) < 0) {
		perror("stat()");
		exit(1);
	}

	size = s.st_size;

	int fd = open(filename, O_RDONLY, 0);
	if (fd < 0) {
		perror("read_pidfile open()");
		exit(1);
	}

	char buf[size];
	if (size != read(fd, buf, size)) {
		perror("read()");
		exit(1);
	}

	pid_t pid = 0;
	sscanf(buf, "%d", &pid);
	close(fd);
	return pid;
}


void write_pidfile(const char * filename, pid_t pid)
{
	char buf[32] = {'\0'};
	snprintf(buf, 32, "%d", pid);
	int fd = open(filename, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR);
	if (fd < 0) perror("write_pidfile open()");
	write(fd, buf, 32);
	close(fd);
}


void status(const app_t app)
{
	char pid_fname[4096] = {'\0'};
	get_pid_filename(app, pid_fname);

	int p = access(pid_fname, R_OK);

	if (p == -1 && errno == ENOENT) {
		printf(ANSI_COLOR_BRIGHT_YELLOW "  %-8s "
			   ANSI_COLOR_RESET ": %s\n",
			   "new", app.name);
		return;
	}

	pid_t pid = read_pidfile(pid_fname);

	if (pid > 0 && pid_alive(pid)) {
		printf(ANSI_COLOR_BRIGHT_GREEN
			   "  %-8s"
			   ANSI_COLOR_RESET
			   " : "
			   ANSI_COLOR_BRIGHT_CYAN
			   "%6d"
			   ANSI_COLOR_RESET
			   " : %s\n",
			   "running",
			   pid, app.name);
	}
	else {
		printf(ANSI_COLOR_BRIGHT_RED
			   "  %-8s"
			   ANSI_COLOR_RESET
			   " : "
			   ANSI_COLOR_BRIGHT_CYAN
			   "%6d"
			   ANSI_COLOR_RESET
			   " : %s\n",
			   "stopped", pid, app.name);
	}
}


void run(const app_t app)
{
	printf("[%s] run: %s\n", sys_argv[0], app.command);
	master_loop(app);
}

void run_daemon(app_t app)
{

	if (fork()) return;

	/* TODO: app.directory */
	if (chdir("/") < 0) {
		perror("run_daemon chdir()");
		exit(1);
	}

	umask(0);

	if (fork()) exit(0);

	if (setsid() < 0) {
		perror("run_daemon setsid()");
		exit(1);
	}

	handle_signals();
	redirect_stdio(app.out_file, app.err_file);

	char pid_fname[4096] = {'\0'};
	get_pid_filename(app, pid_fname);
	write_pidfile(pid_fname, getpid());

	setproctitle(sys_argv[0], app.name);

	run(app);
}

void start(const app_t app)
{

	if (access(POLA_DIR, W_OK) == -1) {
		perror("cannot write to POLA_DIR");
		exit(1);
	}

	char pid_fname[4096] = {'\0'};
	get_pid_filename(app, pid_fname);
	pid_t pid = read_pidfile(pid_fname);

	if (pid == -1 || !pid_alive(pid)) {
		current_app = app;
		run_daemon(app);
		usleep(300000);
	}

	printf(ANSI_COLOR_BRIGHT_GREEN "  %-8s"
		   ANSI_COLOR_RESET	" : %s\n",
		   "started", app.name);
}


void start_foreground(const app_t app)
{
	run(app);
}


void stop(const app_t app)
{
	if (access(POLA_DIR, R_OK) == -1) {
		perror("cannot read POLA_DIR");
		exit(1);
	}

	char pid_fname[4096] = {'\0'};
	get_pid_filename(app, pid_fname);
	pid_t pid = read_pidfile(pid_fname);
	int killed = 0;

	if (pid > 0 && pid_alive(pid)) {
		for (int cnt = 0; cnt < 5; cnt++) {
			/* TODO: stopsig */
			kill(pid, SIGTERM);
			usleep(300000);
			if (!pid_alive(pid)) {
				killed = 1;
				break;
			};
		}
	}
	else {
		killed = 1;
	}

	if (killed) {
		printf(ANSI_COLOR_BRIGHT_YELLOW "  %-8s"
			   ANSI_COLOR_RESET " : %s\n",
			   "killed", app.name);
	}
	else {
		printf(ANSI_COLOR_BRIGHT_RED "  %s"
			   ANSI_COLOR_RESET	" : %s\n",
			   "cannot kill in 5 seconds", app.name);
	}
}


void restart(const app_t app)
{
	stop(app);
	start(app);
}

void hup(const app_t app)
{
	char pid_fname[4096] = {'\0'};
	get_pid_filename(app, pid_fname);
	pid_t pid = read_pidfile(pid_fname);

	if (pid > 0 && pid_alive(pid)) {
		kill(pid, SIGHUP);
	}
	printf("  hup signal sent to %s\n", app.name);
}


void tail(const app_t app)
{
	char command[8192] = {'\0'};
	snprintf(
		command,
		strlen(app.out_file) + 31,
		"/usr/bin/tail -n 30 -F %s", app.out_file);

	execle("/bin/sh", "sh", "-c", command, (char *) NULL, NULL);
}


void info(const app_t app)
{
	printf("  name: %s\n", app.name);
	printf("  out_file: %s\n", app.out_file);
	printf("  user: %s\n", app.user);
	printf("  interval: %d\n", app.interval);
}

void help()
{
	printf("\n");
	printf("  %s usage: \n", sys_argv[0]);
	printf("\n");
	printf("    %s [status]\n", sys_argv[0]);
	printf("    %s status [app_name]\n", sys_argv[0]);
	printf("    %s start [app_name]\n", sys_argv[0]);
	printf("    %s stop [app_name]\n", sys_argv[0]);
	printf("    %s restart [app_name]\n", sys_argv[0]);
	printf("    %s hup [app_name]\n", sys_argv[0]);
	printf("    %s info [app_name]\n", sys_argv[0]);
	printf("    %s tail app_name\n", sys_argv[0]);
	printf("    %s help\n", sys_argv[0]);
	printf("\n");

}

void initialize()
{
	memset(&config, 0, sizeof(config));
	read_config(POLA_CONFIG);
}


int main(int argc, const char ** argv)
{
	sys_argv = (char **) argv;
	initproctitle(argc, (char **) argv);

	initialize();

	void (*func)(const app_t) = NULL;

	if (argc == 1) {
		func = &status;
	}

	if (argc >= 2) {

		if (!strcmp("run", argv[1])) {
			if (argc == 2) {
				printf("%s <command>\n", argv[0]);
				exit(1);
			}

			app_t app;
			memset(&app, 0, sizeof(app_t));
			snprintf(app.command, strlen(argv[2]) + 1,
					"%s", argv[2]);
			app.proc_num = 1;
			app.interval = 1;
			run(app);
			exit(1);
		}
		else if (!strcmp("start", argv[1])) {
			func = &start;
		}
		else if (!strcmp("start-foreground", argv[1])) {
			if (argc == 2) {
				printf("%s start-foreground <app>\n",
					   argv[0]);
				exit(1);
			}
			func = &start_foreground;
		}
		else if (!strcmp("status", argv[1])) {
			func = &status;
		}
		else if (!strcmp("stop", argv[1])) {
			func = &stop;
		}
		else if (!strcmp("restart", argv[1])) {
			func = &restart;
		}
		else if (!strcmp("hup", argv[1])) {
			func = &hup;
		}
		else if (!strcmp("tail", argv[1])) {
			if (argc == 2) {
				printf("%s tail <app>\n",
					   argv[0]);
				exit(1);
			}
			func = &tail;
		}
		else if (!strcmp("info", argv[1])) {
			if (argc == 2) {
				printf("%s info <app>\n",
					   argv[0]);
				exit(1);
			}
			func = &info;
		}
		else if (!strcmp("help", argv[1]) || !strcmp("-h", argv[1]) ||
				 !strcmp("--help", argv[1])) {
			help();
			exit(1);
		}

		else {
			fprintf(stderr, "unknown action: %s\n", argv[1]);
			exit(1);
		}
	}

	char * name = "*";

	if (argc >= 3) {
		name = (char *) argv[2];
	}


	printf("\n");

	glob_t result;
	glob(config.apps, 0, NULL, &result);

	for (unsigned int i = 0 ; i < result.gl_pathc; i++) {
		app_t app;
		memset(&app, 0, sizeof(app));
	    read_app_config(result.gl_pathv[i], &app);
		if (strlen(app.name) > 0 &&
			fnmatch(name, app.name, 0) == 0) {
			(*func)(app);
		}
	}
	printf("\n");

	return 0;
}
