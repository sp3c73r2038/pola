#include <ctype.h>
#include <errno.h>
#include <libgen.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>



char *ltrim(char *s)
{
	size_t len = strlen(s);
	while (len && isspace(*s)) {
		s++;
		len--;
	}

	return s;
}


char *rtrim(char *s)
{
	size_t size = strlen(s);
	char *end;

	if (!size)
		return s;

	end = s + size - 1;
	while (end >= s && isspace(*end))
		end--;
	*(end + 1) = '\0';

	return s;
}

char *trim(char *s)
{
	s = ltrim(s);
	s = rtrim(s);
	return s;
}

void path_join(char * path, char * filename, char *result)
{

	int s = 1024;
	char * tmp;
	tmp = (char *)malloc((s + 1) * sizeof(char));
	memset(tmp, '\0', s + 1);
	snprintf(tmp, strlen(path) + 3, "%s/1", path);

	char * p = dirname(tmp);

	snprintf(result, strlen(p) + strlen(filename) + 2,
			 "%s/%s", p, filename);
	free(tmp);
}


ssize_t readline(int fd, void *buffer, size_t n)
{
	ssize_t num_read;
	size_t total_read;
	char *buf;
	char ch;

	if (n <= 0 || buffer == NULL) {
		errno = EINVAL;
		return -1;
	}

	buf = buffer;

	total_read = 0;
	for (;;) {
		num_read = read(fd, &ch, 1);

		if (num_read == -1) {
			if (errno == EINTR)
				continue;
			else
				return -1;

		} else if (num_read == 0) {
			if (total_read == 0)
				return 0;
			else
				break;

		} else {
			if (total_read < n - 1) {
				total_read++;
				*buf++ = ch;
			}

			if (ch == '\n')
				break;
		}
	}

	*buf = '\0';
	return total_read;
}
