# Squid eCAP GZIP/DEFLATE adapter
[![License](https://img.shields.io/badge/License-BSD%203--Clause-blue.svg)](https://github.com/yvoinov/squid-ecap-gzip/blob/master/LICENSE)

This Software is an eCAP adapter for HTTP compression with GZIP and DEFLATE.

INSTALLATION
============
The adapter will be installed in /usr/local/lib/ by default.

The libecap library is required to build and use these adapters. You can get
the library from http://www.e-cap.org/. The adapter can be built and
installed from source, usually by running:

```
    % ./configure 'CXXFLAGS=-m32' 'LDFLAGS=-L/usr/local/lib'

	or

    % ./configure 'CXXFLAGS=-m64' 'LDFLAGS=-L/usr/local/lib'

    % make
    % make install-strip
```

Note: LDFLAGS should point on libecap directory.

Note: Adapter requires libecap 1.0.0 or above.
      Configuration checks libecap version; make sure your pkg-config see libecap.pc, set/adjust PKG_CONFIG_PATH otherwise.

CONFIGURATION
=============
Adapter versions starting 1.5.0 configures via ecap_service arguments in squid.conf.

Supported configuration parameters:

```
* maxsize (default 16777216 bytes, i.e. 16 Mb) - maximum compressed file size
* level (default is 6, valid range 0-9) - gzip/deflate global compression level
* errlogname (default path/filename is /var/log/ecap_gzip_err.log)	- arbitrary error log name.
* complogname (default path/filename is /var/log/ecap_gzip_comp.log)	- arbitrary compression log name.
* errlog (default is 0, default path/filename is /var/log/ecap_gzip_err.log) 	- error log
* complog (default is 0, default path/filename is /var/log/ecap_gzip_comp.log)	- compression log
```

Note: errlogname/complogname should be specify with full path and file name. Directory(-ies) should have write permission for proxy.
      If file(s) exists - it will appends. It not exists - will be created.

Adapter logging disabled by default. To enable error log specify errlog=1. To enable compression log specify complog=1.
Proxy must have permissions to write.

Note: When configuration parameters has any error in specifications, adapter starts with defaults. If error log exists,
      diagnostics message will be write.

Example:
--------
```
ecap_enable on
acl gzipmimes rep_mime_type -i "/usr/local/squid/etc/acl.gzipmimes"
loadable_modules /usr/local/lib/ecap_adapter_gzip.so
ecap_service gzip_service respmod_precache ecap://www.thecacheworks.com/ecap_gzip_deflate [maxsize=16777216] [level=6] [errlog=0] [complog=0] bypass=off
adaptation_access gzip_service allow gzipmimes
```

acl.gzipmimes contents:
-----------------------
```
# Note: single "/" produces error in simulators,
#       but works in squid's regex
^application/atom+xml
^application/dash+xml
^application/javascript
^application/json
^application/ld+json
^application/manifest+json
^application/opensearchdescription+xml
^application/rdf+xml
^application/rss+xml
^application/schema+json
^application/soap+xml
^application/vnd.apple.installer+xml
^application/vnd.apple.mpegurl
^application/vnd.geo+json
^application/vnd.google-earth.kml+xml
^application/vnd.mozilla.xul+xml
^application/x-apple-plist
^application/x-javascript
^application/x-mpegurl
^application/x-ns-proxy-autoconfig
^application/x-protobuffer
^application/x-web-app-manifest+json
^application/x-www-form-urlencoded
^application/xop+xml
^application/xhtml+xml
^application/xml
^application/x-yaml
^application/x-cdf
^application/txt
^application/x-sdch-dictionary
^application/x-steam-manifest
^audio/x-mpegurl
^image/svg+xml
^image/x-icon
^text/.*
^video/abst
^video/vnd.mpeg.dash.mpd
```

NOTES
=====
Due to performance reasons, all mime checks executes only once outside adapter, at proxy level.
So, be careful when choose what mime types will be pass into adapter.

Also, HTTP/200 status now checks directly inside adapter. So, this rule:

```
acl HTTP_STATUS_OK http_status 200
adaptation_access gzip_service allow HTTP_STATUS_OK
```

is no longer required.

Also be careful with text/plain mime-type. For some reasons you may be required to remove it from acl,
because of sometimes plain text files can be inadequately big and and can overload the CPU during
decompression. In this case specify "maxsize" which fit you requirements.

** Adapter requires c++11 - compatible C++ compiler to build. **

ADDITIONAL DOCUMENTATION
========================
For eCAP documentation, the libecap library implementation, and support
information, please visit the eCAP project web site: http://www.e-cap.org/

For original eCAP GZIP adapter documentation and support, please visit:
https://github.com/c-rack/squid-ecap-gzip
