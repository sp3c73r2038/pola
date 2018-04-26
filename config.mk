# version
VERSION = `git rev-parse HEAD | cut -b 1-7`
BUILD_TIME = "`date`"

LIBS = -ljson-c -lpthread

# flags
CFLAGS := -g -std=c99 -Wpedantic -Wall \
	-D_XOPEN_SOURCE=600 -DVERSION=\"${VERSION}\" \
  -DBUILD_TIME=\"${BUILD_TIME}\"
LDFLAGS := -g ${LIBS}
# CC = cc
