# version
VERSION = `git rev-parse HEAD | cut -b 1-7`

LIBS = -ljson-c -lpthread

# flags
CFLAGS := -g -std=c99 -Wpedantic -Wall \
	-D_XOPEN_SOURCE=600 -DVERSION=\"${VERSION}\"
LDFLAGS := -g ${LIBS}
# CC = cc
