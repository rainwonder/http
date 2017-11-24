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

#include <stdio.h>

#define	S_HTTP	0
#define S_HTTPS	1
#define S_FTP	2
#define S_FILE	3

#define TMPBUF_LEN	131072

enum {
	IMSG_STAT,
	IMSG_OPEN
};

struct imsg;
struct imsgbuf;

struct url {
	int	 scheme;
	char	*host;
	char	*port;
	char	*path;

	char	*fname;
	off_t	 file_sz;
	off_t	 offset;
};

struct open_req {
	char	fname[FILENAME_MAX];
	int	flags;
};

/* file.c */
void		 file_connect(struct imsgbuf *, struct imsg *, struct url *);
struct url	*file_request(struct imsgbuf *, struct imsg *, struct url *);
void		 file_save(struct url *, FILE *);

/* ftp.c */
extern int	 activemode;

void		 ftp_connect(struct url *, int, struct url *);
struct url	*ftp_get(struct url *);
void		 ftp_quit(struct url *);
void		 ftp_save(struct url *, FILE *);

/* http.c */
void		 http_connect(struct url *, int, struct url *);
struct url	*http_get(struct url *, struct url *);
void		 http_save(struct url *, FILE *);
void		 https_init(void);

/* main.c */
extern const char	*scheme_str[4];
extern const char	*port_str[4];
extern char		*tls_options;
extern const char	*ua;
extern const char	*title;
extern int		 http_debug;
extern int		 progressmeter;
extern int		 verbose;
extern struct imsgbuf	 child_ibuf;
extern struct imsg	 child_imsg;

/* progressmeter.c */
void	start_progress_meter(const char *, off_t, off_t *);
void	stop_progress_meter(void);

/* url.c */
void		 url_connect(struct url *, struct url *, int);
char		*url_encode(const char *);
void		 url_free(struct url *);
struct url	*url_parse(char *);
struct url	*url_request(struct url *, struct url *);
void		 url_save(struct url *, struct url *, int);

/* util.c */
void	 copy_file(struct url *, FILE *, FILE *);
int	 tcp_connect(const char *, const char *, int, struct url *);
char	*xstrdup(const char *, const char *);
char	*xstrndup(const char *, size_t, const char *);
off_t	 stat_request(struct imsgbuf *, struct imsg *, const char *, int *);
int	 fd_request(struct imsgbuf *, struct imsg *, const char *, int);
int	 read_message(struct imsgbuf *, struct imsg *);
void	 send_message(struct imsgbuf *, int, uint32_t, void *, size_t, int);
void	 log_info(const char *, ...)
	    __attribute__((__format__ (printf, 1, 2)))
	    __attribute__((__nonnull__ (1)));
void	 log_request(const char *, struct url *, struct url *);
