# Ndless SDK build (needs nspire-gcc, nspire-ld, genzehn and make-prg on PATH)
GCC = nspire-gcc
LD = nspire-ld
GENZEHN = genzehn
MAKEPRG = make-prg
GCCFLAGS = -Ofast -Wall -W -marm
LDFLAGS =
ZEHNFLAGS = --name "c64"
OBJS = c64.o nspire_main.o
EXE = c64

all: $(EXE).prg.tns

%.o: %.c c64.h
	$(GCC) $(GCCFLAGS) -c $< -o $@

$(EXE).elf: $(OBJS)
	$(LD) $(LDFLAGS) $^ -o $@

$(EXE).tns: $(EXE).elf
	$(GENZEHN) --input $^ --output $@.zehn $(ZEHNFLAGS)
	$(MAKEPRG) $@.zehn $@
	rm -f $@.zehn

$(EXE).prg.tns: $(EXE).tns
	cp $< $@

host_test: host_test.c c64.c c64.h
	gcc -O2 -Wall -Wextra -o $@ host_test.c c64.c

clean:
	rm -f *.o *.elf *.zehn $(EXE).tns $(EXE).prg.tns host_test
