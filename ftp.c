/*
 * Copyright (c) 2015 Sunil Nimmagadda <sunil@openbsd.org>
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

#include <sys/socket.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <err.h>
#include <libgen.h>
#include <limits.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "http.h"

static int	ftp_auth(const char *, const char *);
static int	ftp_eprt(void);
static int	ftp_epsv(void);
static int	ftp_size(const char *, off_t *, char **);

static FILE	*ctrl_fp;
static int	 data_fd;

void
ftp_connect(struct url *url, struct url *proxy, int timeout)
{
	char	*buf = NULL;
	size_t	 n = 0;
	int	 sock;

	if (proxy) {
		http_connect(url, proxy, timeout);
		return;
	}

	sock = tcp_connect(url->host, url->port, timeout, NULL);
	if ((ctrl_fp = fdopen(sock, "r+")) == NULL)
		err(1, "%s: fdopen", __func__);

	/* greeting */
	if (ftp_getline(&buf, &n, 0, ctrl_fp) != P_OK) {
		warnx("Can't connect to host `%s'", url->host);
		ftp_command(ctrl_fp, "QUIT");
		exit(1);
	}

	free(buf);
	log_info("Connected to %s\n", url->host);
	if (ftp_auth(NULL, NULL) != P_OK) {
		warnx("Can't login to host `%s'", url->host);
		ftp_command(ctrl_fp, "QUIT");
		exit(1);
	}
}

struct url *
ftp_get(struct url *url, struct url *proxy, off_t *offset, off_t *sz)
{
	char	*buf = NULL, *dir, *file;

	if (proxy) {
		url = http_get(url, proxy, offset, sz);
		/* this url should now be treated as HTTP */
		url->scheme = S_HTTP;
		return url;
	}

	log_info("Using binary mode to transfer files.\n");
	if (ftp_command(ctrl_fp, "TYPE I") != P_OK)
		errx(1, "Failed to set mode to binary");

	dir = dirname(url->path);
	if (ftp_command(ctrl_fp, "CWD %s", dir) != P_OK)
		errx(1, "CWD command failed");

	log_info("Retrieving %s\n", url->path);
	file = basename(url->path);
	if (strcmp(url->fname, "-"))
		log_info("local: %s remote: %s\n", url->fname, file);
	else
		log_info("remote: %s\n", file);

	if (ftp_size(file, sz, &buf) != P_OK) {
		fprintf(stderr, "%s", buf);
		ftp_command(ctrl_fp, "QUIT");
		exit(1);
	}
	free(buf);

	if (activemode) {
		if ((data_fd = ftp_eprt()) == -1)
			errx(1, "Failed to establish data connection");
	} else if ((data_fd = ftp_epsv()) == -1)
		if ((data_fd = ftp_eprt()) == -1)
			errx(1, "Failed to establish data connection");

	if (*offset && ftp_command(ctrl_fp, "REST %lld", *offset) != P_INTER)
		errx(1, "REST command failed");

	if (ftp_command(ctrl_fp, "RETR %s", file) != P_PRE) {
		ftp_command(ctrl_fp, "QUIT");
		exit(1);
	}

	return url;
}

void
ftp_save(struct url *url, FILE *dst_fp, off_t *offset)
{
	struct sockaddr_storage	 ss;
	FILE			*data_fp;
	socklen_t		 len;
	int			 s;

	if (activemode) {
		len = sizeof(ss);
		if ((s = accept(data_fd, (struct sockaddr *)&ss, &len)) == -1)
			err(1, "%s: accept", __func__);

		close(data_fd);
		data_fd = s;
	}

	if ((data_fp = fdopen(data_fd, "r")) == NULL)
		err(1, "%s: fdopen data_fd", __func__);

	copy_file(url, data_fp, dst_fp, offset);
	fclose(data_fp);
}

void
ftp_quit(struct url *url)
{
	char	*buf = NULL;
	size_t	 n = 0;

	if (ftp_getline(&buf, &n, 0, ctrl_fp) != P_OK)
		errx(1, "error retrieving file %s", url->fname);

	free(buf);
	ftp_command(ctrl_fp, "QUIT");
	fclose(ctrl_fp);
}

static int
ftp_epsv(void)
{
	struct sockaddr_storage	 ss;
	struct sockaddr_in	*s_in;
	struct sockaddr_in6	*s_in6;
	char			*buf = NULL, delim[4], *s, *e;
	size_t			 n = 0;
	socklen_t		 len;
	int			 port, sock;

	if (http_debug)
		fprintf(stderr, ">>> EPSV\n");

	if (fprintf(ctrl_fp, "EPSV\r\n") < 0)
		errx(1, "%s: fprintf", __func__);

	(void)fflush(ctrl_fp);
	if (ftp_getline(&buf, &n, 1, ctrl_fp) != P_OK) {
		free(buf);
		return -1;
	}

	if ((s = strchr(buf, '(')) == NULL || (e = strchr(s, ')')) == NULL) {
		warnx("Malformed EPSV reply");
		free(buf);
		return -1;
	}

	s++;
	*e = '\0';
	if (sscanf(s, "%c%c%c%d%c", &delim[0], &delim[1], &delim[2],
	    &port, &delim[3]) != 5) {
		warnx("EPSV parse error");
		free(buf);
		return -1;
	}
	free(buf);

	if (delim[0] != delim[1] || delim[0] != delim[2]
	    || delim[0] != delim[3]) {
		warnx("EPSV parse error");
		return -1;
	}

	len = sizeof(ss);
	memset(&ss, 0, len);
	if (getpeername(fileno(ctrl_fp), (struct sockaddr *)&ss, &len) == -1)
		err(1, "%s: getpeername", __func__);

	switch (ss.ss_family) {
	case AF_INET:
		s_in = (struct sockaddr_in *)&ss;
		s_in->sin_port = htons(port);
		break;
	case AF_INET6:
		s_in6 = (struct sockaddr_in6 *)&ss;
		s_in6->sin6_port = htons(port);
		break;
	default:
		errx(1, "%s: Invalid socket family", __func__);
	}

	if ((sock = socket(ss.ss_family, SOCK_STREAM, 0)) == -1)
		err(1, "%s: socket", __func__);

	if (connect(sock, (struct sockaddr *)&ss, ss.ss_len) == -1)
		err(1, "%s: connect", __func__);

	return sock;
}

