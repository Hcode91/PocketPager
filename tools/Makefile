CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable
SRC = ../core/cpu/i386.c ../core/cpu/i8259.c ../core/cpu/i8254.c \
      ../core/rim/board.c ../core/rim/scramble.c ppcli.c
BBOS = ../core/bbos/cod.c ../core/bbos/vm.c ../core/bbos/interp.c ../core/bbos/disasm.c \
       ../core/bbos/natives.c ../core/bbos/natives_rim.c ../core/bbos/natives_vm.c ../core/bbos/natives_sys.c ../core/bbos/natives_crypto.c

all: ppcli bbdump bbrun

ppcli: $(SRC) $(wildcard ../core/*/*.h)
	$(CC) $(CFLAGS) -o $@ $(SRC) -lm

bbdump: bbdump.c ../core/bbos/cod.c ../core/bbos/cod.h
	$(CC) $(CFLAGS) -o $@ bbdump.c ../core/bbos/cod.c

bbrun: bbrun.c $(BBOS) $(wildcard ../core/bbos/*.h ../core/bbos/*.inc)
	$(CC) $(CFLAGS) -Wno-format-truncation -o $@ bbrun.c $(BBOS) -lm

clean:
	rm -f ppcli bbdump bbrun
