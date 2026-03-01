#########################
## Toolchain variables ##
#########################

# Set NATIVE=1 for native x86_64 build (for testing), otherwise Miyoo Mini cross-compile
NATIVE ?= 0

ifeq ($(NATIVE),1)
# Native toolchain
CC		?= gcc
CXX		?= g++
STRIP		?= strip
INC_DIR		?=
LIB_DIR		?=
LTO		=
STRIP_BIN	= 0
else
# Miyoo Mini cross-compile toolchain
TOOLCHAIN_DIR	?= /opt/miyoomini-toolchain
CC		= $(TOOLCHAIN_DIR)/bin/arm-linux-gnueabihf-gcc
CXX		= $(TOOLCHAIN_DIR)/bin/arm-linux-gnueabihf-g++
STRIP		= $(TOOLCHAIN_DIR)/bin/arm-linux-gnueabihf-strip
SDL_CONFIG	?= $(TOOLCHAIN_DIR)/arm-linux-gnueabihf/libc/usr/bin/sdl-config
FREETYPE_CONFIG	?= $(TOOLCHAIN_DIR)/arm-linux-gnueabihf/libc/usr/bin/freetype-config
INC_DIR		?= $(TOOLCHAIN_DIR)/arm-linux-gnueabihf/libc/usr/include
LIB_DIR		?= $(TOOLCHAIN_DIR)/arm-linux-gnueabihf/libc/usr/lib
LTO		= -flto
STRIP_BIN	= 1
endif

#########################
#########################

ifeq ($(PACKAGE_NAME),)
PACKAGE_NAME = retroarch
endif

DEBUG ?= 0

ifeq ($(NATIVE),1)
MIYOOMINI = 0
DINGUX = 1
else
MIYOOMINI = 1
DINGUX = 1
endif
HAVE_SCREENSHOTS = 1
HAVE_REWIND = 1
HAVE_7ZIP = 1
HAVE_AL = 0
HAVE_ALSA = 0
HAVE_DSP_FILTER = 1
HAVE_VIDEO_FILTER = 1
HAVE_STATIC_VIDEO_FILTERS = 1
HAVE_STATIC_AUDIO_FILTERS = 1
HAVE_FILTERS_BUILTIN	= 1
HAVE_BUILTINMBEDTLS = 0
HAVE_BUILTINZLIB = 1
HAVE_C99 = 1
HAVE_CC = 1
#HAVE_CC_RESAMPLER = 1
HAVE_CHD = 1
HAVE_COMMAND = 1
HAVE_CXX = 1
HAVE_DR_MP3 = 1
HAVE_DYNAMIC = 1
HAVE_DYLIB = 1
HAVE_EGL = 0
HAVE_FREETYPE = 0
HAVE_GDI = 1
HAVE_GETOPT_LONG = 1
HAVE_GLSL = 0
HAVE_HID = 0
HAVE_IBXM = 1
HAVE_IMAGEVIEWER = 0
HAVE_LANGEXTRA = 1
HAVE_LIBRETRODB = 1
HAVE_MENU = 1
HAVE_MENU_COMMON = 1
HAVE_GFX_WIDGETS = 1
HAVE_MMAP = 1
HAVE_OPENDINGUX_FBDEV = 0
HAVE_OPENGL = 0
HAVE_OPENGL1 = 0
HAVE_OPENGLES = 0
HAVE_OPENGLES3 = 0
HAVE_OPENGL_CORE = 0
HAVE_OPENSSL = 1
HAVE_OVERLAY = 1
HAVE_RBMP = 1
HAVE_RJPEG = 1
HAVE_RPILED = 0
HAVE_RPNG = 1
HAVE_RUNAHEAD = 1
ifeq ($(NATIVE),1)
HAVE_SDL_DINGUX = 0
HAVE_SDL2 = 1
else
HAVE_SDL_DINGUX = 1
HAVE_SDL2 = 0
endif
HAVE_SHADERPIPELINE = 0
HAVE_STB_FONT = 0
HAVE_STB_IMAGE = 0
HAVE_STB_VORBIS = 0
HAVE_STDIN_CMD = 0
HAVE_STRCASESTR = 1
HAVE_THREADS = 1
HAVE_UDEV = 0
HAVE_RGUI = 1
HAVE_MATERIALUI = 0
HAVE_XMB = 0
HAVE_OZONE = 0
HAVE_ZLIB = 1
HAVE_CONFIGFILE = 1
HAVE_PATCH = 1
HAVE_CHEATS = 1
HAVE_LIBSHAKE = 0
HAVE_CORE_INFO_CACHE = 1
#HAVE_TINYALSA = 1
HAVE_NEAREST_RESAMPLER = 1
ifeq ($(NATIVE),1)
HAVE_NEON = 0
else
HAVE_NEON = 1
endif
HAVE_OSS = 1
ifeq ($(NATIVE),1)
HAVE_AUDIOIO = 0
else
HAVE_AUDIOIO = 1
endif
HAVE_TRANSLATE = 0
HAVE_VIDEO_LAYOUT = 1
HAVE_NETWORKING = 1
HAVE_GETADDRINFO = 1
HAVE_IFINFO = 1

