# Executables go into OUT, everything else into OUT/obj.
#   make                         release md.exe, in .
#   make debug | bench | test | clean
#   make OUT=foo debug
CC       = gcc
OUT     ?= .
OBJ      = $(OUT)/obj
# Names the compiler that built the objects, so that switching compilers
# rebuilds. Rewritten whenever its content differs (build.bat leaves its own
# there too), so the next make notices.
STAMP    = $(OBJ)/cc
ifneq ($(MAKECMDGOALS),clean)
$(shell mkdir -p $(OBJ); echo '$(CC)' | cmp -s - $(STAMP) || echo '$(CC)' > $(STAMP))
endif

# The DLLs each kind of program imports, all of which ship with Windows.
# Naming exactly these keeps a stray import, or a stray libc call, from
# linking.
APPDLLS  = kernel32 user32 gdi32 ole32 advapi32 shell32 comdlg32 dwmapi \
           d2d1 dwrite
TESTDLLS = kernel32
IMGDLLS  = kernel32 ole32
LIBS     = $(APPDLLS:%=-l%)
TESTLIBS = $(TESTDLLS:%=-l%)
IMGLIBS  = $(IMGDLLS:%=-l%)

CFLAGS   = -std=c11 -nostdlib \
           -Wall -Wextra -Wconversion -Wno-sign-conversion -Wno-unused-function \
           -Wdouble-promotion -Wshadow -Wnull-dereference -Wstrict-prototypes -Wvla
# Release only: debuggers and profilers need the unwind tables to walk the
# stack on x64 Windows.
RELEASE  = -O2 -s -fno-asynchronous-unwind-tables
DEBUG    = -g3 -DDEBUG -fsanitize=undefined -fsanitize=bounds-strict \
           -fsanitize-trap -ftrivial-auto-var-init=pattern
GUI      = -mwindows
# libgcc for ___chkstk_ms. -nostdlib keeps msvcrt out, so a stray libc call
# is a link error, not an import.
RTLIBS   = -lgcc

# Portable core, then the Windows layer.
CORE     = base.c platform.h md.c mdtables.c highlight.c layout.c view.c
SRC      = $(CORE) win32.c image.c gfx.c
WINDRES  = windres
# The icon, compiled to an object linked into every build of the app.
RES      = $(OBJ)/md.res
EXES     = $(OUT)/md.exe $(OUT)/md-debug.exe $(OUT)/md-bench.exe
TESTS    = $(OUT)/md-test.exe $(OUT)/hl-test.exe $(OUT)/view-test.exe \
           $(OUT)/img-test.exe

.DEFAULT_GOAL := release
release: $(OUT)/md.exe
debug: $(OUT)/md-debug.exe
bench: $(OUT)/md-bench.exe

$(OUT)/md.exe: main.c $(SRC) $(RES) $(STAMP)
	$(CC) $(CFLAGS) $(RELEASE) $(GUI) -o $@ main.c $(RES) $(LIBS) $(RTLIBS)

# A release build that also takes --bench, for timing. Not shipped.
$(OUT)/md-bench.exe: main.c $(SRC) $(RES) $(STAMP)
	$(CC) $(CFLAGS) $(RELEASE) -DBENCH $(GUI) -o $@ main.c $(RES) $(LIBS) $(RTLIBS)

$(OUT)/md-debug.exe: main.c $(SRC) $(RES) $(STAMP)
	$(CC) $(CFLAGS) $(DEBUG) $(GUI) -o $@ main.c $(RES) $(LIBS) $(RTLIBS)

$(RES): res/md.rc res/md.ico $(STAMP)
	$(WINDRES) -O coff -o $@ res/md.rc

$(OUT)/md-test.exe: test.c $(SRC) $(STAMP)
	$(CC) $(CFLAGS) $(DEBUG) -o $@ test.c $(TESTLIBS) $(RTLIBS)

$(OUT)/hl-test.exe: hltest.c base.c win32.c highlight.c $(STAMP)
	$(CC) $(CFLAGS) $(DEBUG) -o $@ hltest.c $(TESTLIBS) $(RTLIBS)

# Layout and view against a fake, deterministic platform (fakeos.c).
$(OUT)/view-test.exe: viewtest.c fakeos.c $(CORE) win32.c $(STAMP)
	$(CC) $(CFLAGS) $(DEBUG) -o $@ viewtest.c $(TESTLIBS) $(RTLIBS)

# Image decoding, with WIC but without a window or Direct2D. Reads the
# pictures in test/img.
$(OUT)/img-test.exe: imgtest.c base.c platform.h win32.c image.c $(STAMP)
	$(CC) $(CFLAGS) $(DEBUG) -o $@ imgtest.c $(IMGLIBS) $(RTLIBS)

test: $(TESTS)
	$(OUT)/md-test.exe
	$(OUT)/hl-test.exe
	$(OUT)/view-test.exe
	$(OUT)/img-test.exe

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
	rm -f $(EXES) $(TESTS) comcheck.exe comcheck-consts.exe comcheck-consts.inc
	rm -rf $(OBJ)

.PHONY: release debug bench test comcheck clean
