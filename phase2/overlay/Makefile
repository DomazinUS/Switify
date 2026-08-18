#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET      := switify-overlay
BUILD       := build
SOURCES     := source ../ipc-client/source
INCLUDES    := ../ipc-client/include ../common/include
APP_TITLE   := Switify
APP_AUTHOR  := HiroshiYamauchi
APP_VERSION := 0.4.4
NO_ICON     := 1

ifeq ($(LIGHTWEIGHT),1)
APP_TITLE   := Switify Lite
APP_VERSION := 0.4.4-lite1
DEFINES     += -DSWITIFY_LIGHTWEIGHT_OVERLAY=1
endif

include $(TOPDIR)/lib/libultrahand/ultrahand.mk

ARCH        := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS      := -Os -g -Wall -Wextra -ffunction-sections -fdata-sections \
               -flto -fuse-linker-plugin -fomit-frame-pointer \
               -fno-strict-aliasing $(ARCH) $(DEFINES)
CFLAGS      += $(INCLUDE) -DNDEBUG=1
CXXFLAGS    := $(CFLAGS) -std=gnu++26 -fno-rtti -fno-exceptions
ASFLAGS     := -g $(ARCH)
LDFLAGS     := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) \
               -Wl,--gc-sections,-Map,$(notdir $*.map)
LIBS        := -lturbojpeg -lcurl -lz -lmbedtls -lmbedx509 -lmbedcrypto -lnx
LIBDIRS     := $(PORTLIBS) $(LIBNX)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)
export VPATH := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

export LD := $(CXX)
export OFILES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
export NROFLAGS := --nacp=$(CURDIR)/$(TARGET).nacp

.PHONY: all clean $(BUILD)

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@rm -rf $(BUILD) $(TARGET).ovl $(TARGET).nacp $(TARGET).elf \
	        $(TARGET).map

else

DEPENDS := $(OFILES:.o=.d)

.PHONY: all
all: $(OUTPUT).ovl

$(OUTPUT).ovl: $(OUTPUT).elf $(OUTPUT).nacp
	@elf2nro $< $@ $(NROFLAGS)
	@printf 'ULTR' >> $@
	@echo built ... $(notdir $@)

$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif
