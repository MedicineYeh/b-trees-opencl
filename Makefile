CC=gcc
CFLAGS=
LFLAGS=-lOpenCL


all	:	tree

execute	:	tree
	./tree ./test.db < ./test_set

tree	:	tree.c
	$(CC) $(CFLAGS) $(LFLAGS) $< -o $@

clean	:	
	rm tree
