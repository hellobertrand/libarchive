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
#ifdef HAVE_STDINT_H
#include <stdint.h>
#endif
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif
#ifdef HAVE_ZXC_H
#include <zxc.h>
#endif

#include "archive.h"
#include "archive_private.h"
#include "archive_string.h"
#include "archive_write_private.h"

/*
 * Streaming compression driven by libzxc's push API (zxc_pstream.h):
 *  - open()  → create cstream + bounded output buffer
 *  - write() → push input chunks, drain compressed output to next filter
 *  - close() → call zxc_cstream_end() until done, drain final output
 *  - free()  → release cstream + buffer
 *
 * Memory footprint is constant in the size of the archive (≈ block_size +
 * output buffer ≈ 512 KB) instead of growing with the uncompressed size.
 */

struct private_data {
	int		 compression_level;
#if HAVE_ZXC_H && HAVE_LIBZXC
	int		 checksum_enabled;
	zxc_cstream	*cs;
	uint8_t		*out_buf;
	size_t		 out_cap;
#else
	struct archive_write_program_data *pdata;
#endif
};

/* Default range when libzxc is not available at compile time. */
#define ZXC_CLEVEL_MIN 1
#define ZXC_CLEVEL_DEFAULT 3
#define ZXC_CLEVEL_MAX 5

static int archive_compressor_zxc_options(struct archive_write_filter *,
		    const char *, const char *);
static int archive_compressor_zxc_open(struct archive_write_filter *);
static int archive_compressor_zxc_write(struct archive_write_filter *,
		    const void *, size_t);
static int archive_compressor_zxc_close(struct archive_write_filter *);
static int archive_compressor_zxc_free(struct archive_write_filter *);

/*
 * Add a zxc compression filter to this write handle.
 */
int
archive_write_add_filter_zxc(struct archive *_a)
{
	struct archive_write *a = (struct archive_write *)_a;
	struct archive_write_filter *f = __archive_write_allocate_filter(_a);
	struct private_data *data;

	archive_check_magic(&a->archive, ARCHIVE_WRITE_MAGIC,
	    ARCHIVE_STATE_NEW, "archive_write_add_filter_zxc");

	data = calloc(1, sizeof(*data));
	if (data == NULL) {
		archive_set_error(&a->archive, ENOMEM, "Out of memory");
		return (ARCHIVE_FATAL);
	}
	f->data = data;
	f->open = &archive_compressor_zxc_open;
	f->options = &archive_compressor_zxc_options;
	f->close = &archive_compressor_zxc_close;
	f->free = &archive_compressor_zxc_free;
	f->code = ARCHIVE_FILTER_ZXC;
	f->name = "zxc";
	data->compression_level = ZXC_CLEVEL_DEFAULT;
#if HAVE_ZXC_H && HAVE_LIBZXC
	data->checksum_enabled = 1;
	return (ARCHIVE_OK);
#else
	data->pdata = __archive_write_program_allocate("zxc");
	if (data->pdata == NULL) {
		free(data);
		archive_set_error(&a->archive, ENOMEM, "Out of memory");
		return (ARCHIVE_FATAL);
	}
	archive_set_error(&a->archive, ARCHIVE_ERRNO_MISC,
	    "Using external zxc program");
	return (ARCHIVE_WARN);
#endif
}

static int
archive_compressor_zxc_free(struct archive_write_filter *f)
{
	struct private_data *data = (struct private_data *)f->data;
#if HAVE_ZXC_H && HAVE_LIBZXC
	zxc_cstream_free(data->cs);
	free(data->out_buf);
#else
	__archive_write_program_free(data->pdata);
#endif
	free(data);
	f->data = NULL;
	return (ARCHIVE_OK);
}

static int
string_to_number(const char *string, intmax_t *numberp)
{
	char *end;

	if (string == NULL || *string == '\0')
		return (ARCHIVE_WARN);
	*numberp = strtoimax(string, &end, 10);
	if (end == string || *end != '\0' || errno == EOVERFLOW) {
		*numberp = 0;
		return (ARCHIVE_WARN);
	}
	return (ARCHIVE_OK);
}

/*
 * Set write options.
 */
static int
archive_compressor_zxc_options(struct archive_write_filter *f, const char *key,
    const char *value)
{
	struct private_data *data = (struct private_data *)f->data;

	if (strcmp(key, "compression-level") == 0) {
		intmax_t level;
		int minimum = ZXC_CLEVEL_MIN;
		int maximum = ZXC_CLEVEL_MAX;

		if (string_to_number(value, &level) != ARCHIVE_OK) {
			archive_set_error(f->archive, ARCHIVE_ERRNO_MISC,
			    "compression-level invalid");
			return (ARCHIVE_FAILED);
		}
#if HAVE_ZXC_H && HAVE_LIBZXC
		minimum = zxc_min_level();
		maximum = zxc_max_level();
#endif
		if (level < minimum || level > maximum) {
			archive_set_error(f->archive, ARCHIVE_ERRNO_MISC,
			    "compression-level out of range");
			return (ARCHIVE_FAILED);
		}
		data->compression_level = (int)level;
		return (ARCHIVE_OK);
	}
#if HAVE_ZXC_H && HAVE_LIBZXC
	if (strcmp(key, "checksum") == 0) {
		if (value == NULL || strcmp(value, "1") == 0 ||
		    strcmp(value, "on") == 0 || strcmp(value, "yes") == 0)
			data->checksum_enabled = 1;
		else
			data->checksum_enabled = 0;
		return (ARCHIVE_OK);
	}
#endif

	/* Note: The "warn" return is just to inform the options
	 * supervisor that we didn't handle it.  It will generate
	 * a suitable error if no one used this option. */
	return (ARCHIVE_WARN);
}

