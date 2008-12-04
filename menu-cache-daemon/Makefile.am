NULL = 

INCLUDES =				\
	-I$(top_srcdir)/libmenu-cache	\
	$(GLIB_CFLAGS)			\
	$(MONITOR_CFLAGS)		\
	$(DEBUG_CFLAGS)			\
	$(WARN_CFLAGS)			\
        -DBINDIR=\"$(bindir)\"          \
	-Werror-implicit-function-declaration \
	$(NULL)

bin_PROGRAMS = menu-cached

menu_cached_SOURCES =		\
	menu-cached.c			\
	$(NULL)

menu_cached_LDADD = 		\
	$(top_builddir)/libmenu-cache/libmenu-cache.la	\
	$(GLIB_LIBS)					\
	$(NULL)
menu_cached_LDFLAGS =			\
	-no-undefined				\
	$(NULL)
EXTRA_DIST =				\
	$(NULL)
