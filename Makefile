
# If we have ccdv (http://ccdv.sf.net) in our path, use it
SHELL=/bin/bash
CCDVCMD=$(shell type -p ccdv)
#CCDVCMD=
ifeq ($(CCDVCMD),)
export CCDV=
else
export CCDV=@$(CCDVCMD)
endif

export CC=$(CCDV) cc
export AR=$(CCDV) ar
export RM=$(CCDV) rm
export LD=$(CCDV) ld
export STRIP=$(CCDV) strip

# End CCDV stuff

CFLAGS=-Wall -g
LDFLAGS=-lpthread

#.c.o:
#	$(CCDV) $(CC) -c $(CFLAGS) $(CPPFLAGS) -o $@ $<

TARGETS=eCrash.a

# Real meat goes here

all: $(TARGETS)

eCrash.a: eCrash.o
	$(AR) r $@ $^

eCrash.o: eCrash.c eCrash.h

ecrash_test: ecrash_test.o eCrash.a
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@# Make .debug version, and strip real ecrash_test,
	@# Since, in the real world, you don't ship code with debugging
	@# information
	$(CCDV) cp ecrash_test ecrash_test.debug
	$(STRIP) ecrash_test

ecrash_test.o: ecrash_test.c
	$(CC) $(CFLAGS) -o $@ -c $<

test:	ecrash_test

clean:
	$(RM) -f *.a *.o ecrash_test ecrash_test.debug
