AM_CPPFLAGS = -I$(top_srcdir)/include

noinst_LTLIBRARIES = libcompat.la

if COMPAT_EPOLL
COMPAT=compat-epoll.c
else
COMPAT=compat-poll.c
endif

libcompat_la_SOURCES = poll.h fcntl.h endian.h mman.h clone.h \
                       socket.h compat-fcntl.c $(COMPAT)