ifeq ($(MIYOO354),1)
# Netplay and Cheevos for MMP
HAVE_NETPLAYDISCOVERY = 1
HAVE_CHEEVOS = 1
endif

OS = Linux
TARGET = $(PACKAGE_NAME)

OBJ :=
ifneq ($(NATIVE),1)
OBJ += miyoo.o
endif
LINK := $(CXX)

ifeq ($(NATIVE),1)
# Native x86_64 flags (with DINGUX to match Miyoo feature set)
DEF_FLAGS := -ffast-math -fomit-frame-pointer
DEF_FLAGS += -ffunction-sections -fdata-sections
DEF_FLAGS += -I. -Ideps -Ideps/stb -DDINGUX -DNATIVE_BUILD -MMD
DEF_FLAGS += -Wall -Wno-unused-function -Wno-unused-variable $(LTO)
DEF_FLAGS += -std=gnu99 -D_GNU_SOURCE
LIBS := -ldl -lz -lrt -pthread
else
# Miyoo Mini ARM flags
DEF_FLAGS := -marm -mtune=cortex-a7 -march=armv7ve+simd -mfpu=neon-vfpv4 -mfloat-abi=hard -ffast-math -fomit-frame-pointer
DEF_FLAGS += -ffunction-sections -fdata-sections
DEF_FLAGS += -I. -Ideps -Ideps/stb -DMIYOOMINI -DDINGUX -MMD
DEF_FLAGS += -Wall -Wno-unused-function -Wno-unused-variable $(LTO)
DEF_FLAGS += -std=gnu99 -D_GNU_SOURCE
LIBS := -ldl -lz -lrt -pthread -lmi_sys -lmi_gfx -lmi_ao -lmi_common
endif
CFLAGS :=
CXXFLAGS := -fno-exceptions -fno-rtti -std=c++11 -D__STDC_CONSTANT_MACROS
ASFLAGS :=

ifneq ($(DEBUG),1)
LDFLAGS := -Wl,--gc-sections -s
else
LDFLAGS :=
endif

ifeq ($(NATIVE),1)
INCLUDE_DIRS =
LIBRARY_DIRS =
else
INCLUDE_DIRS = -I$(INC_DIR)
LIBRARY_DIRS = -L$(LIB_DIR)
endif

DEFINES := -DRARCH_INTERNAL -D_FILE_OFFSET_BITS=64 -UHAVE_STATIC_DUMMY
DEFINES += -DHAVE_C99=1 -DHAVE_CXX=1
DEFINES += -DHAVE_GETOPT_LONG=1 -DHAVE_STRCASESTR=1 -DHAVE_DYNAMIC=1 -DHAVE_OSS
ifeq ($(NATIVE),1)
DEFINES += -DHAVE_SDL2
else
DEFINES += -DHAVE_AUDIOIO -DHAVE_ARM_NEON_ASM_OPTIMIZATIONS
endif
DEFINES += -DHAVE_FILTERS_BUILTIN

