## Process this file with automake to produce Makefile.in

ACLOCAL_AMFLAGS = -I acinclude

SUBDIRS	= src
DIST_SUBDIRS = src

dist-hook:
	rm -f `find $(distdir)/src -name *.h`
	rm -f `find $(distdir)/lib -name Makefile`
	rm -f `find $(distdir)/lib -name *.o`
	rm -f `find $(distdir)/lib -name *.lo`
	rm -Rf `find $(distdir)/lib -name .d*`
	rm -Rf `find $(distdir)/lib -name .libs`

EXTRA_DIST = \
	NOTICE \
	CREDITS \
	README* \
	LICENSE \
	ChangeLog \
	create_logs.sh \
	bootstrap.sh

DISTCLEANFILES = \
        _configs.sed
