#
# Build kernel
#

include Config.mk

OBJS		:=	src/main.o \
				src/kernel.o \
				src/config.o \
				src/midiparser.o \
				src/mt32synth.o \
				src/rommanager.o \
				src/lcd/hd44780.o \
				src/lcd/hd44780fourbit.o \
				src/lcd/hd44780i2c.o \
				src/lcd/mt32lcd.o \
				src/lcd/ssd1306.o

EXTRACLEAN	+=	src/*.d src/*.o \
				src/lcd/*.d src/lcd/*.o

#
# inih
#
OBJS		+=	$(INIHHOME)/ini.o
INCLUDE		+=	-I $(INIHHOME)
EXTRACLEAN	+=	$(INIHHOME)/ini.d \
				$(INIHHOME)/ini.o

include $(CIRCLEHOME)/Rules.mk

CFLAGS		+=	-Werror -Wextra -Wno-unused-parameter

CFLAGS		+=	-I "$(NEWLIBDIR)/include" \
				-I $(STDDEF_INCPATH) \
				-I $(CIRCLESTDLIBHOME)/include \
				-I include \
				-I .

LIBS 		:=	$(CIRCLE_STDLIB_LIBS) \
				$(CIRCLEHOME)/addon/fatfs/libfatfs.a \
				$(CIRCLEHOME)/addon/SDCard/libsdcard.a \
				$(CIRCLEHOME)/addon/wlan/hostap/wpa_supplicant/libwpa_supplicant.a \
				$(CIRCLEHOME)/addon/wlan/libwlan.a \
				$(CIRCLEHOME)/lib/fs/libfs.a \
				$(CIRCLEHOME)/lib/input/libinput.a \
				$(CIRCLEHOME)/lib/libcircle.a \
				$(CIRCLEHOME)/lib/net/libnet.a \
				$(CIRCLEHOME)/lib/sched/libsched.a \
				$(CIRCLEHOME)/lib/usb/libusb.a

ifeq ($(HDMI_CONSOLE), 1)
DEFINE		+=	-D HDMI_CONSOLE
endif

-include $(DEPS)

INCLUDE		+=	-I $(MT32EMUBUILDDIR)/include
EXTRALIBS	+=	$(MT32EMULIB)

#
# Generate version string from git tag
#
VERSION=$(shell git describe --tags --dirty --always 2>/dev/null)
ifneq ($(VERSION),)
DEFINE		+=	-D MT32_PI_VERSION=\"$(VERSION)\"
endif
