COPTS= -Wall -pedantic -O4 

all : miniweb minimal

minimal : minimal.c miniweb.h miniweb.o
	gcc -o minimal minimal.c miniweb.o $(COPTS)

miniweb : main.c miniweb.h miniweb.o
	gcc -o miniweb main.c miniweb.o $(COPTS)

miniweb.o : miniweb.c miniweb.h
	gcc -c miniweb.c $(COPTS)
