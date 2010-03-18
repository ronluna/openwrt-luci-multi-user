#define _XOPEN_SOURCE 500	/* strptime() ... */
#define _BSD_SOURCE			/* scandir() ... */

#include "uhttpd.h"
#include "uhttpd-file.h"
#include "uhttpd-utils.h"

#include "uhttpd-mimetypes.h"


static const char * uh_file_mime_lookup(const char *path)
{
	struct mimetype *m = &uh_mime_types[0];
	char *p, *pd, *ps;

	ps = strrchr(path, '/');
	pd = strrchr(path, '.');

	/* use either slash or dot as separator, whatever comes last */
	p = (ps && pd && (ps > pd)) ? ps : pd;

	if( (p != NULL) && (*(++p) != 0) )
	{
		while( m->extn )
		{
			if( ! strcasecmp(p, m->extn) )
				return m->mime;

			m++;
		}
	}

	return "application/octet-stream";
}

static const char * uh_file_mktag(struct stat *s)
{
	static char tag[128];

	snprintf(tag, sizeof(tag), "\"%x-%x-%x\"",
		(unsigned int) s->st_ino,
		(unsigned int) s->st_size,
		(unsigned int) s->st_mtime
	);

	return tag;
}

static time_t uh_file_date2unix(const char *date)
{
	struct tm t;

	memset(&t, 0, sizeof(t));

	if( strptime(date, "%a, %d %b %Y %H:%M:%S %Z", &t) != NULL )
		return mktime(&t);

	return 0;
}

static char * uh_file_unix2date(time_t ts)
{
	static char str[128];
	struct tm *t = localtime(&ts);

	strftime(str, sizeof(str), "%a, %d %b %Y %H:%M:%S %Z", t);

	return str;
}

static char * uh_file_header_lookup(struct http_request *req, const char *name)
{
	int i;

	foreach_header(i, req->headers)
	{
		if( ! strcasecmp(req->headers[i], name) )
			return req->headers[i+1];
	}

	return NULL;
}

static void uh_file_response_ok_hdrs(struct client *cl, struct http_request *req, struct stat *s)
{
	if( s )
	{
		uh_http_sendf(cl, NULL, "ETag: %s\r\n", uh_file_mktag(s));
		uh_http_sendf(cl, NULL, "Last-Modified: %s\r\n", uh_file_unix2date(s->st_mtime));
	}

	uh_http_sendf(cl, NULL, "Date: %s\r\n", uh_file_unix2date(time(NULL)));
}

static void uh_file_response_200(struct client *cl, struct http_request *req, struct stat *s)
{
	uh_http_sendf(cl, NULL, "HTTP/%.1f 200 OK\r\n", req->version);
	uh_file_response_ok_hdrs(cl, req, s);
}

static void uh_file_response_304(struct client *cl, struct http_request *req, struct stat *s)
{
	uh_http_sendf(cl, NULL, "HTTP/%.1f 304 Not Modified\r\n", req->version);
	uh_file_response_ok_hdrs(cl, req, s);
}

static void uh_file_response_412(struct client *cl, struct http_request *req)
{
	uh_http_sendf(cl, NULL, "HTTP/%.1f 412 Precondition Failed\r\n",
		req->version);
}

static int uh_file_if_match(struct client *cl, struct http_request *req, struct stat *s)
{
	const char *tag = uh_file_mktag(s);
	char *hdr = uh_file_header_lookup(req, "If-Match");
	char *p;
	int i;

	if( hdr )
	{
		p = &hdr[0];

		for( i = 0; i < strlen(hdr); i++ )
		{
			if( (hdr[i] == ' ') || (hdr[i] == ',') )
			{
				hdr[i++] = 0;
				p = &hdr[i];
			}
			else if( !strcmp(p, "*") || !strcmp(p, tag) )
			{
				return 1;
			}
		}

		uh_file_response_412(cl, req);
		return 0;
	}

	return 1;
}

static int uh_file_if_modified_since(struct client *cl, struct http_request *req, struct stat *s)
{
	char *hdr = uh_file_header_lookup(req, "If-Modified-Since");

	if( hdr )
	{
		if( uh_file_date2unix(hdr) < s->st_mtime )
		{
			return 1;
		}
		else
		{
			uh_file_response_304(cl, req, s);
			return 0;
		}
	}

	return 1;
}

static int uh_file_if_none_match(struct client *cl, struct http_request *req, struct stat *s)
{
	const char *tag = uh_file_mktag(s);
	char *hdr = uh_file_header_lookup(req, "If-None-Match");
	char *p;
	int i;

	if( hdr )
	{
		p = &hdr[0];

		for( i = 0; i < strlen(hdr); i++ )
		{
			if( (hdr[i] == ' ') || (hdr[i] == ',') )
			{
				hdr[i++] = 0;
				p = &hdr[i];
			}
			else if( !strcmp(p, "*") || !strcmp(p, tag) )
			{
				if( (req->method == UH_HTTP_MSG_GET) ||
				    (req->method == UH_HTTP_MSG_HEAD) )
					uh_file_response_304(cl, req, s);
				else
					uh_file_response_412(cl, req);

				return 0;
			}
		}
	}

	return 1;
}

static int uh_file_if_range(struct client *cl, struct http_request *req, struct stat *s)
{
	char *hdr = uh_file_header_lookup(req, "If-Range");

	if( hdr )
	{
		uh_file_response_412(cl, req);
		return 0;
	}

	return 1;
}

