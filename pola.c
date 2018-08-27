#define _POSIX_C_SOURCE 199309L

#include <asm/errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <glob.h>
#include <libgen.h>
#include <locale.h>
#include <netdb.h>
#include <netinet/in.h>
#include <errno.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#include <json-c/json.h>

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
#define HEARTBEAT_INTERVAL_DEFAULT 5000000
#define IOLOOP_INTERVAL_DEFAULT 1000
#define HOSTNAME_BUF_SIZE 256

#ifndef VERSION
#define VERSION "missing version"
#endif
#ifndef BUILD_TIME
#define BUILD_TIME "missing time"
#endif


static config_t config;
static app_t current_app;
static char ** sys_argv;
static int sig_handling = 0;
static int metric_running = 0;
static pthread_t metric_p = 0;
static unsigned int daemon_flag = 0;

void signal_handlers(int sig);
void read_output(const int fd);
void children_io(int * fds, size_t count);
void cleanup_metric_flag(void *);
void * metric_thread(void *);
int send_udp_msg(const char *, const int, const char *);
pid_t read_pidfile(const char *);

void * metric_thread(void * args)
{
	app_t * app = (app_t *) args;
	pthread_cleanup_push(cleanup_metric_flag, NULL);
	time_t now;
	char * hostname = (char *)
		malloc((HOSTNAME_BUF_SIZE + 1) * sizeof(char));

	gethostname(hostname, HOSTNAME_BUF_SIZE);

	while(1) {
		now = time(NULL);

		char _[1024] = {'\0'};
		memset(_, 0, sizeof(char));
		struct json_object * jobj;
		jobj = json_object_new_object();

		json_object_object_add(
			jobj, "timestamp",
			json_object_new_int((unsigned long) now));

		json_object_object_add(
			jobj, "hostname",
			json_object_new_string(hostname));

		json_object_object_add(
			jobj, "name",
			json_object_new_string(app->name));

		const char * msg = json_object_to_json_string_ext(
			jobj, JSON_C_TO_STRING_PRETTY);

		send_udp_msg(
			app->heartbeat_host,
			app->heartbeat_port,
			msg);
		json_object_put(jobj);
		usleep(app->heartbeat_interval);
	}

	free(hostname);

	pthread_cleanup_pop(0);
	return NULL;
}

