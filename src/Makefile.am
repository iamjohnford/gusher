bin_PROGRAMS = gusher
gusher_SOURCES = main.c postgres.c gtime.c cache.c json.c template.c gnotify.c log.c http.c butter.c messaging.c

lib1dir = /var/lib/gusher
lib1_SCRIPTS = boot.scm

#install-data-local:
#	groupadd -f gusher
#	mkdir -p $(lib1dir)/kc
#	mkdir -p $(lib1dir)/signals
#	chown -R root:gusher $(lib1dir)
#	chmod -R g+sw $(lib1dir)
#	mkdir -p /var/log/gusher
#	chown -R root:gusher /var/log/gusher
#	chmod -R g+sw /var/log/gusher

AM_CFLAGS = -Wall -I/usr/include/guile/2.0 -I/usr/include/postgresql -I/usr/include/libxml2 -DLOCALLIB=\"$(libdir)\"
