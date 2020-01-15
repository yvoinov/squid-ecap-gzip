EXTRA_DIST = \
	adapter_gzip.cc

lib_LTLIBRARIES = \
	ecap_adapter_gzip.la

noinst_HEADERS = \
	adapter_gzip.h \
	autoconf.h 

ecap_adapter_gzip_la_SOURCES = adapter_gzip.cc
ecap_adapter_gzip_la_LDFLAGS = -module -version-info @LIBADAPTER_CURRENT@:@LIBADAPTER_REVISION@:@LIBADAPTER_AGE@
ecap_adapter_gzip_la_LIBADD = -lecap -lz

DISTCLEANFILES = \
        autoconf.h \
	adapter_gzip.h \
	Makefile

AM_CXXFLAGS = @CXXFLAGS@ -mtune=native
AM_CPPFLAGS = -I$(top_srcdir)/src
