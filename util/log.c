#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>


#include "log.h"

#ifndef __OBJEKT002_LOG__

static LOG_LEVEL global_log_level;

void LOG_TRACE(LOG_LEVEL lvl, char *fmt, ... );
void SET_LOG_LEVEL(LOG_LEVEL lvl);
LOG_LEVEL GET_LOG_LEVEL();

/* LOG_TRACE(log level, format, args ) */
void LOG_TRACE(LOG_LEVEL lvl, char *fmt, ... )
{
	va_list  list;
	char *s, c;
	int i;
	char buf[20] = {'\0'};
	time_t now = time(NULL);
	strftime(buf, 20, "%Y-%m-%d %H:%M:%S", localtime(&now));

	if( lvl >= global_log_level) {

		fprintf(stderr, "[%s ", buf);
		if (lvl == LOG_DEBUG) {
			fprintf(stderr, "DEBUG] ");
		}
		else if (lvl == LOG_INFO) {
			fprintf(stderr, " INFO] ");
		}
		else if (lvl == LOG_ERROR) {
			fprintf(stderr, "ERROR] ");
		}

		va_start( list, fmt );

		while(*fmt)	{
			if ( *fmt != '%' )
				putc( *fmt, stderr );
			else {
				switch ( *++fmt )
				{
				case 's':
					/* set r as the next
					   char in list (string) */
					s = va_arg( list, char * );
					fprintf(stderr, "%s", s);
					break;

				case 'd':
					i = va_arg( list, int );
					fprintf(stderr, "%d", i);
					break;

				case 'c':
					c = va_arg( list, int);
					fprintf(stderr, "%c", c);
					break;

				default:
					putc(*fmt, stderr);
					break;
				}
			}
			++fmt;
		}
		va_end( list );
		fprintf(stderr, "\n");
	}
	fflush( stderr );
}

void SET_LOG_LEVEL(LOG_LEVEL lvl)
{
	global_log_level = lvl;
}

LOG_LEVEL GET_LOG_LEVEL()
{
	return global_log_level;
}

#endif
