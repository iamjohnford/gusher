bin_PROGRAMS = gusher
gusher_SOURCES = main.c postgres.c gtime.c cache.c json.c template.c gnotify.c log.c http.c

lib1dir = $(libdir)/gusher
lib1_SCRIPTS = boot.scm

AM_CFLAGS = -Wall -I/usr/include/libxml2 -DLOCALLIB=\"$(libdir)\"
