CC       = gcc
CFLAGS   = -std=c11 -nostdlib \
           -Wall -Wextra -Wconversion -Wno-sign-conversion -Wno-unused-function \
           -Wdouble-promotion -Wshadow -Wnull-dereference -Wstrict-prototypes -Wvla
# Release only: debuggers and profilers need the unwind tables to walk the
# stack on x64 Windows.
RELEASE  = -O2 -s -fno-asynchronous-unwind-tables
DEBUG    = -g3 -DDEBUG -fsanitize=undefined -fsanitize=bounds-strict \
           -fsanitize-trap -ftrivial-auto-var-init=pattern
GUI      = -mwindows
# Only DLLs that ship with Windows. libgcc for ___chkstk_ms. -nostdlib
# keeps msvcrt out, so a stray libc call is a link error, not an import.
LDLIBS   = -lkernel32 -luser32 -lgdi32 -lole32 -ladvapi32 -lshell32 \
           -lcomdlg32 -ldwmapi -ld2d1 -ldwrite -lwindowscodecs -lgcc
TESTLIBS = -lkernel32 -lgcc
# Portable core, then the Windows layer.
CORE     = base.c platform.h md.c mdtables.c highlight.c layout.c view.c
SRC      = $(CORE) win32.c gfx.c demo.c

md.exe: main.c $(SRC)
	$(CC) $(CFLAGS) $(RELEASE) $(GUI) -o $@ main.c $(LDLIBS)

md-debug.exe: main.c $(SRC)
	$(CC) $(CFLAGS) $(DEBUG) $(GUI) -o $@ main.c $(LDLIBS)

md-test.exe: test.c $(SRC)
	$(CC) $(CFLAGS) $(DEBUG) -o $@ test.c $(TESTLIBS)

hl-test.exe: hltest.c base.c win32.c highlight.c
	$(CC) $(CFLAGS) $(DEBUG) -o $@ hltest.c $(TESTLIBS)

# Layout and view against a fake, deterministic platform (fakeos.c).
view-test.exe: viewtest.c fakeos.c $(CORE) win32.c
	$(CC) $(CFLAGS) $(DEBUG) -o $@ viewtest.c $(TESTLIBS)

TESTS    = md-test.exe hl-test.exe view-test.exe

test: $(TESTS)
	./md-test.exe
	./hl-test.exe
	./view-test.exe

# Check the hand-written COM and Win32 declarations in win32.c against the
# official mingw headers. Layout (vtable slots, struct fields, GUIDs) is
# asserted at compile time and run; constants are compared through a file
# generated from the constants block in win32.c. See comcheck.c. Never
# shipped, so it uses windows.h and the C runtime.
CCHECK   = gcc -std=gnu11 -Wall -Wextra -Wno-unused-function -Wno-unused-parameter

comcheck-consts.inc: win32.c
	sed -n '/BEGIN CONSTANTS/,/END CONSTANTS/p' win32.c | \
	sed -n 's|^ *\([A-Z][A-Za-z0-9_]*\) *= *\([^,]*\),.*|_Static_assert((unsigned)(unsigned long long)(\1) == (unsigned)(\2), "\1");|p' > $@

comcheck: comcheck.c win32.c base.c comcheck-consts.inc
	$(CCHECK) -o comcheck.exe comcheck.c
	./comcheck.exe
	$(CCHECK) -DCONSTS -o comcheck-consts.exe comcheck.c
	./comcheck-consts.exe

clean:
	rm -f md.exe md-debug.exe $(TESTS) comcheck.exe comcheck-consts.exe comcheck-consts.inc

.PHONY: test comcheck clean
