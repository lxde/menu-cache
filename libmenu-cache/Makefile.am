NULL =

INCLUDES =				\
	$(GLIB_CFLAGS)			\
	$(MONITOR_CFLAGS)		\
	$(DEBUG_CFLAGS) \
	$(ADDITIONAL_FLAGS) \
	-Werror-implicit-function-declaration \
	-DMENUCACHE_LIBEXECDIR="\"$(pkglibexecdir)\"" \
	$(NULL)

lib_LTLIBRARIES = libmenu-cache.la

libmenu_cache_la_SOURCES =		\
	version.h			\
	menu-cache.c			\
	$(MONITOR_SOURCES)		\
	$(NULL)

libmenu_cache_la_LIBADD = 		\
	$(GLIB_LIBS)			\
	$(MONITOR_LIBS)			\
	$(NULL)
libmenu_cache_la_LDFLAGS =			\
	-no-undefined				\
	-export-symbols-regex menu_cache	\
	-version-info 3:2:0 \
	$(NULL)

lib_menu_cache_includedir = $(includedir)/menu-cache

lib_menu_cache_include_HEADERS =		\
	menu-cache.h			\
	$(NULL)

EXTRA_DIST =				\
        libmenu-cache.pc.in		\
        libmenu-cache-uninstalled.pc.in	\
	$(NULL)

pkgconfigdir   = $(libdir)/pkgconfig
pkgconfig_DATA = libmenu-cache.pc
