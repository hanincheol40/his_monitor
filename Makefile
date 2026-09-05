CC      ?= gcc
# -std=c11 hides everything POSIX from the standard headers, so strtok_r,
# clock_nanosleep and TIOCGWINSZ need the feature test macro. Two of those
# three fail loudly without it -- clock_nanosleep returns int, so an implicit
# declaration only warns, and TIOCGWINSZ is a macro, so it is a hard "not
# declared" error. strtok_r is the dangerous one: implicit int truncates the
# returned char * to 32 bits and the program crashes at run time.
#
# Every source file also defines the macro for itself, before its first
# include, so dropping this flag alone changes nothing. It is here so that a
# new file added to the project inherits the right declarations by default.
CFLAGS  ?= -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -pedantic -Isrc
LDLIBS   = -lm -lpthread

MON_SRC = src/main.c src/hisfile.c src/wave.c src/render.c
MON_OBJ = $(MON_SRC:.c=.o)
HDR     = src/monitor.h src/hisfile.h src/wave.h src/render.h

all: his_monitor preflight

his_monitor: $(MON_OBJ)
	$(CC) $(CFLAGS) -o $@ $(MON_OBJ) $(LDLIBS)

preflight: src/preflight.o
	$(CC) $(CFLAGS) -o $@ $< -lm

%.o: %.c $(HDR)
	$(CC) $(CFLAGS) -c -o $@ $<

# Two kinds of test, because the two programs fail differently.
#
# his_monitor's parsing layer is linked in directly (without main.c) and driven
# from C: its failures are wrong values in memory, so the state has to be read
# back. preflight is exercised as a whole binary from the shell: its dangerous
# failure is printing "OK" for a file that is not OK, which is a property of
# the parser, the checks and the reporting together -- and the one real bug in
# it produced a perfectly well-formed number that only the verdict revealed.
test: tests/test_hisfile preflight
	./tests/test_hisfile
	@bash tests/test_preflight.sh ./preflight

tests/test_hisfile: tests/test_hisfile.c src/hisfile.o src/wave.o $(HDR)
	$(CC) $(CFLAGS) -o $@ tests/test_hisfile.c src/hisfile.o src/wave.o $(LDLIBS)

clean:
	rm -f his_monitor preflight $(MON_OBJ) src/preflight.o tests/test_hisfile

.PHONY: all clean test
