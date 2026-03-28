# Makefile.frag — Appended to the phpize-generated Makefile
# Provides the test-unit target for CMocka-based C unit tests.

CMOCKA_FLAGS = -lcmocka
UNIT_CFLAGS  = -std=c11 -Wall -Wextra -g -O0 -I$(srcdir)
UNIT_LDFLAGS = $(CMOCKA_FLAGS)

# Pure C modules (no PHP headers)
UNIT_RINGBUF_SRCS  = $(srcdir)/tests/test_ringbuf.c $(srcdir)/lumbre_ringbuf.c
UNIT_MSGPACK_SRCS  = $(srcdir)/tests/test_msgpack.c $(srcdir)/lumbre_msgpack.c
UNIT_WHITELIST_SRCS = $(srcdir)/tests/test_whitelist.c $(srcdir)/lumbre_whitelist.c
UNIT_SPAN_SRCS     = $(srcdir)/tests/test_span.c $(srcdir)/lumbre_span.c \
                     $(srcdir)/lumbre_ringbuf.c $(srcdir)/lumbre_msgpack.c

test-unit: test-unit-ringbuf test-unit-msgpack test-unit-whitelist test-unit-span

test-unit-ringbuf: $(UNIT_RINGBUF_SRCS)
	$(CC) $(UNIT_CFLAGS) -o $(builddir)/test_ringbuf $^ $(UNIT_LDFLAGS) && \
	$(builddir)/test_ringbuf

test-unit-msgpack: $(UNIT_MSGPACK_SRCS)
	$(CC) $(UNIT_CFLAGS) -o $(builddir)/test_msgpack $^ $(UNIT_LDFLAGS) && \
	$(builddir)/test_msgpack

test-unit-whitelist: $(UNIT_WHITELIST_SRCS)
	$(CC) $(UNIT_CFLAGS) -o $(builddir)/test_whitelist $^ $(UNIT_LDFLAGS) && \
	$(builddir)/test_whitelist

test-unit-span: $(UNIT_SPAN_SRCS)
	$(CC) $(UNIT_CFLAGS) -DLUMBRE_TEST -o $(builddir)/test_span $^ $(UNIT_LDFLAGS) && \
	$(builddir)/test_span

.PHONY: test-unit test-unit-ringbuf test-unit-msgpack test-unit-whitelist test-unit-span
