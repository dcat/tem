LDFLAGS = -lxcb -lxcb-keysyms -lxcb-util -lxcb-xrm -lutil
CFLAGS  = -g

.c:
	${CC} ${CFLAGS} -o $@ $< ${LDFLAGS}

all: tem

