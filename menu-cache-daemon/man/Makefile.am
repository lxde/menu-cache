
dist_man_MANS = menu-cached.1

EXTRA_DIST = menu-cached.sgml

dist-hook: menu-cached.1

menu-cached.1: menu-cached.sgml
	(test -f "`which docbook-to-man`" && docbook-to-man $< > $@) || \
	(test -f "`which db2man`" && db2man $< > $@) || \
	(test -f "`which docbook2man`" && docbook2man $< > $@)
