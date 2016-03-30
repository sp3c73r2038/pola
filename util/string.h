#include <unistd.h>

char* ltrim(char *s);
char* rtrim(char *s);
char* trim(char *s);

void path_join(char * path, char * filename, char *result);

ssize_t readline(int fd, void *buffer, size_t n);
