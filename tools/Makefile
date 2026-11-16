CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable
SRC = ../core/cpu/i386.c ../core/cpu/i8259.c ../core/cpu/i8254.c \
      ../core/rim/board.c ../core/rim/scramble.c ppcli.c

ppcli: $(SRC) $(wildcard ../core/*/*.h)
	$(CC) $(CFLAGS) -o $@ $(SRC) -lm

clean:
	rm -f ppcli
