/*-
 * Copyright (c) 2026 Bertrand Lebonnois
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "archive_platform.h"

#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif
#include <stdio.h>
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif
#if HAVE_ZXC_H
#include <zxc.h>
#endif

#include "archive.h"
#include "archive_endian.h"
#include "archive_private.h"
#include "archive_read_private.h"

/*
 * Streaming decompression driven by libzxc's push API (zxc_pstream.h):
 *  - bid checks the 4-byte magic (0x9CB02EF5 LE) without allocating;
 *  - init creates a dstream + an output staging buffer;
 *  - read pulls upstream chunks via __archive_read_filter_ahead and feeds
 *    them to zxc_dstream_decompress, returning each freshly produced batch.
 *
 * Memory footprint stays bounded (≈ block_size + framing) regardless of
 * archive size.
 */
#define ZXC_MAGIC_LE32 0x9CB02EF5U

#if HAVE_ZXC_H && HAVE_LIBZXC

struct private_data {
	zxc_dstream	*ds;
	uint8_t		*out_buf;
	size_t		 out_cap;
};

static ssize_t	zxc_filter_read(struct archive_read_filter *, const void **);
static int	zxc_filter_close(struct archive_read_filter *);
#endif

static int	zxc_bidder_bid(struct archive_read_filter_bidder *,
		    struct archive_read_filter *);
static int	zxc_bidder_init(struct archive_read_filter *);

static const struct archive_read_filter_bidder_vtable
zxc_bidder_vtable = {
	.bid = zxc_bidder_bid,
	.init = zxc_bidder_init,
};

int
archive_read_support_filter_zxc(struct archive *_a)
{
	struct archive_read *a = (struct archive_read *)_a;

	if (__archive_read_register_bidder(a, NULL, "zxc",
				&zxc_bidder_vtable) != ARCHIVE_OK)
		return (ARCHIVE_FATAL);

#if HAVE_ZXC_H && HAVE_LIBZXC
	return (ARCHIVE_OK);
#else
	archive_set_error(_a, ARCHIVE_ERRNO_MISC,
	    "Using external zxc program for zxc decompression");
	return (ARCHIVE_WARN);
#endif
}

static int
zxc_bidder_bid(struct archive_read_filter_bidder *self,
    struct archive_read_filter *filter)
{
	const unsigned char *buffer;
	ssize_t avail;
	uint32_t magic;

	(void)self; /* UNUSED */

	buffer = __archive_read_filter_ahead(filter, 4, &avail);
	if (buffer == NULL || avail < 4)
		return (0);

	magic = archive_le32dec(buffer);
	if (magic == ZXC_MAGIC_LE32)
		return (32);
	return (0);
}

#if !(HAVE_ZXC_H && HAVE_LIBZXC)

/*
 * Without libzxc, fall back to the external "zxc -d" program.
 */
static int
zxc_bidder_init(struct archive_read_filter *self)
{
	int r;

	r = __archive_read_program(self, "zxc -d");
	self->code = ARCHIVE_FILTER_ZXC;
	self->name = "zxc";
	return (r);
}

#else

static const struct archive_read_filter_vtable
zxc_reader_vtable = {
	.read = zxc_filter_read,
	.close = zxc_filter_close,
};

static int
zxc_bidder_init(struct archive_read_filter *self)
{
	struct private_data *state;
	zxc_decompress_opts_t opts;

	self->code = ARCHIVE_FILTER_ZXC;
	self->name = "zxc";

	state = calloc(1, sizeof(*state));
	if (state == NULL) {
		archive_set_error(&self->archive->archive, ENOMEM,
		    "Can't allocate data for zxc decompression");
		return (ARCHIVE_FATAL);
	}

	memset(&opts, 0, sizeof(opts));
	opts.checksum_enabled = 1;
	state->ds = zxc_dstream_create(&opts);
	if (state->ds == NULL) {
		free(state);
		archive_set_error(&self->archive->archive, ENOMEM,
		    "Can't allocate zxc decompression stream");
		return (ARCHIVE_FATAL);
	}
	/* Pre-size output to one default block; grown lazily after the file
	 * header is parsed (zxc_dstream_out_size returns 0 until then). */
	state->out_cap = 256 * 1024;
	state->out_buf = malloc(state->out_cap);
	if (state->out_buf == NULL) {
		zxc_dstream_free(state->ds);
		free(state);
		archive_set_error(&self->archive->archive, ENOMEM,
		    "Can't allocate zxc output buffer");
		return (ARCHIVE_FATAL);
	}

	self->data = state;
	self->vtable = &zxc_reader_vtable;
	return (ARCHIVE_OK);
}

/*
 * libarchive read filters return one chunk per call.  We sit on top of
 * upstream and pump compressed bytes into the dstream until it produces
 * decompressed output (or the upstream is exhausted).
 */
static ssize_t
zxc_filter_read(struct archive_read_filter *self, const void **p)
{
	struct private_data *state = (struct private_data *)self->data;
	zxc_outbuf_t out;
	zxc_inbuf_t in;
	ssize_t avail_in;
	const void *src;

	out.dst = state->out_buf;
	out.size = state->out_cap;
	out.pos = 0;

	for (;;) {
		src = __archive_read_filter_ahead(self->upstream, 1, &avail_in);
		if (avail_in < 0)
			return (avail_in);

		in.src = (avail_in > 0) ? src : NULL;
		in.size = (avail_in > 0) ? (size_t)avail_in : 0;
		in.pos = 0;

		int64_t r = zxc_dstream_decompress(state->ds, &out, &in);
		if (in.pos > 0)
			__archive_read_filter_consume(self->upstream,
			    (int64_t)in.pos);
		if (r < 0) {
			archive_set_error(&self->archive->archive,
			    ARCHIVE_ERRNO_MISC,
			    "zxc decompression failed: %s",
			    zxc_error_name((int)r));
			return (ARCHIVE_FATAL);
		}
		if (out.pos > 0) {
			*p = state->out_buf;
			return ((ssize_t)out.pos);
		}
		/* No output yet — if upstream is exhausted and the dstream
		 * has either reached DONE or made no progress, return 0. */
		if (avail_in == 0) {
			if (!zxc_dstream_finished(state->ds)) {
				archive_set_error(&self->archive->archive,
				    ARCHIVE_ERRNO_MISC,
				    "Truncated zxc input");
				return (ARCHIVE_FATAL);
			}
			*p = NULL;
			return (0);
		}
		/* Otherwise loop and pull more upstream bytes. */
	}
}

static int
zxc_filter_close(struct archive_read_filter *self)
{
	struct private_data *state = (struct private_data *)self->data;

	zxc_dstream_free(state->ds);
	free(state->out_buf);
	free(state);
	return (ARCHIVE_OK);
}

#endif /* HAVE_ZXC_H && HAVE_LIBZXC */
