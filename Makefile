CC     = gcc
CFLAGS = -Wall -Wno-sizeof-pointer-memaccess -std=c99 -g

# All services share common.c
COMMON = common.c

ALL = port_manager conn_manager traffic_manager cli

.PHONY: all clean setup run stop test rebuild cleanall

all: $(ALL)

port_manager: port_manager.c $(COMMON)
	$(CC) $(CFLAGS) -o $@ $^

conn_manager: conn_manager.c $(COMMON)
	$(CC) $(CFLAGS) -o $@ $^

traffic_manager: traffic_manager.c $(COMMON)
	$(CC) $(CFLAGS) -o $@ $^

cli: cli.c $(COMMON)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(ALL)

setup:
	chmod +x start.sh test.py stop.sh

run: all
	./start.sh

stop:
	./stop.sh

test: all
	./test.py --no-build

rebuild: clean all

cleanall: clean
	rm -f wsmini.log
	rm -rf *.dSYM
