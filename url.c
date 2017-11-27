/*
 * Copyright (c) 2017 Sunil Nimmagadda <sunil@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*-
 * Copyright (c) 1997 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Jason Thorpe and Luke Mewburn.
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
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <ctype.h>
#include <err.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "http.h"

#ifndef nitems
#define nitems(_a)	(sizeof((_a)) / sizeof((_a)[0]))
#endif

static int	unsafe_char(const char *);
static int	scheme_lookup(const char *);
static int	authority_parse(const char *, size_t, char **, char **);

static int
scheme_lookup(const char *str)
{
	const char	*s;
	size_t		 i;
	int		 scheme;

	scheme = -1;
	for (i = 0; i < nitems(scheme_str); i++) {
		s = scheme_str[i];
		if (strncasecmp(str, s, strlen(s)) == 0) {
			scheme = i;
			break;
		}
	}

	return scheme;
}

static int
authority_parse(const char *buf, size_t len, char **host, char **port)
{
	char	*str;
	char	*p;

	str = xstrndup(buf, len, __func__);
	/* IPv6 address is encapsulated in [] */
	if (str[0] == '[') {
		if ((p = strchr(str, ']')) == NULL) {
			warnx("%s: invalid IPv6 address: %s", __func__, str);
			return 1;
		}

		*p++ = '\0';
		if (strlen(str + 1) > 0)
			*host = xstrdup(str + 1, __func__);

		if (*p == '\0')
			goto done;

		if (*p != ':') {
			warnx("%s: invalid port: %s", __func__, p);
			return 1;
		}

		if (strlen(p + 1) > 0)
			*port = xstrdup(p + 1, __func__);

		goto done;
	}

	/* host:port */
	if ((p = strchr(str, ':')) != NULL) {
		*p++ = '\0';
		if (strlen(p) > 0)
			*port = xstrdup(p, __func__);
	}

	if (strlen(str) > 0)
		*host = xstrdup(str, __func__);

 done:
	free(str);
	return 0;
}

struct url *
url_parse(const char *str)
{
	struct url	*url;
	const char	*p, *q;
	char		*host, *port, *path;
	size_t		 len;
	int		 scheme;

	p = str;
	host = port = path = NULL;
	while (isblank((unsigned char)*p))
		p++;

	if ((q = strchr(p, ':')) == NULL) {
		warnx("%s: scheme missing: %s", __func__, str);
		return NULL;
	}

	if ((scheme = scheme_lookup(p)) == -1) {
		warnx("%s: invalid scheme: %s", __func__, p);
		return NULL;
	}

	p = ++q;
	if (strncmp(p, "//", 2) != 0)
		goto done;

	p += 2;
 	if ((q = strchr(p, '@')) != NULL) {
		warnx("%s: ignoring deprecated userinfo", __func__);
		p = ++q;
 	}

	len = strlen(p);
	/* Authority terminated by a '/' if present */
	if ((q = strchr(p, '/')) != NULL)
		len = q - p;

	if (authority_parse(p, len, &host, &port) != 0)
		return NULL;

	if (port == NULL && scheme != S_FILE)
		port = xstrdup(port_str[scheme], __func__);

 done:
	if (q != NULL)
		path = xstrdup(q, __func__);

	if (http_debug) {
		fprintf(stderr,
		    "scheme: %s\nhost: %s\nport: %s\npath: %s\n",
		    scheme_str[scheme], host, port, path);
	}

	if ((url = calloc(1, sizeof *url)) == NULL)
		err(1, "%s: malloc", __func__);

	url->scheme = scheme;
	url->host = host;
	url->port = port;
	url->path = path;
	return url;
}

void
url_free(struct url *url)
{
	if (url == NULL)
		return;

	free(url->host);
	free(url->port);
	free(url->path);
	free(url->fname);
	free(url);
}

