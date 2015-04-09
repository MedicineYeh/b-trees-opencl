CC=gcc
CFLAGS=
LFLAGS=-lOpenCL


all	:	tree

execute	:	tree
	./tree ./test.db

tree	:	tree.c
	$(CC) $(CFLAGS) $(LFLAGS) $< -o $@

clean	:	
	rm tree
