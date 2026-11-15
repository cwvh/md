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
CORE     = base.c platform.h md.c
SRC      = $(CORE) win32.c

md.exe: main.c $(SRC)
	$(CC) $(CFLAGS) $(RELEASE) $(GUI) -o $@ main.c $(LDLIBS)

md-debug.exe: main.c $(SRC)
	$(CC) $(CFLAGS) $(DEBUG) $(GUI) -o $@ main.c $(LDLIBS)

md-test.exe: test.c $(SRC)
	$(CC) $(CFLAGS) $(DEBUG) -o $@ test.c $(TESTLIBS)

test: md-test.exe
	./md-test.exe

# Compile-time check of the hand-written COM declarations in win32.c
# against the official mingw headers. Produces no binary.
comcheck: comcheck.c win32.c
	$(CC) -std=c11 -fsyntax-only -Wall comcheck.c

clean:
	rm -f md.exe md-debug.exe md-test.exe

.PHONY: test comcheck clean