static int uh_file_if_unmodified_since(struct client *cl, struct http_request *req, struct stat *s)
{
	char *hdr = uh_file_header_lookup(req, "If-Unmodified-Since");

	if( hdr )
	{
		if( uh_file_date2unix(hdr) <= s->st_mtime )
		{
			uh_file_response_412(cl, req);
			return 0;
		}
	}

	return 1;
}


static int uh_file_scandir_filter_dir(const struct dirent *e)
{
	return strcmp(e->d_name, ".") ? 1 : 0;
}

static void uh_file_dirlist(struct client *cl, struct http_request *req, struct uh_path_info *pi)
{
	int i, count;
	char filename[PATH_MAX];
	char *pathptr;
	struct dirent **files = NULL;
	struct stat s;

	uh_http_sendf(cl, req,
		"<html><head><title>Index of %s</title></head>"
		"<body><h1>Index of %s</h1><hr /><ol>",
			pi->name, pi->name
	);

	if( (count = scandir(pi->phys, &files, uh_file_scandir_filter_dir, alphasort)) > 0 )
	{
		memset(filename, 0, sizeof(filename));
		memcpy(filename, pi->phys, sizeof(filename));
		pathptr = &filename[strlen(filename)];

		/* list subdirs */
		for( i = 0; i < count; i++ )
		{
			strncat(filename, files[i]->d_name,
				sizeof(filename) - strlen(files[i]->d_name));

			if( !stat(filename, &s) && (s.st_mode & S_IFDIR) )
				uh_http_sendf(cl, req,
					"<li><strong><a href='%s%s'>%s</a>/</strong><br />"
					"<small>modified: %s<br />directory - %.02f kbyte"
					"<br /><br /></small></li>",
						pi->name, files[i]->d_name, files[i]->d_name,
						uh_file_unix2date(s.st_mtime), s.st_size / 1024.0
				);

			*pathptr = 0;
		}

		/* list files */
		for( i = 0; i < count; i++ )
		{
			strncat(filename, files[i]->d_name,
				sizeof(filename) - strlen(files[i]->d_name));

			if( !stat(filename, &s) && !(s.st_mode & S_IFDIR) )
				uh_http_sendf(cl, req,
					"<li><strong><a href='%s%s'>%s</a></strong><br />"
					"<small>modified: %s<br />%s - %.02f kbyte<br />"
					"<br /></small></li>",
						pi->name, files[i]->d_name, files[i]->d_name,
						uh_file_unix2date(s.st_mtime),
						uh_file_mime_lookup(filename), s.st_size / 1024.0
				);

			*pathptr = 0;
			free(files[i]);
		}
	}

	free(files);

	uh_http_sendf(cl, req, "</ol><hr /></body></html>");
	uh_http_sendf(cl, req, "");
}


void uh_file_request(struct client *cl, struct http_request *req)
{
	int fd, rlen;
	char buf[UH_LIMIT_MSGHEAD];
	struct uh_path_info *pi;

	/* obtain path information */
	if( (pi = uh_path_lookup(cl, req->url)) != NULL )
	{
		/* we have a file */
		if( (pi->stat.st_mode & S_IFREG) &&
		    ((fd = open(pi->phys, O_RDONLY)) > 0)
		) {
			/* test preconditions */
			if(
				uh_file_if_modified_since(cl, req, &pi->stat)  	&&
				uh_file_if_match(cl, req, &pi->stat)           	&&
				uh_file_if_range(cl, req, &pi->stat)           	&&
				uh_file_if_unmodified_since(cl, req, &pi->stat)	&&
				uh_file_if_none_match(cl, req, &pi->stat)
			) {
				/* write status */
				uh_file_response_200(cl, req, &pi->stat);

				uh_http_sendf(cl, NULL, "Content-Type: %s\r\n", uh_file_mime_lookup(pi->name));
				uh_http_sendf(cl, NULL, "Content-Length: %i\r\n", pi->stat.st_size);

				/* if request was HTTP 1.1 we'll respond chunked */
				if( req->version > 1.0 )
					uh_http_send(cl, NULL, "Transfer-Encoding: chunked\r\n", -1);

				/* close header */
				uh_http_send(cl, NULL, "\r\n", -1);

				/* pump file data */
				while( (rlen = read(fd, buf, sizeof(buf))) > 0 )
				{
					uh_http_send(cl, req, buf, rlen);
				}

				/* send trailer in chunked mode */
				uh_http_send(cl, req, "", 0);
			}

			/* one of the preconditions failed, terminate opened header and exit */
			else
			{
				uh_http_send(cl, NULL, "\r\n", -1);
			}

			close(fd);
		}

		/* directory */
		else if( pi->stat.st_mode & S_IFDIR )
		{
			/* write status */
			uh_file_response_200(cl, req, NULL);

			if( req->version > 1.0 )
				uh_http_send(cl, NULL, "Transfer-Encoding: chunked\r\n", -1);

			uh_http_send(cl, NULL, "Content-Type: text/html\r\n\r\n", -1);

			/* content */
			uh_file_dirlist(cl, req, pi);
		}

		/* 403 */
		else
		{
			uh_http_sendhf(cl, 403, "Forbidden",
				"Access to this resource is forbidden");
		}
	}

	/* 404 */
	else
	{
		uh_http_sendhf(cl, 404, "Not Found",
			"No such file or directory");
	}
}

