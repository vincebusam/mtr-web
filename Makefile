all: backend

backend: backend.c
	$(CC) -o backend backend.c -lwebsockets -ljansson -O2 -Wall
