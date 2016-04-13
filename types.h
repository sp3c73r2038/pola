#include <sys/types.h>

typedef struct {
	char dir[2048];
	char log_dir[2048];
	char apps[2048];
	unsigned int interval;
} config_t;

typedef struct {
	char name[1024];
	char command[4096];
	char out_file[2048];
	char err_file[2048];
	char user[128];
	unsigned int proc_num;
	unsigned int interval;
} app_t;

typedef struct {
	pid_t pid;
	int fd;
} process_t;
