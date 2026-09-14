EXTRA_DIST = \
	adapter_gzip_async.cc

lib_LTLIBRARIES = \
	ecap_adapter_gzip.la

noinst_HEADERS = \
	autoconf.h 

ecap_adapter_gzip_la_SOURCES = adapter_gzip_async.h adapter_gzip_async.cc
ecap_adapter_gzip_la_LDFLAGS = -module -version-info @LIBADAPTER_CURRENT@:@LIBADAPTER_REVISION@:@LIBADAPTER_AGE@
ecap_adapter_gzip_la_LIBADD = -lecap -lz

DISTCLEANFILES = \
        autoconf.h \
	Makefile

AM_CXXFLAGS = @CXXFLAGS@ -mtune=native
AM_CPPFLAGS = -I$(top_srcdir)/src