#if HAVE_ZXC_H && HAVE_LIBZXC

/*
 * Drain everything currently sitting in cs (after a _compress or _end call)
 * into the next filter.  Loops until cs reports no pending bytes (return 0)
 * or an error.  Returns ARCHIVE_OK / ARCHIVE_FATAL.
 */
static int
zxc_drain(struct archive_write_filter *f, struct private_data *data,
    int finalising)
{
	zxc_outbuf_t out;
	int64_t r;

	for (;;) {
		out.dst = data->out_buf;
		out.size = data->out_cap;
		out.pos = 0;
		if (finalising) {
			zxc_inbuf_t empty = { NULL, 0, 0 };
			(void)empty;
			r = zxc_cstream_end(data->cs, &out);
		} else {
			zxc_inbuf_t empty = { NULL, 0, 0 };
			r = zxc_cstream_compress(data->cs, &out, &empty);
		}
		if (r < 0) {
			archive_set_error(f->archive, ARCHIVE_ERRNO_MISC,
			    "zxc streaming failed: %s",
			    zxc_error_name((int)r));
			return (ARCHIVE_FATAL);
		}
		if (out.pos > 0) {
			int rc = __archive_write_filter(f->next_filter,
			    data->out_buf, out.pos);
			if (rc != ARCHIVE_OK)
				return (rc);
		}
		if (r == 0)
			return (ARCHIVE_OK);
	}
}

static int
archive_compressor_zxc_open(struct archive_write_filter *f)
{
	struct private_data *data = (struct private_data *)f->data;
	zxc_compress_opts_t opts;

	memset(&opts, 0, sizeof(opts));
	opts.level = data->compression_level;
	opts.checksum_enabled = data->checksum_enabled;

	if (data->cs == NULL) {
		data->cs = zxc_cstream_create(&opts);
		if (data->cs == NULL) {
			archive_set_error(f->archive, ENOMEM,
			    "Can't allocate zxc compression stream");
			return (ARCHIVE_FATAL);
		}
	}
	if (data->out_buf == NULL) {
		data->out_cap = zxc_cstream_out_size(data->cs);
		if (data->out_cap == 0)
			data->out_cap = 256 * 1024;
		data->out_buf = malloc(data->out_cap);
		if (data->out_buf == NULL) {
			archive_set_error(f->archive, ENOMEM,
			    "Can't allocate zxc output buffer");
			return (ARCHIVE_FATAL);
		}
	}
	f->write = archive_compressor_zxc_write;
	return (ARCHIVE_OK);
}

static int
archive_compressor_zxc_write(struct archive_write_filter *f, const void *buff,
    size_t length)
{
	struct private_data *data = (struct private_data *)f->data;
	zxc_inbuf_t in;
	zxc_outbuf_t out;

	in.src = buff;
	in.size = length;
	in.pos = 0;

	while (in.pos < in.size) {
		out.dst = data->out_buf;
		out.size = data->out_cap;
		out.pos = 0;
		int64_t r = zxc_cstream_compress(data->cs, &out, &in);
		if (r < 0) {
			archive_set_error(f->archive, ARCHIVE_ERRNO_MISC,
			    "zxc compression failed: %s",
			    zxc_error_name((int)r));
			return (ARCHIVE_FATAL);
		}
		if (out.pos > 0) {
			int rc = __archive_write_filter(f->next_filter,
			    data->out_buf, out.pos);
			if (rc != ARCHIVE_OK)
				return (rc);
		}
	}
	return (ARCHIVE_OK);
}

static int
archive_compressor_zxc_close(struct archive_write_filter *f)
{
	struct private_data *data = (struct private_data *)f->data;

	return (zxc_drain(f, data, 1));
}

#else /* HAVE_ZXC_H && HAVE_LIBZXC */

static int
archive_compressor_zxc_open(struct archive_write_filter *f)
{
	struct private_data *data = (struct private_data *)f->data;
	struct archive_string as;
	int r;

	archive_string_init(&as);
	archive_string_sprintf(&as, "zxc -%d", data->compression_level);

	f->write = archive_compressor_zxc_write;
	r = __archive_write_program_open(f, data->pdata, as.s);
	archive_string_free(&as);
	return (r);
}

static int
archive_compressor_zxc_write(struct archive_write_filter *f, const void *buff,
    size_t length)
{
	struct private_data *data = (struct private_data *)f->data;

	return __archive_write_program_write(f, data->pdata, buff, length);
}

static int
archive_compressor_zxc_close(struct archive_write_filter *f)
{
	struct private_data *data = (struct private_data *)f->data;

	return __archive_write_program_close(f, data->pdata);
}

#endif /* HAVE_ZXC_H && HAVE_LIBZXC */