# ifeq ($(ADD_NETWORKING),1)
# DEFINES += -DHAVE_ONLINE_UPDATER=1 -DHAVE_UPDATE_ASSETS=1
# endif

ifeq ($(NATIVE),1)
SDL2_CFLAGS := $(shell sdl2-config --cflags 2>/dev/null)
SDL2_LIBS := $(shell sdl2-config --libs 2>/dev/null)
SDL_DINGUX_CFLAGS :=
SDL_DINGUX_LIBS :=
FREETYPE_CFLAGS := $(shell pkg-config freetype2 --cflags 2>/dev/null)
FREETYPE_LIBS := $(shell pkg-config freetype2 --libs 2>/dev/null)
else
SDL2_CFLAGS :=
SDL2_LIBS :=
SDL_DINGUX_CFLAGS := $(shell $(SDL_CONFIG) --cflags)
SDL_DINGUX_LIBS := $(shell $(SDL_CONFIG) --libs)
FREETYPE_CFLAGS := $(shell $(FREETYPE_CONFIG) --cflags)
FREETYPE_LIBS := $(shell $(FREETYPE_CONFIG) --libs)
endif
MMAP_LIBS = -lc

OBJDIR_BASE := obj-unix

ifeq ($(DEBUG), 1)
   OBJDIR := $(OBJDIR_BASE)/debug
   DEF_FLAGS += -O0 -g -DDEBUG -D_DEBUG
else
   OBJDIR := $(OBJDIR_BASE)/release
   DEF_FLAGS += -Ofast -DNDEBUG
endif

include Makefile.common

DEF_FLAGS += $(INCLUDE_DIRS)
LDFLAGS += $(CFLAGS) $(CXXFLAGS) $(DEF_FLAGS)
CFLAGS += $(DEF_FLAGS)
CXXFLAGS += $(DEF_FLAGS)

HEADERS = $(wildcard */*/*.h) $(wildcard */*.h) $(wildcard *.h)

Q := @

RARCH_OBJ := $(addprefix $(OBJDIR)/,$(OBJ))

all: $(TARGET)

-include $(RARCH_OBJ:.o=.d)

SYMBOL_MAP := -Wl,-Map=output.map

$(TARGET): $(RARCH_OBJ)
	@$(if $(Q), $(shell echo echo LD $@),)
	$(Q)$(LINK) -o $@ $(RARCH_OBJ) $(LIBS) $(LDFLAGS) $(LIBRARY_DIRS)

ifeq ($(STRIP_BIN),1)
	$(STRIP) --strip-unneeded $(TARGET)
endif

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@$(if $(Q), $(shell echo echo CC $<),)
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) $(DEFINES) -c -o $@ $<

$(OBJDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	@$(if $(Q), $(shell echo echo CXX $<),)
	$(Q)$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEFINES) -MMD -c -o $@ $<

$(OBJDIR)/%.o: %.m
	@mkdir -p $(dir $@)
	@$(if $(Q), $(shell echo echo OBJC $<),)
	$(Q)$(CXX) $(OBJCFLAGS) $(DEFINES) -MMD -c -o $@ $<

$(OBJDIR)/%.o: %.S $(HEADERS)
	@mkdir -p $(dir $@)
	@$(if $(Q), $(shell echo echo AS $<),)
	$(Q)$(CC) $(CFLAGS) $(ASFLAGS) $(DEFINES) -c -o $@ $<

clean:
	rm -rf $(OBJDIR_BASE)
	rm -f $(TARGET)
	rm -f *.d

.PHONY: all clean

print-%:
	@echo '$*=$($*)'
