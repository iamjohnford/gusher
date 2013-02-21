#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.69])
AC_INIT([gusher], [0.1], [pmy@virginia.edu])
AM_INIT_AUTOMAKE([foreign -Wall -Werror])
AC_CONFIG_SRCDIR([src/main.c])
AC_CONFIG_HEADERS([config.h])

AC_CONFIG_FILES([src/Makefile Makefile])
# Checks for programs.
AC_PROG_CC

# Checks for libraries.
AC_CHECK_LIB([pthread], [pthread_cancel])
AC_CHECK_LIB([microhttpd], [MHD_start_daemon])
AC_CHECK_LIB([guile], [scm_boot_guile])
AC_CHECK_LIB([pq], [PQconnectdb])
AC_CHECK_LIB([jansson], [json_string])
AC_CHECK_LIB([uuid], [uuid_generate])

# Checks for header files.
AC_CHECK_HEADERS([arpa/inet.h fcntl.h netdb.h netinet/in.h stdlib.h string.h sys/ioctl.h sys/socket.h sys/time.h unistd.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_CHECK_HEADER_STDBOOL
AC_C_INLINE
AC_TYPE_SIZE_T

# Checks for library functions.
AC_FUNC_FORK
AC_FUNC_MALLOC
AC_CHECK_FUNCS([gethostbyname gettimeofday localtime_r memset pow socket strstr])

AC_OUTPUT
