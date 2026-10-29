#---------------------------------------------------------------------------------
# NSMBW 3DS static recompilation — based on the devkitPro 3DS template.
#
#   make recomp   regenerate build/gen from game/ (needs Python 3)
#   make          build nsmbw3ds.3dsx
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

# make TRACE=1: guest function trace + sampling profiler (separate objects)
ifeq ($(TRACE),1)
TARGET      := nsmbw3ds_trace
BUILD       := build/3ds-trace
else
TARGET      := nsmbw3ds
BUILD       := build/3ds
endif
SOURCES     := runtime/src build/gen
DATA        :=
INCLUDES    := runtime/include build/gen

APP_TITLE       := NSMBW 3DS
APP_DESCRIPTION := New Super Mario Bros. Wii static recompilation
APP_AUTHOR      := nsmbw3ds

#---------------------------------------------------------------------------------
ARCH    := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS  := -g -Wall -Wno-unused-label -O2 -std=gnu11 -mword-relocations \
           -ffunction-sections $(ARCH)
CFLAGS  += $(INCLUDE) -D__3DS__
ifeq ($(TRACE),1)
CFLAGS  += -DRT_TRACE_ENABLED
endif
ASFLAGS := -g $(ARCH)
LDFLAGS  = -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map) -Wl,--gc-sections

LIBS    := -lcitro3d -lctru -lm
LIBDIRS := $(CTRULIB)

#---------------------------------------------------------------------------------
ifneq ($(notdir $(BUILD)),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)

CFILES    := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
PICAFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.v.pica)))

export LD       := $(CC)
export OFILES_SOURCES := $(CFILES:.c=.o)
export OFILES_BIN     := $(PICAFILES:.v.pica=.shbin.o)
export OFILES   := $(OFILES_BIN) $(OFILES_SOURCES)
export HFILES   := $(PICAFILES:.v.pica=_shbin.h)
export INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                   $(foreach dir,$(LIBDIRS),-I$(dir)/include) -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
export APP_ICON := $(CTRULIB)/default_icon.png
export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh

.PHONY: all clean recomp cxi cia cia-full

all: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

# .cxi / .cia ask for the New 3DS 178 MiB memory mode (config/nsmbw3ds.rsf);
# a .3dsx only gets 64 MiB, too little for the Wii RAM. makerom: tools/bin.
MAKEROM ?= $(CURDIR)/tools/bin/makerom.exe

cxi: all
	$(MAKEROM) -f cxi -o $(TARGET).cxi -rsf config/nsmbw3ds.rsf -target t -elf $(TARGET).elf -icon $(TARGET).smdh

cia: cxi
	$(MAKEROM) -f cia -o $(TARGET).cia -content $(TARGET).cxi:0:0

# cia-full: game.iso and rel_image.bin (from pcdata/) inside the RomFS, nothing
# to copy to the SD card. It contains the game: personal use only.
cia-full: all
	@rm -rf build/romfs && mkdir -p build/romfs
	cp pcdata/game.iso pcdata/rel_image.bin build/romfs/
	$(MAKEROM) -f cxi -o $(TARGET)_full.cxi -rsf config/nsmbw3ds_full.rsf -target t -elf $(TARGET).elf -icon $(TARGET).smdh
	$(MAKEROM) -f cia -o $(TARGET)_full.cia -content $(TARGET)_full.cxi:0:0

$(BUILD):
	@mkdir -p $@

recomp:
	python -m recomp game build/gen

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(TARGET).smdh $(TARGET).elf

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
DEPENDS := $(OFILES:.o=.d)

$(OUTPUT).3dsx: $(OUTPUT).elf $(OUTPUT).smdh
$(OFILES_SOURCES) : $(HFILES)
$(OUTPUT).elf:  $(OFILES)

.PRECIOUS : %.shbin
%.shbin.o %_shbin.h : %.shbin
	$(SILENTMSG) $(notdir $<)
	$(bin2o)

-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
