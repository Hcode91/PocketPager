CC ?= cc
CFLAGS ?= -O2 -g -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable
SRC = ../core/cpu/i386.c ../core/cpu/i8259.c ../core/cpu/i8254.c \
      ../core/rim/board.c ../core/rim/scramble.c ppcli.c
BBOS = ../core/bbos/cod.c ../core/bbos/vm.c ../core/bbos/interp.c ../core/bbos/disasm.c \
       ../core/bbos/natives.c ../core/bbos/natives_rim.c ../core/bbos/natives_vm.c ../core/bbos/natives_sys.c ../core/bbos/natives_crypto.c ../core/bbos/natives_gfx.c ../core/bbos/natives_misc.c ../core/bbos/natives_font.c ../core/bbos/blockcipher.c ../core/bbos/bbhost.c \
       ../core/bbos/bbarena.c ../core/bbos/bbsnap.c

all: ppcli bbdump bbrun bbimport

ppcli: $(SRC) $(wildcard ../core/*/*.h)
	$(CC) $(CFLAGS) -o $@ $(SRC) -lm

bbdump: bbdump.c ../core/bbos/cod.c ../core/bbos/cod.h
	$(CC) $(CFLAGS) -o $@ bbdump.c ../core/bbos/cod.c

bbrun: bbrun.c $(BBOS) $(wildcard ../core/bbos/*.h ../core/bbos/*.inc)
	$(CC) $(CFLAGS) -Wno-format-truncation -D_GNU_SOURCE -include ../core/bbos/bbarena.h -o $@ bbrun.c $(BBOS) -lm -lz

bbimport: bbimport_cli.c ../core/bbos/bbimport.c ../core/bbos/cod.c ../core/bbos/bbimport.h
	$(CC) $(CFLAGS) -o $@ bbimport_cli.c ../core/bbos/bbimport.c ../core/bbos/cod.c -lz

clean:
	rm -f ppcli bbdump bbrun bbimport
