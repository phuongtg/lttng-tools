AM_CFLAGS = -g -Wall -I../
AM_LDFLAGS = -lurcu -lurcu-cds

EXTRA_DIST = runall.sh

noinst_PROGRAMS = test_sessions test_kernel_data_trace

UTILS=utils.h
SESSIONS=$(top_srcdir)/src/bin/lttng-sessiond/session.c
KERN_DATA_TRACE=$(top_srcdir)/src/bin/lttng-sessiond/trace-kernel.c
COMMON=$(top_builddir)/src/common/libcommon.la
HASHTABLE=$(top_builddir)/src/common/hashtable/libhashtable.la

# Tracing sessions unit tests
test_sessions_SOURCES = test_sessions.c $(UTILS) $(SESSIONS)
test_sessions_LDADD = $(COMMON) $(HASHTABLE)

# Kernel trace data unit tests
test_kernel_data_trace_SOURCES = test_kernel_data_trace.c $(UTILS) $(KERN_DATA_TRACE)

if HAVE_LIBLTTNG_UST_CTL
noinst_PROGRAMS += test_ust_data_trace
UST_DATA_TRACE=$(top_srcdir)/src/bin/lttng-sessiond/trace-ust.c
# UST trace data unit tests
test_ust_data_trace_SOURCES = test_ust_data_trace.c $(UTILS) $(UST_DATA_TRACE)
test_ust_data_trace_LDADD = $(COMMON) $(HASHTABLE)
endif