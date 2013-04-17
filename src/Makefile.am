bin_PROGRAMS = gusher
gusher_SOURCES = main.c postgres.c gtime.c cache.c json.c template.c gnotify.c log.c http.c

lib1dir = /var/lib/gusher
lib1_SCRIPTS = boot.scm

install-data-local:
	groupadd -f gusher
	mkdir -p $(lib1dir)/kc
	mkdir -p $(lib1dir)/signals
	chown -R root:gusher $(lib1dir)
	chmod -R g+sw $(lib1dir)

AM_CFLAGS = -Wall -I/usr/local/include/guile/2.0 -I/usr/include/libxml2 -DLOCALLIB=\"$(libdir)\"
