# Builds with GCC or tcc, the Tiny C Compiler, picked by CC. Executables go
# into OUT, everything else into OUT/obj.
#   make                         release md.exe with GCC, in .
#   make debug | bench | test | clean
#   CC=tcc make test
#   make CC=tcc OUT=foo debug
ifeq ($(origin CC),default)
CC       = gcc
endif
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
# Naming exactly these keeps a stray import, or under GCC a stray libc call,
# from linking.
APPDLLS  = kernel32 user32 gdi32 ole32 advapi32 shell32 comdlg32 dwmapi \
           d2d1 dwrite
TESTDLLS = kernel32
IMGDLLS  = kernel32 ole32
LIBS     = $(APPDLLS:%=-l%)
TESTLIBS = $(TESTDLLS:%=-l%)
IMGLIBS  = $(IMGDLLS:%=-l%)

ifneq ($(findstring tcc,$(notdir $(CC))),)
# tcc builds md in a blink but barely optimises. The release build links no C
# runtime, like GCC's. The debug build and the tests use tcc's bounds checker
# (-b), which needs tcc's own runtime (see base.c), so they do not get
# -nostdlib.
CFLAGS   = -std=c11 -Wall -Wl,--stack=0x200000
RELEASE  = -nostdlib -Wl,-e=WinMainCRTStartup
DEBUG    = -b -bt -g -DDEBUG
GUI      = -Wl,-subsystem=windows
# tcc's own import definitions are too old for md (no GetTickCount64), so
# make them from the DLLs themselves.
DEFS     = $(sort $(APPDLLS) $(TESTDLLS) $(IMGDLLS) ntdll)
DEFFILES = $(DEFS:%=$(OBJ)/%.def)
LDDIRS   = -L$(OBJ)
# tcc's own stack probe, in libtcc1.a, moves the stack pointer itself, so
# it must come before ntdll's, which only checks it.
LIBTCC1  := $(shell $(CC) -print-search-dirs | sed -n '/^libtcc1:/{n;s/^ *//;p}')
RTLIBS   = $(LIBTCC1) -lntdll
EXTRA    = $(DEFFILES)
# -b makes tcc write the exports of every program next to it.
TIDY     = @rm -f $(@:.exe=.def)
else
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
LDDIRS   =
RTLIBS   = -lgcc
EXTRA    =
TIDY     =
endif

# Portable core, then the Windows layer.
CORE     = base.c platform.h md.c mdtables.c highlight.c layout.c view.c
SRC      = $(CORE) win32.c image.c gfx.c
WINDRES  = windres
# The icon, compiled to an object linked into every build of the app. tcc
# only takes resources in windres's COFF form, though it wants them named
# .res, which GCC's linker does not mind.
RES      = $(OBJ)/md.res
EXES     = $(OUT)/md.exe $(OUT)/md-debug.exe $(OUT)/md-bench.exe
TESTS    = $(OUT)/md-test.exe $(OUT)/hl-test.exe $(OUT)/view-test.exe \
           $(OUT)/img-test.exe

.DEFAULT_GOAL := release
release: $(OUT)/md.exe
debug: $(OUT)/md-debug.exe
bench: $(OUT)/md-bench.exe

$(OUT)/md.exe: main.c $(SRC) $(RES) $(EXTRA) $(STAMP)
	$(CC) $(CFLAGS) $(RELEASE) $(GUI) -o $@ main.c $(RES) $(LDDIRS) $(LIBS) $(RTLIBS)
	$(TIDY)

# A release build that also takes --bench, for timing. Not shipped.
$(OUT)/md-bench.exe: main.c $(SRC) $(RES) $(EXTRA) $(STAMP)
	$(CC) $(CFLAGS) $(RELEASE) -DBENCH $(GUI) -o $@ main.c $(RES) $(LDDIRS) $(LIBS) $(RTLIBS)
	$(TIDY)

$(OUT)/md-debug.exe: main.c $(SRC) $(RES) $(EXTRA) $(STAMP)
	$(CC) $(CFLAGS) $(DEBUG) $(GUI) -o $@ main.c $(RES) $(LDDIRS) $(LIBS) $(RTLIBS)
	$(TIDY)

$(RES): res/md.rc res/md.ico $(STAMP)
	$(WINDRES) -O coff -o $@ res/md.rc

$(OBJ)/%.def:
	$(CC) -impdef C:/Windows/System32/$*.dll -o $@

$(OUT)/md-test.exe: test.c $(SRC) $(EXTRA) $(STAMP)
	$(CC) $(CFLAGS) $(DEBUG) -o $@ test.c $(LDDIRS) $(TESTLIBS) $(RTLIBS)
	$(TIDY)

$(OUT)/hl-test.exe: hltest.c base.c win32.c highlight.c $(EXTRA) $(STAMP)
	$(CC) $(CFLAGS) $(DEBUG) -o $@ hltest.c $(LDDIRS) $(TESTLIBS) $(RTLIBS)
	$(TIDY)

# Layout and view against a fake, deterministic platform (fakeos.c).
$(OUT)/view-test.exe: viewtest.c fakeos.c $(CORE) win32.c $(EXTRA) $(STAMP)
	$(CC) $(CFLAGS) $(DEBUG) -o $@ viewtest.c $(LDDIRS) $(TESTLIBS) $(RTLIBS)
	$(TIDY)

# Image decoding, with WIC but without a window or Direct2D. Reads the
# pictures in test/img.
$(OUT)/img-test.exe: imgtest.c base.c platform.h win32.c image.c $(EXTRA) $(STAMP)
	$(CC) $(CFLAGS) $(DEBUG) -o $@ imgtest.c $(LDDIRS) $(IMGLIBS) $(RTLIBS)
	$(TIDY)

# Under tcc's bounds checker the tests pick smaller defaults, see the tests.
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
