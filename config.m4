dnl config.m4 — PHP lumbre extension build configuration

PHP_ARG_ENABLE(lumbre, whether to enable the lumbre extension,
[  --enable-lumbre           Enable the PHP lumbre extension])

PHP_ARG_ENABLE(lumbre-debug, whether to enable lumbre debug mode,
[  --enable-lumbre-debug     Enable debug functions (lumbre_debug_read_buffer)], no, no)

if test "$PHP_LUMBRE" != "no"; then

  dnl Check required POSIX functions
  AC_CHECK_FUNCS([mmap munmap ftruncate clock_gettime])

  dnl Link -lrt conditionally (needed on glibc < 2.17 for clock_gettime)
  AC_CHECK_LIB(rt, clock_gettime, [
    PHP_ADD_LIBRARY(rt, , LUMBRE_SHARED_LIBADD)
  ])

  PHP_SUBST(LUMBRE_SHARED_LIBADD)

  dnl Add include path for lumbre headers
  PHP_ADD_INCLUDE([$ext_srcdir])

  dnl Debug build flag
  if test "$PHP_LUMBRE_DEBUG" != "no"; then
    AC_DEFINE(LUMBRE_DEBUG, 1, [Enable lumbre debug functions])
  fi

  dnl Extension sources — flat list, no glob
  PHP_NEW_EXTENSION(lumbre,
    php_lumbre.c lumbre_ringbuf.c lumbre_span.c lumbre_msgpack.c lumbre_whitelist.c,
    $ext_shared,, -D_GNU_SOURCE -std=c11 -Wall -Wextra)

  dnl Include Makefile.frag for test-unit target
  PHP_ADD_MAKEFILE_FRAGMENT
fi