static int
ftp_eprt(void)
{
	struct sockaddr_storage	 ss;
	struct sockaddr_in	*s_in;
	struct sockaddr_in6	*s_in6;
	char			 addr[NI_MAXHOST], port[NI_MAXSERV], *eprt;
	socklen_t		 len;
	int			 e, on, ret, sock;

	len = sizeof(ss);
	memset(&ss, 0, len);
	if (getsockname(fileno(ctrl_fp), (struct sockaddr *)&ss, &len) == -1)
		err(1, "%s: getsockname", __func__);

	if (ss.ss_family != AF_INET && ss.ss_family != AF_INET6)
		errx(1, "Control connection not on IPv4 or IPv6");

	/* pick a free port */
	switch (ss.ss_family) {
	case AF_INET:
		s_in = (struct sockaddr_in *)&ss;
		s_in->sin_port = 0;
		break;
	case AF_INET6:
		s_in6 = (struct sockaddr_in6 *)&ss;
		s_in6->sin6_port = 0;
		break;
	}

	if ((sock = socket(ss.ss_family, SOCK_STREAM, 0)) == -1)
		err(1, "%s: socket", __func__);

	switch (ss.ss_family) {
	case AF_INET:
		on = IP_PORTRANGE_HIGH;
		if (setsockopt(sock, IPPROTO_IP, IP_PORTRANGE,
		    (char *)&on, sizeof(on)) < 0)
			warn("setsockopt IP_PORTRANGE (ignored)");
		break;
	case AF_INET6:
		on = IPV6_PORTRANGE_HIGH;
		if (setsockopt(sock, IPPROTO_IPV6, IPV6_PORTRANGE,
		    (char *)&on, sizeof(on)) < 0)
			warn("setsockopt IPV6_PORTRANGE (ignored)");
		break;
	}

	if (bind(sock, (struct sockaddr *)&ss, ss.ss_len) == -1)
		err(1, "%s: bind", __func__);

	if (listen(sock, 1) == -1)
		err(1, "%s: listen", __func__);

	/* Find out the ephermal port chosen */
	len = sizeof(ss);
	memset(&ss, 0, len);
	if (getsockname(sock, (struct sockaddr *)&ss, &len) == -1)
		err(1, "%s: getsockname", __func__);

	if ((e = getnameinfo((struct sockaddr *)&ss, ss.ss_len,
	    addr, sizeof(addr), port, sizeof(port),
	    NI_NUMERICHOST | NI_NUMERICSERV)) != 0)
		err(1, "%s: getnameinfo: %s", __func__, gai_strerror(e));

	xasprintf(&eprt, "EPRT |%d|%s|%s|",
	    ss.ss_family == AF_INET ? 1 : 2, addr, port);

	ret = ftp_command(ctrl_fp, "%s", eprt);
	free(eprt);
	if (ret != P_OK) {
		close(sock);
		activemode = 0;
		return -1;
	}

	activemode = 1;
	return sock;
}

static int
ftp_size(const char *fn, off_t *sizep, char **buf)
{
	size_t	 n = 0;
	off_t	 file_sz;
	int	 code;

	if (http_debug)
		fprintf(stderr, ">>> SIZE %s\n", fn);

	if (fprintf(ctrl_fp, "SIZE %s\r\n", fn) < 0)
		errx(1, "%s: fprintf", __func__);

	(void)fflush(ctrl_fp);
	if ((code = ftp_getline(buf, &n, 1, ctrl_fp)) != P_OK)
		return code;

	if (sscanf(*buf, "%*u %lld", &file_sz) != 1)
		errx(1, "%s: sscanf size", __func__);

	if (sizep)
		*sizep = file_sz;

	return code;
}

static int
ftp_auth(const char *user, const char *pass)
{
	char	*addr = NULL, hn[HOST_NAME_MAX+1], *un;
	int	 code;

	code = ftp_command(ctrl_fp, "USER %s", user ? user : "anonymous");
	if (code != P_OK && code != P_INTER)
		return code;

	if (pass == NULL) {
		if (gethostname(hn, sizeof hn) == -1)
			err(1, "%s: gethostname", __func__);

		un = getlogin();
		xasprintf(&addr, "%s@%s", un ? un : "anonymous", hn);
	}

	code = ftp_command(ctrl_fp, "PASS %s", pass ? pass : addr);
	free(addr);
	return code;
}
