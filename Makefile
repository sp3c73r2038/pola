include config.mk

SRC = $(wildcard pola.c util/*.c)
OBJ = $(SRC:.c=.o)

all: options pola

options:
	@echo pola build options
	@echo "CFLAGS = ${CFLAGS}"
	@echo "LDLAGS = ${LDFLAGS}"
	@echo "CC = ${CC}"

config.h:
	cp config.def.h config.h

%.o : %.c
	${CC} -c ${CFLAGS} -o $@ $<

${OBJ}: config.h config.mk

pola: ${OBJ}
	${CC} -o $@ ${OBJ} ${LDFLAGS}

clean:
	@echo cleaning
	rm -f pola ${OBJ}

.PHONY: all options clean
