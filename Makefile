all : miniweb minimal

minimal : minimal.c miniweb.h miniweb.o
	gcc -o minimal minimal.c miniweb.o -Wall -pedantic 

miniweb : main.c miniweb.h miniweb.o
	gcc -o miniweb main.c miniweb.o -Wall -pedantic

miniweb.o : miniweb.c miniweb.h
	gcc -c miniweb.c -Wall -pedantic