void
url_connect(struct url *url, struct url *proxy, int timeout)
{
	switch (url->scheme) {
	case S_HTTP:
	case S_HTTPS:
		http_connect(url, proxy, timeout);
		break;
	case S_FTP:
		ftp_connect(url, proxy, timeout);
		break;
	case S_FILE:
		file_connect(&child_ibuf, url);
		break;
	}
}

struct url *
url_request(struct url *url, struct url *proxy)
{
	switch (url->scheme) {
	case S_HTTP:
	case S_HTTPS:
		return http_get(url, proxy);
	case S_FTP:
		return ftp_get(url, proxy);
	case S_FILE:
		return file_request(&child_ibuf, url);
	}

	return NULL;
}

void
url_save(struct url *url, struct url *proxy, const char *title, int pm, int fd)
{
	FILE		*dst_fp;
	const char	*fname;

	fname = strcmp(url->fname, "-") == 0 ?
	    basename(url->path) : basename(url->fname);

	if (pm)
		start_progress_meter(fname, title, url->file_sz, &url->offset);

	if ((dst_fp = fdopen(fd, "w")) == NULL)
		err(1, "%s: fdopen", __func__);

	switch (url->scheme) {
	case S_HTTP:
	case S_HTTPS:
		http_save(url, dst_fp);
		break;
	case S_FTP:
		ftp_save(url, proxy, dst_fp);
		break;
	case S_FILE:
		file_save(url, dst_fp);
		break;
	}

 	fclose(dst_fp);
	if (pm)
		stop_progress_meter();

	if (url->scheme == S_FTP)
		ftp_quit(url);
}

char *
url_str(struct url *url)
{
	char	*host, *str;
	int	 custom_port;

	custom_port = strcmp(url->port, port_str[url->scheme]) ? 1 : 0;
	if (strchr(url->host, ':') != NULL)
		xasprintf(&host, "[%s]", url->host);	/* IPv6 literal */
	else
		host = xstrdup(url->host, __func__);

	xasprintf(&str, "%s//%s%s%s%s",
	    scheme_str[url->scheme],
	    host,
	    custom_port ? ":" : "",
	    custom_port ? url->port : "",
	    url->path ? url->path : "/");

	free(host);
	return str;
}

/*
 * Encode given URL, per RFC1738.
 * Allocate and return string to the caller.
 */
char *
url_encode(const char *path)
{
	size_t i, length, new_length;
	char *epath, *epathp;

	length = new_length = strlen(path);

	/*
	 * First pass:
	 * Count unsafe characters, and determine length of the
	 * final URL.
	 */
	for (i = 0; i < length; i++)
		if (unsafe_char(path + i))
			new_length += 2;

	epath = epathp = malloc(new_length + 1);	/* One more for '\0'. */
	if (epath == NULL)
		err(1, "Can't allocate memory for URL encoding");

	/*
	 * Second pass:
	 * Encode, and copy final URL.
	 */
	for (i = 0; i < length; i++)
		if (unsafe_char(path + i)) {
			snprintf(epathp, 4, "%%" "%02x",
			    (unsigned char)path[i]);
			epathp += 3;
		} else
			*(epathp++) = path[i];

	*epathp = '\0';
	return epath;
}

/*
 * Determine whether the character needs encoding, per RFC1738:
 * 	- No corresponding graphic US-ASCII.
 * 	- Unsafe characters.
 */
static int
unsafe_char(const char *c0)
{
	const char *unsafe_chars = " <>\"#{}|\\^~[]`";
	const unsigned char *c = (const unsigned char *)c0;

	/*
	 * No corresponding graphic US-ASCII.
	 * Control characters and octets not used in US-ASCII.
	 */
	return (iscntrl(*c) || !isascii(*c) ||

	    /*
	     * Unsafe characters.
	     * '%' is also unsafe, if is not followed by two
	     * hexadecimal digits.
	     */
	    strchr(unsafe_chars, *c) != NULL ||
	    (*c == '%' && (!isxdigit(*++c) || !isxdigit(*++c))));
}
