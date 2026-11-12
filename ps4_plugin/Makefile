# Builds goldportal.prx (GoldHEN plugin).
# Requirements (Linux or WSL):
#   OpenOrbis PS4 toolchain  -> export OO_PS4_TOOLCHAIN=/path/to/PS4Toolchain
#   GoldHEN Plugins SDK      -> export GOLDHEN_SDK=/path/to/GoldHEN_Plugins_SDK (build it first: make)
#   clang + ld.lld

TARGET   := goldportal
TOOLCHAIN ?= $(OO_PS4_TOOLCHAIN)
GOLDHEN  ?= $(GOLDHEN_SDK)

SRC  := $(wildcard source/*.c)
OBJ  := $(patsubst source/%.c,build/%.o,$(SRC))

CC   := clang
LD   := ld.lld

# -fvisibility=hidden + --export-dynamic: only symbols marked attr_public
# (plugin_load, plugin_unload, g_plugin*) end up in the module's export table.
CFLAGS  := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -O2 -Wall -fvisibility=hidden \
           -isysroot $(TOOLCHAIN) -isystem $(TOOLCHAIN)/include \
           -I$(GOLDHEN)/include -Isource -std=c11 -D_DEFAULT_SOURCE -D_BSD_SOURCE
LDFLAGS := -m elf_x86_64 -pie --script $(TOOLCHAIN)/link.x -e _init --eh-frame-hdr --export-dynamic \
           -L$(TOOLCHAIN)/lib -L$(GOLDHEN) \
           -lGoldHEN_Hook -lkernel -lSceLibcInternal

all: $(TARGET).prx

build/%.o: source/%.c source/*.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET).elf: $(OBJ)
	$(LD) $(GOLDHEN)/build/crtprx.o $(OBJ) -o $@ $(LDFLAGS)

$(TARGET).prx: $(TARGET).elf
	$(TOOLCHAIN)/bin/linux/create-fself -in=$< --out=$(TARGET).oelf --lib=$@ --paid 0x3800000000000011

clean:
	rm -rf build $(TARGET).elf $(TARGET).oelf $(TARGET).prx

.PHONY: all clean
