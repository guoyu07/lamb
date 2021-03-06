
CC = gcc
MACRO = -D_GNU_SOURCE
CFLAGS = -std=c99 -Wall -pedantic
OBJS = src/account.o src/cache.o src/channel.o src/company.o src/config.o
OBJS += src/db.o src/routing.o src/common.o src/security.o src/message.o src/gateway.o
OBJS += src/list.o src/template.o src/keyword.o src/socket.o src/command.o src/log.o
LIBS = -pthread -lssl -lcrypto -liconv -lcmpp -lconfig -lpq -lhiredis -lpcre -lprotobuf-c

all: sp ismg server mt mo scheduler delivery daemon test

sp: src/sp.c src/sp.h $(OBJS)
	$(CC) $(CFLAGS) $(MACRO) src/sp.c $(OBJS) $(LIBS) -lnanomsg -o sp

ismg: src/ismg.c src/ismg.h $(OBJS)
	$(CC) $(CFLAGS) $(MACRO) src/ismg.c $(OBJS) $(LIBS) -lnanomsg -o ismg

server: src/server.c src/server.h $(OBJS)
	$(CC) $(CFLAGS) $(MACRO) src/server.c $(OBJS) $(LIBS) -lnanomsg -o server

mt: src/mt.c src/mt.h $(OBJS) src/queue.o
	$(CC) $(CFLAGS) $(MACRO) src/mt.c src/queue.o $(OBJS) $(LIBS) -lnanomsg -o mt

mo: src/mo.c src/mo.h $(OBJS) src/queue.o
	$(CC) $(CFLAGS) $(MACRO) src/mo.c src/queue.o $(OBJS) $(LIBS) -lnanomsg -o mo

scheduler: src/scheduler.c src/scheduler.h $(OBJS) src/queue.o
	$(CC) $(CFLAGS) $(MACRO) src/scheduler.c $(OBJS) src/queue.o $(LIBS) -lnanomsg -o scheduler

delivery: src/delivery.c src/delivery.h $(OBJS) src/queue.o
	$(CC) $(CFLAGS) $(MACRO) src/delivery.c $(OBJS) src/queue.o $(LIBS) -lnanomsg -o delivery

daemon: src/daemon.c src/daemon.h $(OBJS)
	$(CC) $(CFLAGS) $(MACRO) src/daemon.c $(OBJS) $(LIBS) -lnanomsg -o daemon

lamb: src/lamb.c src/lamb.h src/linenoise.c src/linenoise.h $(OBJS)
	$(CC) $(CFLAGS) $(MACRO) src/lamb.c src/linenoise.c $(OBJS) $(LIBS) -lnanomsg -o lamb

test: src/test.c src/test.h $(OBJS)
	$(CC) $(CFLAGS) $(MACRO) src/test.c $(OBJS) $(LIBS) -lnanomsg -o testd

src/account.o: src/account.c src/account.h
	$(CC) $(CFLAGS) $(MACRO) -c src/account.c -o src/account.o

src/cache.o: src/cache.c src/cache.h
	$(CC) $(CFLAGS) $(MACRO) -c src/cache.c -o src/cache.o

src/channel.o: src/channel.c src/channel.h
	$(CC) $(CFLAGS) $(MACRO) -c src/channel.c -o src/channel.o

src/company.o: src/company.c src/company.h
	$(CC) $(CFLAGS) $(MACRO) -c src/company.c -o src/company.o

src/config.o: src/config.c src/config.h
	$(CC) $(CFLAGS) $(MACRO) -c src/config.c -o src/config.o

src/db.o: src/db.c src/db.h
	$(CC) $(CFLAGS) $(MACRO) -c src/db.c -o src/db.o

src/gateway.o: src/gateway.c src/gateway.h
	$(CC) $(CFLAGS) $(MACRO) -c src/gateway.c -o src/gateway.o

src/routing.o: src/routing.c src/routing.h
	$(CC) $(CFLAGS) $(MACRO) -c src/routing.c -o src/routing.o

src/queue.o: src/queue.c src/queue.h
	$(CC) $(CFLAGS) $(MACRO) -c src/queue.c -o src/queue.o

src/common.o: src/common.c src/common.h
	$(CC) $(CFLAGS) $(MACRO) -c src/common.c -o src/common.o

src/security.o: src/security.c src/security.h
	$(CC) $(CFLAGS) $(MACRO) -c src/security.c -o src/security.o

src/list.o: src/list.c src/list.h
	$(CC) $(CFLAGS) $(MACRO) -c src/list.c -o src/list.o

src/keyword.o: src/keyword.c src/keyword.h
	$(CC) $(CFLAGS) $(MACRO) -c src/keyword.c -o src/keyword.o

src/template.o: src/template.c src/template.h
	$(CC) $(CFLAGS) $(MACRO) -c src/template.c -o src/template.o

src/delivery.o: src/delivery.c src/delivery.h
	$(CC) $(CFLAGS) $(MACRO) -c src/delivery.c -o src/delivery.o

src/socket.o: src/socket.c src/socket.h
	$(CC) $(CFLAGS) $(MACRO) -c src/socket.c -o src/socket.o

src/command.o: src/command.c src/command.h
	$(CC) $(CFLAGS) $(MACRO) -c src/command.c -o src/command.o

src/message.o: src/message.c src/message.h
	$(CC) $(CFLAGS) $(MACRO) -c src/message.c -o src/message.o

.PHONY: install clean

install:
	/usr/bin/mkdir -p /usr/local/lamb/bin
	/usr/bin/mkdir -p /etc/lamb
	/usr/bin/install -m 750 lamb /usr/bin
	/usr/bin/install -m 750 ismg /usr/local/lamb/bin
	/usr/bin/install -m 750 mt /usr/local/lamb/bin
	/usr/bin/install -m 750 mo /usr/local/lamb/bin
	/usr/bin/install -m 750 server /usr/local/lamb/bin
	/usr/bin/install -m 750 scheduler /usr/local/lamb/bin
	/usr/bin/install -m 750 delivery /usr/local/lamb/bin
	/usr/bin/install -m 750 sp /usr/local/lamb/bin
	/usr/bin/install -m 750 testd /usr/local/lamb/bin
	/usr/bin/install -m 750 daemon /usr/local/lamb/bin
	/usr/bin/install -m 640 config/ismg.conf /etc/lamb
	/usr/bin/install -m 640 config/mt.conf /etc/lamb
	/usr/bin/install -m 640 config/mo.conf /etc/lamb
	/usr/bin/install -m 640 config/server.conf /etc/lamb
	/usr/bin/install -m 640 config/scheduler.conf /etc/lamb
	/usr/bin/install -m 640 config/deliver.conf /etc/lamb
	/usr/bin/install -m 640 config/sp.conf /etc/lamb
	/usr/bin/install -m 640 config/test.conf /etc/lamb

clean:
	rm -f src/*.o
	rm -f lamb ismg mt mo server scheduler delivery sp testd daemon

