objects := $(patsubst %.c,%.o,$(wildcard *.c)) $(patsubst %.c,%.o,$(wildcard ../FiniteStateEntropy/lib/*.c))

CFLAGS=-I../vidpak/ -I../FiniteStateEntropy/lib/

main : $(objects)
	cc -o example $(objects)

clean:
	rm -f *.o ../FiniteStateEntropy/lib/*.o
