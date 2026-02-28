
PIDGIN_TREE_TOP ?= ../pidgin-2.10.11
PIDGIN3_TREE_TOP ?= ../pidgin-main
LIBPURPLE_DIR ?= $(PIDGIN_TREE_TOP)/libpurple
WIN32_DEV_TOP ?= $(PIDGIN_TREE_TOP)/../win32-dev

WIN32_CC ?= $(WIN32_DEV_TOP)/mingw-4.7.2/bin/gcc

PKG_CONFIG ?= pkg-config
DIR_PERM = 0755
LIB_PERM = 0755
FILE_PERM = 0644
MAKENSIS ?= makensis
XGETTEXT ?= xgettext

CFLAGS	?= -O2 -g -pipe
LDFLAGS ?= 

CFLAGS += -DENABLE_NLS
CFLAGS += -DHAVE_CYRUS_SASL

# Do some nasty OS and purple version detection
ifeq ($(OS),Windows_NT)
  #only defined on 64-bit windows
  PROGFILES32 = ${ProgramFiles(x86)}
  ifndef PROGFILES32
    PROGFILES32 = $(PROGRAMFILES)
  endif
  TARGET = libircv3.dll
  DEST = "$(PROGFILES32)/Pidgin/plugins"
  ICONS_DEST = "$(PROGFILES32)/Pidgin/pixmaps/pidgin/protocols"
  MAKENSIS = "$(PROGFILES32)/NSIS/makensis.exe"
else

  UNAME_S := $(shell uname -s)

  #.. There are special flags we need for OSX
  ifeq ($(UNAME_S), Darwin)
    #
    #.. /opt/local/include and subdirs are included here to ensure this compiles
    #   for folks using Macports.  I believe Homebrew uses /usr/local/include
    #   so things should "just work".  You *must* make sure your packages are
    #   all up to date or you will most likely get compilation errors.
    #
    INCLUDES = -I/opt/local/include -lz $(OS)

    CC = gcc
  else
    INCLUDES = 
    CC ?= gcc
  endif

  ifeq ($(shell $(PKG_CONFIG) --exists purple-3 2>/dev/null && echo "true"),)
    ifeq ($(shell $(PKG_CONFIG) --exists purple 2>/dev/null && echo "true"),)
      TARGET = FAILNOPURPLE
      DEST =
	  ICONS_DEST =
    else
      TARGET = libircv3.so
      DEST = $(DESTDIR)`$(PKG_CONFIG) --variable=plugindir purple`
	  ICONS_DEST = $(DESTDIR)`$(PKG_CONFIG) --variable=datadir purple`/pixmaps/pidgin/protocols
    endif
  else
    TARGET = libircv3-3.so
    DEST = $(DESTDIR)`$(PKG_CONFIG) --variable=plugindir purple-3`
	ICONS_DEST = $(DESTDIR)`$(PKG_CONFIG) --variable=datadir purple-3`/pixmaps/pidgin/protocols
  endif
endif

WIN32_CFLAGS = -I$(WIN32_DEV_TOP)/glib-2.28.8/include -I$(WIN32_DEV_TOP)/glib-2.28.8/include/glib-2.0 -I$(WIN32_DEV_TOP)/glib-2.28.8/lib/glib-2.0/include -I$(WIN32_DEV_TOP)/cyrus-sasl-2.1.26_daa1/include -DENABLE_NLS -DHAVE_CYRUS_SASL -DPACKAGE_VERSION='"$(PLUGIN_VERSION)"' -Wall -Wextra -Werror -Wno-deprecated-declarations -Wno-unused-parameter -fno-strict-aliasing -Wformat -Wno-sign-compare
WIN32_LDFLAGS = -L$(WIN32_DEV_TOP)/glib-2.28.8/lib -L$(WIN32_DEV_TOP)/json-glib-0.14/lib -L$(WIN32_DEV_TOP)/cyrus-sasl-2.1.26_daa1/lib -lpurple -lintl -lglib-2.0 -lgobject-2.0 -lsasl2 -g -ggdb -static-libgcc -lws2_32 -lz
WIN32_PIDGIN2_CFLAGS = -I$(PIDGIN_TREE_TOP)/libpurple -I$(PIDGIN_TREE_TOP) $(WIN32_CFLAGS)
WIN32_PIDGIN3_CFLAGS = -I$(PIDGIN3_TREE_TOP)/libpurple -I$(PIDGIN3_TREE_TOP) -I$(WIN32_DEV_TOP)/gplugin-dev/gplugin $(WIN32_CFLAGS)
WIN32_PIDGIN2_LDFLAGS = -L$(PIDGIN_TREE_TOP)/libpurple $(WIN32_LDFLAGS)
WIN32_PIDGIN3_LDFLAGS = -L$(PIDGIN3_TREE_TOP)/libpurple -L$(WIN32_DEV_TOP)/gplugin-dev/gplugin $(WIN32_LDFLAGS) -lgplugin

C_FILES = \
	cmds.c \
	dcc_send.c \
	irc.c \
	msgs.c \
	parse.c 
PURPLE_COMPAT_FILES := 
PURPLE_C_FILES := $(C_FILES)



.PHONY:	all install FAILNOPURPLE clean translations

all: $(TARGET)

libircv3.so: $(PURPLE_C_FILES) $(PURPLE_COMPAT_FILES)
	$(CC) -fPIC $(CFLAGS) -shared -o $@ $^ $(LDFLAGS) `$(PKG_CONFIG) purple glib-2.0 zlib cyrus-sasl --libs --cflags`  $(INCLUDES) -Ipurple2compat -g -ggdb

libircv3.dll: $(PURPLE_C_FILES) $(PURPLE_COMPAT_FILES)
	$(WIN32_CC) -shared -o $@ $^ $(WIN32_PIDGIN2_CFLAGS) $(WIN32_PIDGIN2_LDFLAGS) -Ipurple2compat


install: $(TARGET)
	mkdir -m $(DIR_PERM) -p $(DEST)
	install -m $(LIB_PERM) -p $(TARGET) $(DEST)

installer: pidgin-ircv3.nsi libircv3.dll
	$(MAKENSIS) "/DPIDGIN_VARIANT"="Pidgin" "/DPRODUCT_NAME"="pidgin-ircv3" "/DINSTALLER_NAME"="pidgin-ircv3-installer" "/DJSON_GLIB_DLL"="libjson-glib-1.0.dll" pidgin-ircv3.nsi

translations: po/purple-ircv3.pot

po/purple-ircv3.pot: $(PURPLE_C_FILES)
	$(XGETTEXT) $^ -k_ --no-location -o $@

po/%.po: po/purple-ircv3.pot
	msgmerge $@ po/purple-ircv3.pot > tmp-$*
	mv -f tmp-$* $@

po/%.mo: po/%.po
	msgfmt -o $@ $^

%-locale-install: po/%.mo
	install -D -m $(FILE_PERM) -p po/$(*F).mo $(LOCALEDIR)/$(*F)/LC_MESSAGES/purple-ircv3.mo
	
FAILNOPURPLE:
	echo "You need libpurple development headers installed to be able to compile this plugin"

clean:
	rm -f $(TARGET)