int send_udp_msg(const char* host, const int port, const char* msg)
{
	int s = socket(AF_INET, SOCK_DGRAM, 0);

	int rv = 0;

	if (s < 0) {
		perror("cannot create socket");
		/* do not require closing */
		return -1;
	}

	int reuse = 1;
	if (setsockopt(
			s, SOL_SOCKET, SO_REUSEADDR,
			(const char*)&reuse, sizeof(reuse)) < 0) {
		perror("setsockopt(SO_REUSEADDR) failed");
		rv = -1;
		goto UDP_END;
	}

	struct sockaddr_in myaddr;
	memset((char *)&myaddr, 0, sizeof(myaddr));
	myaddr.sin_family = AF_INET;
	myaddr.sin_addr.s_addr = htonl(INADDR_ANY);
	myaddr.sin_port = htons(0);

	int b = bind(
		s,
		(struct sockaddr *)&myaddr,
		sizeof(myaddr));

	if (b < 0) {
		perror("bind failed");
		rv = -1;
		goto UDP_END;
	}

	struct sockaddr_in servaddr;
	struct hostent *hp;

	memset((char*)&servaddr, 0, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_port = htons(port);

	hp = gethostbyname(host);
	if (!hp) {
		fprintf(stderr, "could not obtain address of %s\n", host);
		rv = -1;
		goto UDP_END;
	}

	memcpy((void *)&servaddr.sin_addr, hp->h_addr_list[0], hp->h_length);

	int f = sendto(
		s,
		msg,
		strlen(msg),
		0,
		(struct sockaddr *)&servaddr,
		sizeof(servaddr));

	if (f < 0) {
		perror("sendto failed");
		rv = -1;
		goto UDP_END;
	}

UDP_END:
	close(s);
	return rv;
}


void cleanup_metric_flag(void * args)
{
	metric_running = 0;
}

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
	int logfd = open(out, O_WRONLY | O_CREAT | O_APPEND, 0644);
	int error_logfd = -1;
	int nullfd = open("/dev/null", O_RDONLY, 0);

	if (err != NULL && strcmp("", err)) {
		error_logfd = open(err, O_WRONLY | O_CREAT | O_APPEND, 0644);
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

	if (sig_handling == 1) {
		// avoid duplicate handling signals
		return;
	}
	sig_handling = 1;

	if (sig == SIGHUP) {
		signal(SIGHUP, signal_handler);
		// printf("[%s] SIGHUP dectectd!\n", sys_argv[0]);
		LOG_TRACE(LOG_INFO, "[%s] SIGHUP dectectd!\n", sys_argv[0]);
		redirect_stdio(current_app.out_file, current_app.err_file);
		sig_handling = 0;
	}
	if (sig == SIGTERM) {
		signal(SIGTERM, signal_handler);
		// printf("[%s] SIGTERM dectectd!\n", sys_argv[0]);
		LOG_TRACE(LOG_INFO, "[%s] SIGTERM dectectd!\n", sys_argv[0]);

		if (current_app.ignore_term > 0) {
			// printf("[%s] ignore SIGTERM\n", sys_argv[0]);
			LOG_TRACE(
				LOG_INFO, "[%s] ignore SIGTERM\n", sys_argv[0]);
			sig_handling = 0;
			return;
		}

		pid_t pid = getpid();
		/* stopsig */
		kill(-pid, SIGTERM);
		int status = 0;
		waitpid(0, &status, 0);
		sig_handling = 0;
		exit(0);
	}
	if (sig == SIGQUIT) {
		signal(SIGTERM, signal_handler);
		// printf("[%s] SIGQUIT dectectd!\n", sys_argv[0]);
		LOG_TRACE(LOG_INFO, "[%s] SIGQUIT dectectd!\n", sys_argv[0]);

		pid_t pid = getpid();
		/* stopsig */
		kill(-pid, SIGTERM);
		int status = 0;
		waitpid(0, &status, 0);
		sig_handling = 0;
		exit(0);
	}
}


void handle_signals() {
	signal(SIGHUP, signal_handler);
	signal(SIGTERM, signal_handler);
	signal(SIGQUIT, signal_handler);
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

		// clear_signal();

		char * locale;
		locale = (char *)malloc((32 + 1) * sizeof(char));
		memset(locale, '\0', 32 + 1);

		ensure_locale(locale);

		dup2(fd[1], 1);
		dup2(fd[1], 2);
		close(fd[0]);
		close(fd[1]);

		char * env[5];
		char * env_user;
		char * env_login;
		char * env_home;
		env_user = (char *)malloc((128 + 1) * sizeof(char));
		memset(env_user, '\0', 128 + 1);
		env_login = (char *)malloc((128 + 1) * sizeof(char));
		memset(env_login, '\0', 128 + 1);
		env_home = (char *)malloc((1024 + 1) * sizeof(char));
		memset(env_home, '\0', 1024 + 1);
		env[0] = env_user;
		env[1] = env_login;
		env[2] = env_home;
		env[3] = locale;
		env[4] = NULL;

		switch_user(user, env);

		printf("command: %s\n", command);

		execle("/bin/sh", "sh", "-c", command, (char *) 0, env);
		perror("exec error\n");
		exit(1);
	}

	// parent

	close(fd[1]);

	int flags = fcntl(fd[0], F_GETFL, 0);
	fcntl(fd[0], F_SETFL, flags | O_NONBLOCK);

	p->fd = fd[0];
	p->pid = pid;

	if (daemon_flag) {
		handle_signals();
	}
}

void get_pid_filename(app_t app, char * result)
{
	int s = 4096;
	char * filename;
	filename = (char *)malloc((s + 1) * sizeof(char));
	memset(filename, '\0', s + 1);
	snprintf(filename, strlen(app.name) + 5, "%s.pid", app.name);
	path_join((char *)POLA_DIR, filename, result);
	free(filename);
}

