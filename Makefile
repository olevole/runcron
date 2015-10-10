PREFIX?=/usr/local
CC?=cc
STRIP="/usr/bin/strip"
RM?=rm

all:    runcron

clean:
	${RM} -f runcron *.core

runcron:
	${CC} runcron.c -o runcron -lcrypto && strip runcron

install:
	install runcron ${PREFIX}/bin/runcron
