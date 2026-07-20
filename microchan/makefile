# makefile - Open Watcom wmake, builds the 16-bit MS-DOS target.
#
# GNU make reads the tree of module.mk files and builds the host targets.
# This file is read by Open Watcom's wmake and builds the DOS ones, which
# GNU make cannot: mc_ipx.c and plat_dos.c are real-mode 16-bit code.
#
#   wmake            (needs WATCOM set and $WATCOM/binl on PATH)
#
# Objects go to _dos/ so they never meet the host build's output.
#
# Made by a machine. PUBLIC DOMAIN (CC0-1.0)

!ifndef %WATCOM
!error WATCOM env var not set
!endif

!ifdef __UNIX__
RM = rm -f
MD = mkdir -p
!else
RM = del
MD = mkdir
!endif

# Large model: far data across multiple 64 KB segments, each object < 64 KB.
MODEL = ml

CC = wcc
LD = wcl

SRC   = src
XPORT = transport
EX    = examples
THOR  = examples/thor
OBJ   = _dos

COPTS = -q -bt=dos -0 -$(MODEL) -w4 -ox -i=$(%WATCOM)/h -i=$(SRC) -i=$(XPORT) -i=$(THOR)
LOPTS = -q -bt=dos -$(MODEL)

.ERASE
.EXTENSIONS:
.EXTENSIONS: .exe .obj .c

# Where wmake looks for a .c when it wants a .obj. Semicolon-separated.
.c: $(SRC);$(XPORT);$(EX);$(THOR)

.BEFORE
	-@$(MD) $(OBJ)

.c.obj: .AUTODEPEND
	$(CC) $(COPTS) -fo=$^@ $[@

# DOS executables must obey the 8.3 filename limit.
all: $(OBJ)/mcdemo.exe $(OBJ)/ipxtest.exe $(OBJ)/thor.exe .SYMBOLIC

DEMO_OBJS = $(OBJ)/microchan.obj $(OBJ)/mcdemo.obj

$(OBJ)/mcdemo.exe: $(DEMO_OBJS)
	$(LD) $(LOPTS) -fe=$@ $(DEMO_OBJS)

IPXTEST_OBJS = $(OBJ)/microchan.obj $(OBJ)/mc_ipx.obj $(OBJ)/ipxtest.obj

$(OBJ)/ipxtest.exe: $(IPXTEST_OBJS)
	$(LD) $(LOPTS) -fe=$@ $(IPXTEST_OBJS)

GAME_OBJS = $(OBJ)/thor.obj $(OBJ)/render.obj $(OBJ)/plat_dos.obj &
	$(OBJ)/game.obj $(OBJ)/game_net.obj $(OBJ)/microchan.obj &
	$(OBJ)/mc_ipx.obj $(OBJ)/rng.obj

$(OBJ)/thor.exe: $(GAME_OBJS)
	$(LD) $(LOPTS) -fe=$@ $(GAME_OBJS)

clean: .SYMBOLIC
	$(RM) $(OBJ)/*.exe
	$(RM) $(OBJ)/*.obj
