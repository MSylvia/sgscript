
ifdef SystemRoot
	RM = del /Q
	FixPath = $(subst /,\,$1)
	PLATFLAGS = -lkernel32
	BINEXT=.exe
	LIBEXT=.dll
else
	RM = rm -f
	FixPath = $1
	PLATFLAGS = 
	BINEXT=
	LIBEXT=.so
endif

CC=gcc
ifeq ($(mode),release)
	CFLAGS = -O3 -Wall
else
	mode = debug
	CFLAGS = -D_DEBUG -g -Wall
endif

ifneq ($(static),)
	LFLAGS = -lsgscript
	OUTLIB = libsgscript.a
	OUTFILE = $(LIBDIR)/libsgscript.a
else
	LFLAGS = $(OUTDIR)/sgscript$(LIBEXT)
	OUTLIB = sgscript$(LIBEXT)
	OUTFILE = $(OUTDIR)/sgscript$(LIBEXT)
endif

LFLAGS += -ldl

SRCDIR=src
LIBDIR=lib
EXTDIR=ext
OUTDIR=bin
OBJDIR=obj


_DEPS = sgs_cfg.h sgs_int.h sgs_util.h sgs_xpc.h sgscript.h
DEPS = $(patsubst %,$(SRCDIR)/%,$(_DEPS))

_OBJ = sgs_bcg.o sgs_ctx.o sgs_fnt.o sgs_proc.o sgs_std.o sgs_stdL.o sgs_tok.o sgs_util.o sgs_xpc.o
OBJ = $(patsubst %,$(OBJDIR)/%,$(_OBJ))

# the library (default target)
$(LIBDIR)/libsgscript.a: $(OBJ)
	ar rcs $@ $(OBJ)
$(OUTDIR)/sgscript$(LIBEXT): $(OBJ)
	gcc -o $@ -shared $(OBJ) -lm $(PLATFLAGS) -I$(SRCDIR) -L$(LIBDIR) $(CFLAGS)
$(OBJDIR)/%.o: $(SRCDIR)/%.c $(DEPS)
	$(CC) -c -o $@ $< $(CFLAGS)

# the libraries
$(OUTDIR)/sgsjson$(LIBEXT): $(OUTFILE) $(EXTDIR)/sgsjson.c
	$(CC) -o $@ $(EXTDIR)/sgsjson.c -shared $(LFLAGS) -lm $(PLATFLAGS) -I$(SRCDIR) -L$(LIBDIR) $(CFLAGS)
# the tools
$(OUTDIR)/sgstest$(BINEXT): $(OUTFILE) $(EXTDIR)/sgstest.c $(OUTDIR)/sgsjson$(LIBEXT)
	$(CC) -o $@ $(EXTDIR)/sgstest.c $(LFLAGS) -lm $(PLATFLAGS) -I$(SRCDIR) -L$(LIBDIR) $(CFLAGS)
$(OUTDIR)/sgsvm$(BINEXT): $(OUTFILE) $(EXTDIR)/sgsvm.c
	$(CC) -o $@ $(EXTDIR)/sgsvm.c $(LFLAGS) -lm $(PLATFLAGS) -I$(SRCDIR) -L$(LIBDIR) $(CFLAGS)
$(OUTDIR)/sgsc$(BINEXT): $(OUTFILE) $(EXTDIR)/sgsc.c
	$(CC) -o $@ $(EXTDIR)/sgsc.c $(LFLAGS) -lm $(PLATFLAGS) -I$(SRCDIR) -L$(LIBDIR) $(CFLAGS)
.PHONY: tools
tools: $(OUTDIR)/sgsjson$(LIBEXT) $(OUTDIR)/sgstest$(BINEXT) $(OUTDIR)/sgsvm$(BINEXT) $(OUTDIR)/sgsc$(BINEXT)
.PHONY: test
test: $(OUTDIR)/sgstest$(BINEXT)
	$(OUTDIR)/sgstest

# other stuff
# - multithreaded testing
.PHONY: test_mt
test_mt: $(OUTDIR)/sgstest_mt$(BINEXT)
	$(OUTDIR)/sgstest_mt
$(OUTDIR)/sgstest_mt$(BINEXT): $(LIBDIR)/libsgscript.a
	$(CC) -o $@ examples/sgstest_mt.c $(LFLAGS) -lm -lpthread $(PLATFLAGS) -I$(SRCDIR) -L$(LIBDIR) $(CFLAGS)

# clean everything
.PHONY: clean
clean:
	$(RM) $(call FixPath,$(OBJDIR)/*.o $(LIBDIR)/*.a $(LIBDIR)/*.lib $(OUTDIR)/sgs*)
