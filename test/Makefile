MDIR ?= ../../mongoose
WARN ?= -W -Wall -Wextra -Werror -Wundef -Wshadow -Wdouble-promotion -Wno-missing-field-initializers
OPTS ?= -g3 -Os
DEFS ?= -DMIP_DEBUG -DMG_ARCH=MG_ARCH_CUSTOM -DMG_ENABLE_LINES=1
INCL ?= -I. -I.. -I$(MDIR)
LIBS ?=
ARGS ?=
CFLAGS ?= $(WARN) $(OPTS) $(INCL) $(DEFS) 

all: test test++

test:
	$(CC) $(CFLAGS) unit_test.c $(MDIR)/mongoose.c -I$(MDIR) -pthread -o /tmp/ut
	$(RUN) /tmp/ut

test++: test
test++: CC = g++

vlan: vlan.c mongoose_custom.c ../mip.c $(MDIR)/mongoose.c
	$(CC) $? $(CFLAGS) $(LIBS) -o /tmp/vlan
	sudo $(RUN) /tmp/vlan $(ARGS)

clean:
	rm -rf /tmp/vlan /tmp/ut *.dSYM *.o *.obj _CL*
