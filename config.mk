# version
VERSION = 0.0.1

LIBS =

# flags
CFLAGS := -g -std=c99 -Wpedantic -Wall \
	-D_XOPEN_SOURCE=600 -DVERSION=\"${VERSION}\"
LDFLAGS := -g ${LIBS}
# CC = cc
