NULL =

SUBDIRS_DOCS = docs

EXTRA_DIST_DOCS = \
	gtk-doc.make \
	docs/Makefile.* \
	docs/reference/Makefile.* \
	docs/reference/libmenu-cache/Makefile.* \
	$(NULL)

ALL_SUBDIRS = 			\
	libmenu-cache		\
	menu-cache-gen		\
	menu-cache-daemon	\
	$(NULL)

SUBDIRS = $(ALL_SUBDIRS)
DIST_SUBDIRS = $(ALL_SUBDIRS)

EXTRA_DIST = 			\
	$(NULL)

if ENABLE_GTK_DOC
DIST_SUBDIRS += $(SUBDIRS_DOCS)
SUBDIRS += $(SUBDIRS_DOCS)
else
EXTRA_DIST += $(EXTRA_DIST_DOCS)
endif
