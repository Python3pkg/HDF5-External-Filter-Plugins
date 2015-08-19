##                                               -*- Autoconf -*-
## Process this file with autoconf to produce a configure script.
##
## Copyright by The HDF Group.
## Copyright by the Board of Trustees of the University of Illinois.
## All rights reserved.
##
## This file is part of HDF5.  The full HDF5 copyright notice, including
## terms governing use, modification, and redistribution, is contained in
## the files COPYING and Copyright.html.  COPYING can be found at the root
## of the source code distribution tree; Copyright.html can be found at the
## root level of an installed copy of the electronic HDF5 document set and
## is linked from the top-level documents page.  It can also be found at
## http://hdfgroup.org/HDF5/doc/Copyright.html.  If you do not have
## access to either file, you may request a copy from help@hdfgroup.org.

AC_PREREQ(2.59)
AC_INIT([HDF5 LZ4 filter plugin], [0.1.0], help@hdfgroup.org)
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_AUX_DIR([bin])
## AM_INIT_AUTOMAKE takes a list of options that should be applied to
## every Makefile.am when automake is run.
AM_INIT_AUTOMAKE
AM_CONFIG_HEADER(config.h)

## Turn off automake rebuild rules so make doesn't try to run automake
## (which probably won't work).
AM_MAINTAINER_MODE

# Checks for programs.
AC_PROG_CC
AM_PROG_CC_C_O
AC_PROG_INSTALL
PKG_PROG_PKG_CONFIG

AC_CANONICAL_HOST
AC_PREFIX_DEFAULT([`pwd`/plugins])

LT_INIT([dlopen])

## Source any special files that we need. These files normally aren't
## present but can be used by the maintainers to fine tune things like
## turning on debug or profiling flags for the compiler. The search order
## is:
##
##         CPU-VENDOR-OS
##         VENDOR-OS
##         CPU-OS
##         CPU-VENDOR
##         OS
##         VENDOR
##         CPU
##
## If the `OS' ends with a version number then remove it. For instance,
## `freebsd3.1' would become `freebsd'
##
case "$host_os" in
  aix4.*)       host_os_novers="aix4.x"     ;;
  aix5.*)       host_os_novers="aix5.x"     ;;
  freebsd*)     host_os_novers="freebsd"    ;;
  irix5.*)      host_os_novers="irix5.x"    ;;
  irix6.*)      host_os_novers="irix6.x"    ;;
  osf4.*)       host_os_novers="osf4.x"     ;;
  osf5.*)       host_os_novers="osf5.x"     ;;
  solaris2.*)   host_os_novers="solaris2.x" ;;
  *)            host_os_novers="$host_os"   ;;
esac

host_config="none"
for f in $host_cpu-$host_vendor-$host_os        \
         $host_cpu-$host_vendor-$host_os_novers \
         $host_vendor-$host_os                  \
         $host_vendor-$host_os_novers           \
         $host_cpu-$host_os                     \
         $host_cpu-$host_os_novers              \
         $host_cpu-$host_vendor                 \
         $host_os                               \
         $host_os_novers                        \
         $host_vendor                           \
         $host_cpu ; do
  AC_MSG_CHECKING([for config $f])
  if test -f "$srcdir/config/$f"; then
    host_config=$srcdir/config/$f
    AC_MSG_RESULT([found])
    break
  fi
  AC_MSG_RESULT([no])
done
if test "X$host_config" != "Xnone"; then
  CC_BASENAME="`echo $CC | cut -f1 -d' ' | xargs basename 2>/dev/null`"
  . $host_config
fi

## Fix up the INSTALL macro if its a relative path. We want the
## full-path to the binary instead.
case "$INSTALL" in
  *install-sh*)
    INSTALL='\${top_srcdir}/bin/install-sh -c'
    ;;
esac

## ----------------------------------------------------------------------
## Check which archiving tool to use. This needs to be done before
## the AC_PROG_LIBTOOL macro.
##
if test -z "$AR"; then
  AC_CHECK_PROGS([AR], [ar xar], [:], [$PATH])
fi

AC_SUBST([AR])

## Export the AR macro so that it will be placed in the libtool file
## correctly.
export AR

AC_PROG_MAKE_SET
AC_LIBTOOL_DLOPEN
AC_PROG_LIBTOOL

## Post processing to patch up some deficiencies in libtool
case $host_os in
  linux*)
    # If gcc is not used, need to set $wl to use "-Wl,"
    if $CC -v 2>&1 | grep '^gcc' > /dev/null ; then
      : using gcc
    else
      echo 'fixing $wl in' $ofile
ed - $ofile <<EOF 2> /dev/null
/^wl=""/s//wl="-Wl,"/
w
q
EOF

    fi
    ;;
esac



## ----------------------------------------------------------------------
## Fake --with-xxx option to allow us to create a help message for the
## following --with-xxx options which can take either a =DIR or =INC,LIB
## specifier.
##
AC_ARG_WITH([fnord],
  [
 For the following --with-xxx options, you can specify where the header
 files and libraries are in two different ways:

    --with-xxx=INC,LIB - Specify individually the include directory and
                         library directory separated by a comma
    --with-xxx=DIR     - Specify only the directory which contains the
                         include/ and lib/ subdirectories
  ],,)

## ----------------------------------------------------------------------
## Where's HDF5? We need to find out where the HDF5 library
## and header files are if they aren't in system library/include places.
##
## The only reason we need the HDF5 library is to run the tests.
##
## This needs some improvement if we cannot find the HDF5 library via 
## pkg-config.
PKG_CHECK_MODULES([HDF5],[hdf5],
                  [HDF5_FOUND=1 ],
                  [AC_MSG_ERROR(Could not find HDF5 - required for plugin test!)])



## Add the suffix to CC for shared linking.  Cannot just set as an option
## because it must be first.
if test "$shared_suffix" && test ! `echo $CC | grep "$shared_suffix"`; then
    CC=${CC}${shared_suffix}
fi

## ======================================================================
## Checks for library functions
## ======================================================================

## Checks for libraries.

## Checks for header files.
AC_HEADER_STDC
AC_CHECK_HEADERS([fcntl.h stdlib.h string.h])

## Checks for typedefs, structures, and compiler characteristics.
AC_C_CONST
AC_TYPE_SIZE_T

## Checks for library functions.
AC_FUNC_ERROR_AT_LINE
##AC_FUNC_MALLOC
AC_FUNC_MEMCMP
AC_FUNC_VPRINTF
AC_CHECK_FUNCS([memset])

## Check which plugins to build, if no environmental variables are set, build
## all.
if test ! "$PLUGIN_H5LZ4"
then
  PLUGIN_H5LZ4=1
fi
AM_CONDITIONAL(H5LZ4, test "$PLUGIN_H5LZ4")

LT_OUTPUT

## Set subdirectories
AC_CONFIG_FILES([makefile src/makefile example/makefile example/test.sh])

## Configure
AC_OUTPUT
