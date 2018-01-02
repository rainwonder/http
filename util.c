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

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/socket.h>

#include <err.h>
#include <errno.h>
#include <imsg.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "http.h"

static int	connect_wait(int);
static void	tooslow(int);

/*
 * Wait for an asynchronous connect(2) attempt to finish.
 */
int
connect_wait(int s)
{
	struct pollfd pfd[1];
	int error = 0;
	socklen_t len = sizeof(error);

	pfd[0].fd = s;
	pfd[0].events = POLLOUT;

	if (poll(pfd, 1, -1) == -1)
		return -1;
	if (getsockopt(s, SOL_SOCKET, SO_ERROR, &error, &len) < 0)
		return -1;
	if (error != 0) {
		errno = error;
		return -1;
	}
	return 0;
}

static void
tooslow(int signo)
{
	dprintf(STDERR_FILENO, "%s: connect taking too long\n", getprogname());
	_exit(2);
}

int
tcp_connect(const char *host, const char *port, int timeout, struct url *proxy)
{
	struct addrinfo	 hints, *res, *res0;
	char		 hbuf[NI_MAXHOST];
	const char	*cause = NULL;
	int		 error, s = -1, save_errno;

	if (proxy) {
		host = proxy->host;
		port = proxy->port;
	}

	if (host == NULL)
		errx(1, "hostname missing");

	memset(&hints, 0, sizeof hints);
	hints.ai_family = family;
	hints.ai_socktype = SOCK_STREAM;
	if ((error = getaddrinfo(host, port, &hints, &res0)))
		errx(1, "%s: %s: %s", __func__, gai_strerror(error), host);

	if (timeout) {
		(void)signal(SIGALRM, tooslow);
		alarm(timeout);
	}

	for (res = res0; res; res = res->ai_next) {
		if (getnameinfo(res->ai_addr, res->ai_addrlen, hbuf,
		    sizeof hbuf, NULL, 0, NI_NUMERICHOST) != 0)
			(void)strlcpy(hbuf, "(unknown)", sizeof hbuf);

		log_info("Trying %s...\n", hbuf);
		s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
		if (s == -1) {
			cause = "socket";
			continue;
		}

		for (error = connect(s, res->ai_addr, res->ai_addrlen);
		    error != 0 && errno == EINTR; error = connect_wait(s))
			continue;

		if (error != 0) {
			cause = "connect";
			save_errno = errno;
			close(s);
			errno = save_errno;
			s = -1;
			continue;
		}

		break;
	}

	freeaddrinfo(res0);
	if (s == -1)
		err(1, "%s: %s", __func__, cause);

	if (timeout) {
		signal(SIGALRM, SIG_DFL);
		alarm(0);
	}

	return s;
}

char *
xstrdup(const char *str, const char *where)
{
	char	*r;

	if ((r = strdup(str)) == NULL)
		err(1, "%s: strdup", where);

	return r;
}

int
xasprintf(char **str, const char *fmt, ...)
{
	va_list	ap;
	int	ret;

	va_start(ap, fmt);
	ret = vasprintf(str, fmt, ap);
	va_end(ap);
	if (ret == -1)
		err(1, NULL);

	return ret;
}

char *
xstrndup(const char *str, size_t maxlen, const char *where)
{
	char	*r;

	if ((r = strndup(str, maxlen)) == NULL)
		err(1, "%s: strndup", where);

	return r;
}

int
fd_request(char *path, int flags, off_t *offset)
{
	struct imsg	 imsg;
	off_t		*poffset;
	int		 fd, save_errno;

	send_message(&child_ibuf, IMSG_OPEN, flags, path, strlen(path) + 1, -1);
	if (read_message(&child_ibuf, &imsg) == 0)
		return -1;

	if (imsg.hdr.type != IMSG_OPEN)
		errx(1, "%s: IMSG_OPEN expected", __func__);

	fd = imsg.fd;
	if (offset) {
		poffset = imsg.data;
		*offset = *poffset;
	}

	save_errno = imsg.hdr.peerid;
	imsg_free(&imsg);
	errno = save_errno;
	return fd;
}

void
send_message(struct imsgbuf *ibuf, int type, uint32_t peerid,
    void *msg, size_t msglen, int fd)
{
	if (imsg_compose(ibuf, type, peerid, 0, fd, msg, msglen) != 1)
		err(1, "imsg_compose");

	if (imsg_flush(ibuf) != 0)
		err(1, "imsg_flush");
}

int
read_message(struct imsgbuf *ibuf, struct imsg *imsg)
{
	int	n;

	if ((n = imsg_read(ibuf)) == -1)
		err(1, "%s: imsg_read", __func__);
	if (n == 0)
		return 0;

	if ((n = imsg_get(ibuf, imsg)) == -1)
		err(1, "%s: imsg_get", __func__);
	if (n == 0)
		return 0;

	return n;
}

void
log_info(const char *fmt, ...)
{
	va_list	ap;

	if (verbose == 0)
		return;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

void
copy_file(struct url *url, FILE *src_fp, FILE *dst_fp, off_t *offset)
{
	char	*tmp_buf;
	size_t	 r;

	if ((tmp_buf = malloc(TMPBUF_LEN)) == NULL)
		err(1, "%s: malloc", __func__);

	while ((r = fread(tmp_buf, 1, TMPBUF_LEN, src_fp)) != 0) {
		*offset += r;
		if (fwrite(tmp_buf, 1, r, dst_fp) != r)
			err(1, "%s: fwrite", __func__);
	}

	if (!feof(src_fp))
		errx(1, "%s: fread", __func__);

	free(tmp_buf);
}

int
ftp_getline(char **lineptr, size_t *n, int suppress_output, FILE *fp)
{
	ssize_t		 len;
	char		*bufp, code[4];
	const char	*errstr;
	int		 lookup[] = { P_PRE, P_OK, P_INTER, N_TRANS, N_PERM };


	if ((len = getline(lineptr, n, fp)) == -1)
		err(1, "%s: getline", __func__);

	bufp = *lineptr;
	if (!suppress_output)
		log_info("%s", bufp);

	if (len < 4)
		errx(1, "%s: line too short", __func__);

	(void)strlcpy(code, bufp, sizeof code);
	if (bufp[3] == ' ')
		goto done;

	/* multi-line reply */
	while (!(strncmp(code, bufp, 3) == 0 && bufp[3] == ' ')) {
		if ((len = getline(lineptr, n, fp)) == -1)
			err(1, "%s: getline", __func__);

		bufp = *lineptr;
		if (!suppress_output)
			log_info("%s", bufp);

		if (len < 4)
			continue;
	}

 done:
	(void)strtonum(code, 100, 553, &errstr);
	if (errstr)
		errx(1, "%s: Response code is %s: %s", __func__, errstr, code);

	return lookup[code[0] - '1'];
}

