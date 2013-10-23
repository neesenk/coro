CFLAGS += -Wall -O0 -g
test: coro.o main.o
	gcc coro.o main.o $(CFLAGS) -o test

clean:
	rm -rf *.o test
