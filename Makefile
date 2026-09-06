SHELL      = /bin/sh
CC         = gcc
CFLAGS     = -g -Wall -Wextra -I. -L.
RM         = rm -f

IP         = 127.0.0.1
PORT1      = 5050
PORT2      = 5051
PORT3      = 8080
PORT4      = 8081

FOUR_USERS ?= 0

LIBNAME    = libksocket.a
LIBOBJ     = ksocket.o

.PHONY: all library init user runinit runuser clean deepclean

all: library init user

library: $(LIBNAME)

$(LIBNAME): $(LIBOBJ)
	ar rcs $@ $^

$(LIBOBJ): ksocket.c ksocket.h

init: $(LIBNAME) initksocket.c ksocket.h
	$(CC) $(CFLAGS) -o initk initksocket.c -lksocket -lpthread

user: $(LIBNAME) user1.c user2.c ksocket.h
	$(CC) $(CFLAGS) -o u1 user1.c -lksocket
	$(CC) $(CFLAGS) -o u2 user2.c -lksocket

runinit: init
	./initk

runuser: user
	gnome-terminal -- bash -c "./u1 $(IP) $(PORT1) $(IP) $(PORT2); exec bash"
	gnome-terminal -- bash -c "./u2 $(IP) $(PORT2) $(IP) $(PORT1); exec bash"
ifeq ($(FOUR_USERS), 1)
	gnome-terminal -- bash -c "./u1 $(IP) $(PORT3) $(IP) $(PORT4); exec bash"
	gnome-terminal -- bash -c "./u2 $(IP) $(PORT4) $(IP) $(PORT3); exec bash"
endif

clean:
	$(RM) $(LIBOBJ) initk u1 u2 received_*.txt

deepclean: clean
	$(RM) $(LIBNAME)
