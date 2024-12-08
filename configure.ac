#                                               -*- Autoconf -*-
#  @configure_input@
#
#  Configuration script for eCAP GZip/DEFLATE Library
#
# Process this file with autoconf to produce a configure script.

m4_define([VERSION_MAJOR],[1])
m4_define([VERSION_MINOR],[6])
m4_define([VERSION_MICRO],[0])
m4_define([VERSION_PATCH],[9])

AC_INIT(squid-ecap-gzip, m4_defn([VERSION_MAJOR]).m4_defn([VERSION_MINOR]).m4_defn([VERSION_MICRO]).m4_defn([VERSION_PATCH]), [], squid-ecap-gzip)

LIBADAPTER_CURRENT=6
LIBADAPTER_REVISION=4
LIBADAPTER_AGE=1

# 1.6.0.3 had 6:0:1
# 1.6.0.4 had 6:0:1
# 1.6.0.5 had 6:1:1
# 1.6.0.7 had 6:2:1
# 1.6.0.8 had 6:3:1
# 1.6.0.9 had 6:4:1

#   Current  -- the number of the binary API that we're implementing
#   Revision -- which iteration of the implementation of the binary
#               API are we supplying?
#   Age      -- How many previous binary API versions do we also
#               support?
#
# If we release a new version that does not change the binary API,
# increment Revision.
#
# If we release a new version that changes the binary API, but does
# not break programs compiled against the old binary API, increment
# Current and Age.  Set Revision to 0, since this is the first
# implementation of the new API.
#
# Otherwise, we're changing the binary API and breaking backward
# compatibility with old binaries.  Increment Current.  Set Age to 0,
# since we're backward compatible with no previous APIs.  Set Revision
# to 0 too.
AC_SUBST(LIBADAPTER_CURRENT)
AC_SUBST(LIBADAPTER_REVISION)
AC_SUBST(LIBADAPTER_AGE)

AC_CONFIG_AUX_DIR(cfgaux)
AC_CONFIG_MACRO_DIR([acinclude])
AC_CONFIG_HEADERS([src/autoconf.h])
AC_CONFIG_FILES([Makefile src/Makefile src/adapter_gzip.h])

AM_INIT_AUTOMAKE([subdir-objects])
AM_MAINTAINER_MODE

LT_INIT

dnl http://www.gnu.org/savannah-checkouts/gnu/autoconf/manual/autoconf-2.69/html_node/C_002b_002b-Compiler.html
: ${CXXFLAGS=""}
PRESET_CXXFLAGS="$CXXFLAGS"

dnl Checks for programs
AC_PROG_CXX([clang++ g++])
AC_LANG([C++])
AX_CXX_HAVE_STL
AX_CXX_HEADER_STDCXX_TR1
AC_PROG_CPP
AC_PROG_INSTALL
AC_PROG_LN_S
AC_PROG_MAKE_SET

AC_PROG_GREP
if test "x$ac_cv_path_GREP" = "xno"; then
  AC_MSG_ERROR([grep was not found; install it first])
fi

dnl Check STL
if test "x$ax_cv_cxx_have_stl" = "xno"; then
  AC_MSG_ERROR([STL was not found; it is require to build])
fi

dnl Check TR1
if test "x$ax_cv_cxx_stdcxx_tr1" = "xno"; then
  AC_MSG_ERROR([ISO C++ TR1 header files was not found; it is require to build])
fi

dnl If the user did not specify a C++ version.
user_cxx=`echo "$PRESET_CXXFLAGS" | $GREP -o -E "\\-std="`
if test "x$user_cxx" = "x"; then
  dnl Check for C++11 compiler support
  AX_CXX_COMPILE_STDCXX(11, [noext], [mandatory])
fi

dnl If the user did not specify optimization level
user_opt=`echo "$PRESET_CXXFLAGS" | $GREP -o -E "\\-O"`
if test "x$user_opt" = "x"; then
  dnl Set default optimization level
  CXXFLAGS="$CXXFLAGS -O3"
fi

dnl Checks for libraries
AC_CHECK_LIB([ecap], [main],
    [LIBS="-lecap $LIBS"],
    [AC_MSG_FAILURE([libecap is not found or unusable])]
)

AC_CHECK_LIB([z], [main],
    [LIBS="-lz $LIBS"],
    [AC_MSG_FAILURE([zlib is not found or unusable])]
)

dnl Checks for header files.
AC_CHECK_HEADERS([libecap/adapter/service.h])
AC_CHECK_HEADERS([zlib.h])

AC_PATH_TOOL(PKG_CONFIG, pkg-config)

# Libecap compatibility check
if ! $PKG_CONFIG --atleast-version=1.0 libecap; then
    AC_MSG_ERROR(Check PKG_CONFIG_PATH points on libecap.pc dir or get latest libecap from e-cap.org)
fi

dnl Check LTO (GCC)
AX_CHECK_COMPILE_FLAG([-flto], [CXXFLAGS="$CXXFLAGS -flto"; ac_cv_lto_supported=yes], [ac_cv_lto_supported=no], [])

if test x"$ac_cv_lto_supported" = xno; then
 AC_MSG_NOTICE([LTO/Link optimization not supported])
fi

AC_SUBST(AM_CXXFLAGS)
AC_SUBST(AM_LIBS)
AC_MSG_NOTICE([BUILD C++ FLAGS: $CXXFLAGS])
AC_MSG_NOTICE([BUILD LDFLAGS: $LDFLAGS])
AC_OUTPUT
