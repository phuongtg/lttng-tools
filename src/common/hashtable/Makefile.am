AM_CPPFLAGS = -I$(top_srcdir)/include

noinst_LTLIBRARIES = libhashtable.la

libhashtable_la_SOURCES = hashtable.c hashtable.h \
                         utils.c utils.h \
                         rculfhash-internal.h urcu-flavor.h \
                         rculfhash.h rculfhash.c \
                         rculfhash-mm-chunk.c \
                         rculfhash-mm-mmap.c \
                         rculfhash-mm-order.c

libhashtable_la_LIBADD = -lurcu-common -lurcu
