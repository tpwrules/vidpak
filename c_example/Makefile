objects := $(patsubst %.c,%.o,$(wildcard *.c)) $(patsubst %.c,%.o,$(wildcard ../FiniteStateEntropy/lib/*.c)) $(patsubst %.c,%.o,$(wildcard ../vidpak/*.c))

# these optimization settings are good for 4-5x speed increase!
CFLAGS=-I../vidpak/ -I../FiniteStateEntropy/lib/ -O0 -fwrapv -g

main : $(objects)
	cc -o example $(objects)

clean:
	rm -f *.o ../FiniteStateEntropy/lib/*.o ../vidpak/*.o