void read_output(int fd)
{
	int s = 8192;
	char * buf;
	buf = (char *)malloc((s + 1) * sizeof(char));
	memset(buf, '\0', s + 1);
	int f = 0;

	while (1) {
		f = readline(fd, buf, s);

		switch (f) {
		case 0 :
			/* end */
			goto end;
			break;
		case -1:
			usleep(1000);
			goto end;
			break;
		default:
			;
			if (strlen(buf) <= 0) goto end;
			printf("%s", buf);
			break;
		}
	}

end:
	free(buf);
	return;
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
		usleep(app.interval * 1000); /* milliseconds */
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
		usleep(100 * 1000) /* milliseconds */;
	}
	else {
		for (unsigned int i = 0; i < app.proc_num; i++) {
			if (p != children[i].pid) continue;
			printf("child %d exited\n", p);
			close(children[i].fd);
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

void guard_loop(const app_t app)
{
	pid_t prev_pid = 0;
	pid_t pid = 0;
	int err_cnt = 0;
	process_t p;
	int p_status;

	while (1) {
		pid = read_pidfile(app.guard_pidfile);
		if (pid == -1) {
			printf(
				"[%s] cannot read pid file, run command %s\n",
				sys_argv[0],
				app.command);
			run_child(app.command, app.user, &p);
			waitpid(0, &p_status, 0);
			goto NEXT;
		}
		if (pid_alive(pid) == 0) {
			err_cnt++;
			printf(
				"[%s] guard pid %d not alive, cnt %d\n",
				sys_argv[0],
				pid,
				err_cnt);
		}
		if (err_cnt >= 3) {
			err_cnt = 0;
			printf(
				"[%s] guard err count exceeded, run command %s\n",
				sys_argv[0],
				app.command);
			run_child(app.command, app.user, &p);
			waitpid(0, &p_status, 0);
		}

		if (prev_pid == 0) {
			prev_pid = pid;
		}
		else {
			if (pid != prev_pid) {
				printf(
					"[%s] guard pid has changed to %d\n",
					sys_argv[0],
					pid);
				prev_pid = pid;
			}
		}
	NEXT:
		usleep(app.interval * 1000);
	}
}

void read_config(const char * path)
{
	if (access(path, R_OK) == -1) {
		goto pola_config_default;
	}

	FILE *fh = fopen(path, "r");

	if (ferror(fh)) {
		goto pola_config_default;
	}

	char * line = NULL;
	char * buf;
	char * key;
	char * value;
	int line_size = 8192;
	buf = (char *)malloc((line_size + 1) * sizeof(char));
	memset(buf, '\0', line_size + 1);
	key = (char *)malloc((1024 + 1) * sizeof(char));
	memset(key, '\0', 1024 + 1);
	value = (char *)malloc((8192 + 1) * sizeof(char));
	memset(value, '\0', 8192 + 1);

	/* default interval */
	config.interval = 100;

	while (fgets(buf, line_size, fh)) {
		line = trim(buf);
		if (line == NULL || !strcmp("", line))
			continue;

		memset(key, '\0', 1024 + 1);
		memset(value, '\0', 8192 + 1);
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

	free(buf);
	free(key);
	free(value);

	fclose(fh);

pola_config_default:
	/* must not be less than this value */
	if (config.interval < IOLOOP_INTERVAL_DEFAULT) {
		config.interval = IOLOOP_INTERVAL_DEFAULT;
	}

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
	snprintf(app->directory, 2, "%s", "/");
	app->heartbeat = 0;
	app->heartbeat_interval = HEARTBEAT_INTERVAL_DEFAULT;
	snprintf(app->heartbeat_host, 1, "%s", "");
	app->heartbeat_port = 0;
	app->disabled = 0;
	app->guard = 0;
	app->ignore_term = 0;
	snprintf(app->guard_pidfile, 1, "%s", "");
	snprintf(app->guard_pre_start, 1, "%s", "");

	if (access(path, R_OK) == -1) {
		char * err_msg;
		err_msg = (char *)malloc((1024 + 1) * sizeof(char));
		memset(err_msg, '\0', 1024 + 1);
		snprintf(err_msg, strlen(path) + 20,
				 "cannot read config %s", f);
		perror(err_msg);
	}

	FILE *fh = fopen(path, "r");

	char * line = NULL;
	char * buf;
	char * key;
	char * value;
	int line_size = 8192;
	buf = (char *)malloc((line_size + 1) * sizeof(char));
	memset(buf, '\0', line_size + 1);
	key = (char *)malloc((1024 + 1) * sizeof(char));
	memset(key, '\0', 1024 + 1);
	value = (char *)malloc((8192 + 1) * sizeof(char));
	memset(value, '\0', 8192 + 1);

	while (fgets(buf, line_size, fh)) {
		line = trim(buf);
		if (line == NULL || !strcmp("", line))
			continue;

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
		if (!strcmp("heartbeat", key)) {
			char *s = trim(value);
			if (
				!strcmp("yes", s) ||
				!strcmp("true", s) ||
				!strcmp("on", s))
				app->heartbeat = 1;
			continue;
		}
		if (!strcmp("heartbeat_host", key)) {
			char *s = trim(value);
			snprintf(app->heartbeat_host, strlen(s) + 1, "%s", s);
			continue;
		}
		if (!strcmp("heartbeat_port", key)) {
			unsigned int heartbeat_port = 0;
			sscanf(value, "%u", &heartbeat_port);
			app->heartbeat_port = heartbeat_port;
			continue;
		}
		if (!strcmp("heartbeat_interval", key)) {
			unsigned int heartbeat_interval = 0;
			sscanf(value, "%u", &heartbeat_interval);
			app->heartbeat_interval = heartbeat_interval;
			continue;
		}
		if (!strcmp("disabled", key)) {
			char *s = trim(value);
			if (
				!strcmp("yes", s) ||
				!strcmp("true", s) ||
				!strcmp("on", s))
				app->disabled = 1;
			continue;
		}
		if (!strcmp("guard", key)) {
			char *s = trim(value);
			if (
				!strcmp("yes", s) ||
				!strcmp("true", s) ||
				!strcmp("on", s))
				app->guard = 1;
			continue;
		}
		if (!strcmp("guard_pidfile", key)) {
			char *s = trim(value);
			snprintf(app->guard_pidfile, strlen(s) + 1, "%s", s);
			continue;
		}
		if (!strcmp("guard_pre_start", key)) {
			char *s = trim(value);
			snprintf(app->guard_pre_start, strlen(s) + 1, "%s", s);
			continue;
		}
		if (!strcmp("ignore_term", key)) {
			unsigned int ignore_term = 0;
			sscanf(value, "%u", &ignore_term);
			app->ignore_term = ignore_term;
			continue;
		}
		if (!strcmp("directory", key)) {
			char *s = trim(value);
			snprintf(app->directory, strlen(s) + 1, "%s", s);
			continue;
		}
	}
	free(buf);
	free(key);
	free(value);
	fclose(fh);

	if (!strcmp("", app->out_file)) {
		char * filename;
		filename = (char *)malloc((2048 + 1) * sizeof(char));
		memset(filename, '\0', 2048 + 1);
		snprintf(filename, strlen(app->name) + 5, "%s.out", app->name);
		path_join((char *) POLA_LOG_DIR, filename, app->out_file);
		free(filename);
	}

	if (app->interval == 0) {
		app->interval = config.interval;
	}

	if (app->heartbeat_interval == 0) {
		app->heartbeat_interval = HEARTBEAT_INTERVAL_DEFAULT;
	}

}

void touch_pid_file(const char * filename) {
	time_t now = time(NULL);

	struct utimbuf ubuf;

	ubuf.actime = now;
	ubuf.modtime = now;

	utime(filename, &ubuf);
}

void remove_pid_file(const char * filename) {
	int status = remove(filename);
	if (status != 0) {
		perror("cannot remove pid file");
	}
}

int last_mtime(const char * filename, char * p)
{

	struct stat s;
	int status;
	int delta = 0;
	time_t now;
	status = stat(filename, &s);

	if (status > 0) {
		return status;
	}

	now = time(NULL);

	delta = now - s.st_mtime;
	// printf("delta: %d\n", delta);

	if (delta <= 0) {
		return 1;
	}

	if (delta == 1) {
		snprintf(p, 13, "   1 second");
		return 0;
	}

	if (delta < 60) {
		snprintf(p, 13, "%4d seconds", delta);
		return 0;
	}

	if (delta < 120) {
		snprintf(p, 13, "   1 minute");
		return 0;
	}

	if (delta < 3600) {
		snprintf(p, 13, "%4d minutes", delta / 60);
		return 0;
	}

	if (delta < 7200) {
		snprintf(p, 13, "   1 hour");
		return 0;
	}

	if (delta < 86400) {
		snprintf(p, 13, "%4d hours", delta / 3600);
		return 0;
	}

	if (delta < 86400 * 2) {
		snprintf(p, 13, "   1 day");
		return 0;
	}

	if (delta <= 86400 * 14) {
		snprintf(p, 13, "%4d days", delta / 86400);
		return 0;
	}

	if (delta > 86400 * 14) {
		snprintf(p, 13, "%4d weeks", delta / (86400 * 7));
		return 0;
	}

	return 0;
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

	char * buf;
	buf = (char *)malloc((size + 1) * sizeof(char));
	memset(buf, '\0', size + 1);
	if (size != read(fd, buf, size)) {
		perror("read()");
		exit(1);
	}

	pid_t pid = 0;
	sscanf(buf, "%d", &pid);
	close(fd);
	free(buf);
	return pid;
}


void write_pidfile(const char * filename, pid_t pid)
{
	int s = 32;
	char * buf;
	buf = (char *)malloc((s + 1) * sizeof(buf));
	memset(buf, '\0', s + 1);
	snprintf(buf, s, "%d", pid);
	int fd = open(filename, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR);
	if (fd < 0) perror("write_pidfile open()");
	write(fd, buf, s);
	close(fd);
	free(buf);
}


void status(const app_t app)
{
	pid_t pid = 0;
	char * _status =
		ANSI_COLOR_BRIGHT_RED
		"stopped"
		ANSI_COLOR_RESET;

	char * pid_fname;
	pid_fname = (char *)malloc((1024 + 1) * sizeof(char));

	char * mtime_buf;
	mtime_buf = (char *)malloc((12 + 1) * sizeof(char));
	memset(mtime_buf, '\0', 12 + 1);
	get_pid_filename(app, pid_fname);

	int p = access(pid_fname, R_OK);

	if (p == -1 && errno == ENOENT) {
		snprintf(mtime_buf, 4, "N/A");
		goto exit;
	}
	else {
		last_mtime(pid_fname, mtime_buf);
	}

	pid = read_pidfile(pid_fname);

	if (app.disabled) {
		_status = (
			ANSI_COLOR_BRIGHT_YELLOW
			"disabled"
			ANSI_COLOR_RESET
			);
	}
	else if (pid > 0 && pid_alive(pid)) {
		_status = (
			ANSI_COLOR_BRIGHT_GREEN
			"running"
			ANSI_COLOR_RESET
			);
	}

exit:
	printf(
		" %-20s : %12s : "
		ANSI_COLOR_BRIGHT_CYAN
		" %6d : "
		ANSI_COLOR_RESET
		"%s\n",
		_status, mtime_buf, pid, app.name);

	free(mtime_buf);
	free(pid_fname);
}


void run(const app_t app)
{

	/* change working directory */
	if (chdir(app.directory) < 0) {
		perror("run chdir()");
		exit(1);
	}

	if (metric_p) {
		pthread_cancel(metric_p);
		pthread_join(metric_p, NULL);
	}

	if (app.heartbeat)
		pthread_create(&metric_p, NULL, metric_thread, (void *)&app);


	if (app.guard == 1) {
		printf("[%s] guard: %s\n", sys_argv[0], app.guard_pidfile);
		guard_loop(app);
	}
	else {
		printf("[%s] run: %s\n", sys_argv[0], app.command);
		master_loop(app);
	}
}

void run_daemon(app_t app)
{

	if (fork()) return;

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

	// handle_signals();
	redirect_stdio(app.out_file, app.err_file);

	char * pid_fname;
	pid_fname = (char *)malloc((4096 + 1) * sizeof(char));
	memset(pid_fname, '\0', 4096 + 1);
	get_pid_filename(app, pid_fname);
	write_pidfile(pid_fname, getpid());
	free(pid_fname);

	setproctitle(sys_argv[0], app.name);

	run(app);
}

void start(const app_t app)
{

	daemon_flag = 1;

	if (access(POLA_DIR, W_OK) == -1) {
		perror("cannot write to POLA_DIR");
		exit(1);
	}

	if (app.disabled) {
		printf(
			ANSI_COLOR_BRIGHT_RED
			"  disabled, cannot start"
			ANSI_COLOR_RESET
			"\n"
			);
		return;
	}

	char * pid_fname;
	pid_fname = (char *)malloc((4096 + 1) * sizeof(char));
	memset(pid_fname, '\0', 4096 + 1);
	get_pid_filename(app, pid_fname);
	pid_t pid = read_pidfile(pid_fname);
	free(pid_fname);

	if (app.guard == 1) {
		if (strcmp("", app.guard_pidfile) == 0) {
			printf(
				ANSI_COLOR_BRIGHT_RED
				"  missing guard_pidfile config when using guard"
				ANSI_COLOR_RESET
				"\n"
				);
			return;
		}
		current_app = app;
		usleep(300000);
	}

	if (pid == -1 || !pid_alive(pid)) {
		current_app = app;
		run_daemon(app);
		usleep(300000);
	}

	printf(ANSI_COLOR_BRIGHT_GREEN "  %-7s"
		   ANSI_COLOR_RESET	" : %s\n",
		   "started", app.name);
}

/* legacy entry for ``pola run'' */
void start_foreground(const app_t app)
{
	run(app);
}

/* pola run ... function entry */
void cmd_run(const app_t app)
{
	run(app);
}

void stop(const app_t app)
{
	if (access(POLA_DIR, R_OK) == -1) {
		perror("cannot read POLA_DIR");
		exit(1);
	}

	char * pid_fname;
	pid_fname = (char *)malloc((4096 + 1) * sizeof(char));
	memset(pid_fname, '\0', 4096 + 1);
	get_pid_filename(app, pid_fname);
	pid_t pid = read_pidfile(pid_fname);
	int killed = 0;

	if (pid > 0 && pid_alive(pid)) {
		for (int cnt = 0; cnt < 25; cnt++) {
			/* TODO: stopsig */
			int sig = SIGTERM;
			if (app.ignore_term) {
				sig = SIGQUIT;
			}
			kill(pid, sig);
			usleep(200000);
			if (!pid_alive(pid)) {
				killed = 1;
				break;
			};
		}
	}
	else {
		if (app.disabled) {
			printf(
				ANSI_COLOR_BRIGHT_RED
				"  disabled, cannot start"
				ANSI_COLOR_RESET
				"\n"
				);
			return;
		}
		killed = 1;
	}

	if (metric_p) {
		pthread_cancel(metric_p);
		pthread_join(metric_p, NULL);
	}

	if (killed) {
		remove_pid_file(pid_fname);
		printf(ANSI_COLOR_BRIGHT_YELLOW "  %-7s"
			   ANSI_COLOR_RESET " : %s\n",
			   "killed", app.name);
	}
	else {
		printf(ANSI_COLOR_BRIGHT_RED "  %s"
			   ANSI_COLOR_RESET	" : %s\n",
			   "cannot kill in 5 seconds", app.name);
	}

	free(pid_fname);
}

void restart(const app_t app)
{
	if (access(POLA_DIR, W_OK) == -1) {
		perror("cannot write to POLA_DIR");
		exit(1);
	}

	char * pid_fname;
	pid_fname = (char *)malloc((4096 + 1) * sizeof(char));
	memset(pid_fname, '\0', 4096 + 1);
	get_pid_filename(app, pid_fname);
	pid_t pid = read_pidfile(pid_fname);
	free(pid_fname);

	if (pid == -1 || pid_alive(pid)) {
		stop(app);
		start(app);
	}
}


void force_restart(const app_t app)
{
	stop(app);
	start(app);
}

void hup(const app_t app)
{
	char * pid_fname;
	pid_fname = (char *)malloc((4096 + 1) * sizeof(pid_fname));
	memset(pid_fname, '\0', 4096 + 1);
	get_pid_filename(app, pid_fname);
	pid_t pid = read_pidfile(pid_fname);
	free(pid_fname);

	if (pid > 0 && pid_alive(pid)) {
		kill(pid, SIGHUP);
		printf("  hup signal sent to %s\n", app.name);
	}
}


void tail(const app_t app)
{
	char * command;
	command = (char *)malloc((8192 + 1) * sizeof(char));
	memset(command, '\0', 8192 + 1);
	snprintf(
		command,
		strlen(app.out_file) + 31,
		"/usr/bin/tail -n 30 -F %s", app.out_file);

	execle("/bin/sh", "sh", "-c", command, (char *) NULL, NULL);
}


void info(const app_t app)
{
	char * pid_fname;
	char * dt_buf;
	pid_fname = (char *)malloc((4096 + 1) * sizeof(pid_fname));
	memset(pid_fname, '\0', 4096 + 1);
	dt_buf = (char *)malloc((20 + 1) * sizeof(char)) ;
	memset(dt_buf, '\0', 20 +1);
	int status = 0;
	struct stat s;
	time_t t;


	get_pid_filename(app, pid_fname);

	printf("  name: %s\n", app.name);
	printf("  directory: %s\n", app.directory);
	printf("  out_file: %s\n", app.out_file);
	printf("  pid_file: %s\n", pid_fname);
	printf("  user: %s\n", app.user);
	printf("  interval: %d\n", app.interval);
	stat(pid_fname, &s);
	if (status == 0) {
		t = (time_t)s.st_mtime;
		strftime(dt_buf, 20, "%Y-%m-%d %H:%M:%S", localtime(&t));
		printf("  pid_file mtime: %s\n", dt_buf);
	}
	printf("  heartbeat: %d\n", app.heartbeat);
	printf("  heartbeat_host: %s\n", app.heartbeat_host);
	printf("  heartbeat_port: %d\n", app.heartbeat_port);
	printf("  heartbeat_interval: %d\n", app.heartbeat_interval);
	printf("  guard: %d\n", app.guard);
	printf("  guard_pidfile: %s\n", app.guard_pidfile);
	printf("  guard_pre_start: %s\n", app.guard_pre_start);
	printf("  ignore_term: %d\n", app.ignore_term);
	free(pid_fname);
	free(dt_buf);
}

void help()
{
	printf("\n");
	printf("  %s usage: \n", sys_argv[0]);
	printf("\n");
	printf("    %s force-restart [app_name]\n", sys_argv[0]);
	printf("    %s help\n", sys_argv[0]);
	printf("    %s hup [app_name]\n", sys_argv[0]);
	printf("    %s info [app_name]\n", sys_argv[0]);
	printf("    %s restart [app_name]\n", sys_argv[0]);
	printf("    %s run app_name\n", sys_argv[0]);
	printf("    %s start [app_name]\n", sys_argv[0]);
	printf("    %s [status]\n", sys_argv[0]);
	printf("    %s stop [app_name]\n", sys_argv[0]);
	printf("    %s tail app_name\n", sys_argv[0]);
	printf("    %s version\n", sys_argv[0]);
	printf("\n");

}

void version()
{
	printf("\n");
	printf("  %s\n", sys_argv[0]);
	printf("  version: %s\n", VERSION);
	printf("  build time: %s\n", BUILD_TIME);
	printf("\n");
}

void initialize()
{
	memset(&config, 0, sizeof(config));
	read_config(POLA_CONFIG);
	SET_LOG_LEVEL(LOG_INFO);
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

		// TODO: pola exec ...
		if (!strcmp("start", argv[1])) {
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
		else if (!strcmp("run", argv[1])) {
			if (argc == 2) {
				printf("%s run <app>\n",
					   argv[0]);
				exit(1);
			}
			func = &cmd_run;
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
		else if (!strcmp("force-restart", argv[1])) {
			func = &force_restart;
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

		else if (!strcmp("version", argv[1]) ||
				 !strcmp("-v", argv[1]) ||
				 !strcmp("--version", argv[1])) {
			version();
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
	globfree(&result);

	return 0;
}
