/*
 *
 *  Web service library with GLib integration
 *
 *  Copyright (C) 2009-2010  Intel Corporation. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "giognutls.h"
#include "gresolv.h"
#include "gweb.h"

#define DEFAULT_BUFFER_SIZE  2048

#define SESSION_FLAG_USE_TLS	(1 << 0)

enum chunk_state {
	CHUNK_SIZE,
	CHUNK_R_BODY,
	CHUNK_N_BODY,
	CHUNK_DATA,
};

struct _GWebResult {
	guint16 status;
	const guint8 *buffer;
	gsize length;
	gboolean use_chunk;
};

struct web_session {
	GWeb *web;

	char *address;
	char *host;
	uint16_t port;
	unsigned long flags;

	char *content_type;

	GIOChannel *transport_channel;
	guint transport_watch;
	guint send_watch;

	guint resolv_action;
	char *request;

	guint8 *receive_buffer;
	gsize receive_space;
	GString *send_buffer;
	GString *current_header;
	gboolean header_done;
	gboolean body_done;
	gboolean more_data;
	gboolean request_started;

	enum chunk_state chunck_state;
	gsize chunk_size;
	gsize chunk_left;
	gsize total_len;

	GWebResult result;

	GWebResultFunc result_func;
	GWebInputFunc input_func;
	gpointer user_data;
};

struct _GWeb {
	gint ref_count;

	guint next_query_id;

	int index;
	GList *session_list;

	GResolv *resolv;
	char *proxy;
	char *accept_option;
	char *user_agent;
	gboolean close_connection;

	GWebDebugFunc debug_func;
	gpointer debug_data;
};

static inline void debug(GWeb *web, const char *format, ...)
{
	char str[256];
	va_list ap;

	if (web->debug_func == NULL)
		return;

	va_start(ap, format);

	if (vsnprintf(str, sizeof(str), format, ap) > 0)
		web->debug_func(str, web->debug_data);

	va_end(ap);
}

static void free_session(struct web_session *session)
{
	GWeb *web = session->web;

	if (session == NULL)
		return;

	g_free(session->request);

	if (session->resolv_action > 0)
		g_resolv_cancel_lookup(web->resolv, session->resolv_action);

	if (session->transport_watch > 0)
		g_source_remove(session->transport_watch);

	if (session->send_watch > 0)
		g_source_remove(session->send_watch);

	if (session->transport_channel != NULL)
		g_io_channel_unref(session->transport_channel);

	g_string_free(session->send_buffer, TRUE);
	g_string_free(session->current_header, TRUE);
	g_free(session->receive_buffer);

	g_free(session->content_type);

	g_free(session->host);
	g_free(session->address);
	g_free(session);
}

static void flush_sessions(GWeb *web)
{
	GList *list;

	for (list = g_list_first(web->session_list);
					list; list = g_list_next(list))
		free_session(list->data);

	g_list_free(web->session_list);
	web->session_list = NULL;
}

GWeb *g_web_new(int index)
{
	GWeb *web;

	if (index < 0)
		return NULL;

	web = g_try_new0(GWeb, 1);
	if (web == NULL)
		return NULL;

	web->ref_count = 1;

	web->next_query_id = 1;

	web->index = index;
	web->session_list = NULL;

	web->resolv = g_resolv_new(index);
	if (web->resolv == NULL) {
		g_free(web);
		return NULL;
	}

	web->accept_option = g_strdup("*/*");
	web->user_agent = g_strdup_printf("GWeb/%s", VERSION);
	web->close_connection = FALSE;

	return web;
}

GWeb *g_web_ref(GWeb *web)
{
	if (web == NULL)
		return NULL;

	g_atomic_int_inc(&web->ref_count);

	return web;
}

void g_web_unref(GWeb *web)
{
	if (web == NULL)
		return;

	if (g_atomic_int_dec_and_test(&web->ref_count) == FALSE)
		return;

	flush_sessions(web);

	g_resolv_unref(web->resolv);

	g_free(web->proxy);

	g_free(web->accept_option);
	g_free(web->user_agent);

	g_free(web);
}

void g_web_set_debug(GWeb *web, GWebDebugFunc func, gpointer user_data)
{
	if (web == NULL)
		return;

	web->debug_func = func;
	web->debug_data = user_data;

	g_resolv_set_debug(web->resolv, func, user_data);
}

gboolean g_web_set_proxy(GWeb *web, const char *proxy)
{
	if (web == NULL)
		return FALSE;

	g_free(web->proxy);
	web->proxy = g_strdup(proxy);

	return TRUE;
}

gboolean g_web_add_nameserver(GWeb *web, const char *address)
{
	if (web == NULL)
		return FALSE;

	g_resolv_add_nameserver(web->resolv, address, 53, 0);

	return TRUE;
}

static gboolean set_accept_option(GWeb *web, const char *format, va_list args)
{
	g_free(web->accept_option);

	if (format == NULL) {
		web->accept_option = NULL;
		debug(web, "clearing accept option");
	} else {
		web->accept_option = g_strdup_vprintf(format, args);
		debug(web, "setting accept %s", web->accept_option);
	}

	return TRUE;
}

gboolean g_web_set_accept(GWeb *web, const char *format, ...)
{
	va_list args;
	gboolean result;

	if (web == NULL)
		return FALSE;

	va_start(args, format);
	result = set_accept_option(web, format, args);
	va_end(args);

	return result;
}

static gboolean set_user_agent(GWeb *web, const char *format, va_list args)
{
	g_free(web->user_agent);

	if (format == NULL) {
		web->user_agent = NULL;
		debug(web, "clearing user agent");
	} else {
		web->user_agent = g_strdup_vprintf(format, args);
		debug(web, "setting user agent %s", web->user_agent);
	}

	return TRUE;
}

gboolean g_web_set_user_agent(GWeb *web, const char *format, ...)
{
	va_list args;
	gboolean result;

	if (web == NULL)
		return FALSE;

	va_start(args, format);
	result = set_user_agent(web, format, args);
	va_end(args);

	return result;
}

void g_web_set_close_connection(GWeb *web, gboolean enabled)
{
	if (web == NULL)
		return;

	web->close_connection = enabled;
}

gboolean g_web_get_close_connection(GWeb *web)
{
	if (web == NULL)
		return FALSE;

	return web->close_connection;
}

static inline void call_result_func(struct web_session *session, guint16 status)
{
	if (session->result_func == NULL)
		return;

	if (status != 0)
		session->result.status = status;

	session->result_func(&session->result, session->user_data);
}

static gboolean process_send_buffer(struct web_session *session)
{
	GString *buf = session->send_buffer;
	gsize count, bytes_written;
	GIOStatus status;

	count = buf->len;

	if (count == 0) {
		if (session->more_data == FALSE)
			session->body_done = TRUE;

		return FALSE;
	}

	debug(session->web, "bytes to write %zu", count);

	status = g_io_channel_write_chars(session->transport_channel,
					buf->str, count, &bytes_written, NULL);

	debug(session->web, "status %u bytes written %zu",
					status, bytes_written);

	if (status != G_IO_STATUS_NORMAL && status != G_IO_STATUS_AGAIN)
		return FALSE;

	g_string_erase(buf, 0, bytes_written);

	return TRUE;
}

static void process_next_chunk(struct web_session *session)
{
	GString *buf = session->send_buffer;
	const guint8 *body;
	gsize length;

	if (session->input_func == NULL) {
		session->more_data = FALSE;
		return;
	}

	session->more_data = session->input_func(&body, &length,
						session->user_data);

	if (length > 0) {
		g_string_append_printf(buf, "%zx\r\n", length);
		g_string_append_len(buf, (char *) body, length);
		g_string_append(buf, "\r\n");
	}

	if (session->more_data == FALSE)
		g_string_append(buf, "0\r\n\r\n");
}

static void start_request(struct web_session *session)
{
	GString *buf = session->send_buffer;
	const guint8 *body;
	gsize length;

	debug(session->web, "request %s from %s",
					session->request, session->host);

	g_string_truncate(buf, 0);

	if (session->content_type == NULL)
		g_string_append_printf(buf, "GET %s HTTP/1.1\r\n",
							session->request);
	else
		g_string_append_printf(buf, "POST %s HTTP/1.1\r\n",
							session->request);
	g_string_append_printf(buf, "Host: %s\r\n", session->host);
	if (session->web->user_agent != NULL)
		g_string_append_printf(buf, "User-Agent: %s\r\n",
						session->web->user_agent);
	if (session->web->accept_option != NULL)
		g_string_append_printf(buf, "Accept: %s\r\n",
						session->web->accept_option);
	if (session->content_type != NULL) {
		g_string_append_printf(buf, "Content-Type: %s\r\n",
							session->content_type);
		if (session->input_func == NULL) {
			session->more_data = FALSE;
			length = 0;
		} else
			session->more_data = session->input_func(&body, &length,
							session->user_data);
		if (session->more_data == FALSE)
			g_string_append_printf(buf, "Content-Length: %zu\r\n",
									length);
		else
			g_string_append(buf, "Transfer-Encoding: chunked\r\n");
	}
	if (session->web->close_connection == TRUE)
		g_string_append(buf, "Connection: close\r\n");
	g_string_append(buf, "\r\n");

	if (session->content_type != NULL && length > 0) {
		if (session->more_data == TRUE) {
			g_string_append_printf(buf, "%zx\r\n", length);
			g_string_append_len(buf, (char *) body, length);
			g_string_append(buf, "\r\n");
		} else
			g_string_append_len(buf, (char *) body, length);
	}
}

static gboolean send_data(GIOChannel *channel, GIOCondition cond,
						gpointer user_data)
{
	struct web_session *session = user_data;

	if (cond & (G_IO_NVAL | G_IO_ERR | G_IO_HUP)) {
		session->send_watch = 0;
		return FALSE;
	}

	if (process_send_buffer(session) == TRUE)
		return TRUE;

	if (session->request_started == FALSE) {
		session->request_started = TRUE;
		start_request(session);
	} else if (session->more_data == TRUE)
		process_next_chunk(session);

	process_send_buffer(session);

	if (session->body_done == TRUE) {
		session->send_watch = 0;
		return FALSE;
	}

	return TRUE;
}

static int decode_chunked(struct web_session *session,
					const guint8 *buf, gsize len)
{
	const guint8 *ptr = buf;
	gsize counter;

	while (len > 0) {
		guint8 *pos;
		gsize count;
		char *str;

		switch (session->chunck_state) {
		case CHUNK_SIZE:
			pos = memchr(ptr, '\n', len);
			if (pos == NULL) {
				g_string_append_len(session->current_header,
						(gchar *) ptr, len);
				return 0;
			}

			count = pos - ptr;
			if (count < 1 || ptr[count - 1] != '\r')
				return -EILSEQ;

			g_string_append_len(session->current_header,
						(gchar *) ptr, count);

			len -= count + 1;
			ptr = pos + 1;

			str = session->current_header->str;

			counter = strtoul(str, NULL, 16);
			if ((counter == 0 && errno == EINVAL) ||
						counter == ULONG_MAX)
				return -EILSEQ;

			session->chunk_size = counter;
			session->chunk_left = counter;

			session->chunck_state = CHUNK_DATA;
			break;
		case CHUNK_R_BODY:
			if (*ptr != '\r')
				return -EILSEQ;
			ptr++;
			len--;
			session->chunck_state = CHUNK_N_BODY;
			break;
		case CHUNK_N_BODY:
			if (*ptr != '\n')
				return -EILSEQ;
			ptr++;
			len--;
			session->chunck_state = CHUNK_SIZE;
			break;
		case CHUNK_DATA:
			if (session->chunk_size == 0) {
				debug(session->web, "Download Done in chunk");
				g_string_truncate(session->current_header, 0);
				return 0;
			}

			if (session->chunk_left <= len) {
				session->result.buffer = ptr;
				session->result.length = session->chunk_left;
				call_result_func(session, 0);

				len -= session->chunk_left;
				ptr += session->chunk_left;

				session->total_len += session->chunk_left;
				session->chunk_left = 0;

				g_string_truncate(session->current_header, 0);
				session->chunck_state = CHUNK_R_BODY;
				break;
			}
			/* more data */
			session->result.buffer = ptr;
			session->result.length = len;
			call_result_func(session, 0);

			session->chunk_left -= len;
			session->total_len += len;

			len -= len;
			ptr += len;
			break;
		}
	}

	return 0;
}

static int handle_body(struct web_session *session,
				const guint8 *buf, gsize len)
{
	int err;

	debug(session->web, "[body] length %zu", len);

	if (session->result.use_chunk == FALSE) {
		session->result.buffer = buf;
		session->result.length = len;
		call_result_func(session, 0);
		return 0;
	}

	err = decode_chunked(session, buf, len);
	if (err < 0) {
		debug(session->web, "Error in chunk decode %d", err);

		session->result.buffer = NULL;
		session->result.length = 0;
		call_result_func(session, 400);
	}

	return err;
}

static gboolean received_data(GIOChannel *channel, GIOCondition cond,
							gpointer user_data)
{
	struct web_session *session = user_data;
	guint8 *ptr = session->receive_buffer;
	gsize bytes_read;
	GIOStatus status;

	if (cond & (G_IO_NVAL | G_IO_ERR | G_IO_HUP)) {
		session->transport_watch = 0;
		session->result.buffer = NULL;
		session->result.length = 0;
		call_result_func(session, 400);
		return FALSE;
	}

	status = g_io_channel_read_chars(channel,
				(gchar *) session->receive_buffer,
				session->receive_space - 1, &bytes_read, NULL);

	debug(session->web, "bytes read %zu", bytes_read);

	if (status != G_IO_STATUS_NORMAL && status != G_IO_STATUS_AGAIN) {
		session->transport_watch = 0;
		session->result.buffer = NULL;
		session->result.length = 0;
		call_result_func(session, 0);
		return FALSE;
	}

	session->receive_buffer[bytes_read] = '\0';

	if (session->header_done == TRUE) {
		if (handle_body(session, session->receive_buffer,
							bytes_read) < 0) {
			session->transport_watch = 0;
			return FALSE;
		}
		return TRUE;
	}

	while (bytes_read > 0) {
		guint8 *pos;
		gsize count;
		char *str;

		pos = memchr(ptr, '\n', bytes_read);
		if (pos == NULL) {
			g_string_append_len(session->current_header,
						(gchar *) ptr, bytes_read);
			return TRUE;
		}

		*pos = '\0';
		count = strlen((char *) ptr);
		if (count > 0 && ptr[count - 1] == '\r') {
			ptr[--count] = '\0';
			bytes_read--;
		}

		g_string_append_len(session->current_header,
						(gchar *) ptr, count);

		bytes_read -= count + 1;
		if (bytes_read > 0)
			ptr = pos + 1;
		else
			ptr = NULL;

		if (session->current_header->len == 0) {
			session->header_done = TRUE;
			if (handle_body(session, ptr, bytes_read) < 0) {
				session->transport_watch = 0;
				return FALSE;
			}
			break;
		}

		str = session->current_header->str;

		if (session->result.status == 0) {
			unsigned int code;

			if (sscanf(str, "HTTP/%*s %u %*s", &code) == 1)
				session->result.status = code;
		} else if (session->result.use_chunk == FALSE &&
				g_ascii_strncasecmp("Transfer-Encoding:",
								str, 18) == 0) {
			char *val;

			val = g_strrstr(str + 18, "chunked");
			if (val != NULL) {
				session->result.use_chunk = TRUE;

				session->chunck_state = CHUNK_SIZE;
				session->chunk_left = 0;
				session->chunk_left = 0;
				session->total_len = 0;
			}
		}

		debug(session->web, "[header] %s", str);

		g_string_truncate(session->current_header, 0);
	}

	return TRUE;
}

static int connect_session_transport(struct web_session *session)
{
	GIOFlags flags;
	struct sockaddr_in sin;
	int sk;

	sk = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sk < 0)
		return -EIO;

	if (session->flags & SESSION_FLAG_USE_TLS)
		session->transport_channel = g_io_channel_gnutls_new(sk);
	else
		session->transport_channel = g_io_channel_unix_new(sk);

	if (session->transport_channel == NULL) {
		close(sk);
		return -ENOMEM;
	}

	flags = g_io_channel_get_flags(session->transport_channel);
	g_io_channel_set_flags(session->transport_channel,
					flags | G_IO_FLAG_NONBLOCK, NULL);

	g_io_channel_set_encoding(session->transport_channel, NULL, NULL);
	g_io_channel_set_buffered(session->transport_channel, FALSE);

	g_io_channel_set_close_on_unref(session->transport_channel, TRUE);

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(session->port);
	sin.sin_addr.s_addr = inet_addr(session->address);

	if (connect(sk, (struct sockaddr *) &sin, sizeof(sin)) < 0) {
		if (errno != EINPROGRESS) {
			close(sk);
			return -EIO;
		}
	}

	session->transport_watch = g_io_add_watch(session->transport_channel,
				G_IO_IN | G_IO_HUP | G_IO_NVAL | G_IO_ERR,
						received_data, session);

	session->send_watch = g_io_add_watch(session->transport_channel,
				G_IO_OUT | G_IO_HUP | G_IO_NVAL | G_IO_ERR,
						send_data, session);

	return 0;
}

static int create_transport(struct web_session *session)
{
	int err;

	err = connect_session_transport(session);
	if (err < 0)
		return err;

	debug(session->web, "creating session %s:%u",
					session->address, session->port);

	return 0;
}

static int parse_url(struct web_session *session, const char *url)
{
	char *scheme, *host, *port, *path;

	scheme = g_strdup(url);
	if (scheme == NULL)
		return -EINVAL;

	host = strstr(scheme, "://");
	if (host != NULL) {
		*host = '\0';
		host += 3;

		if (strcasecmp(scheme, "https") == 0) {
			session->port = 443;
			session->flags |= SESSION_FLAG_USE_TLS;
		} else if (strcasecmp(scheme, "http") == 0) {
			session->port = 80;
		} else {
			g_free(scheme);
			return -EINVAL;
		}
	} else {
		host = scheme;
		session->port = 80;
	}

	path = strchr(host, '/');
	if (path != NULL)
		*(path++) = '\0';

	session->request = g_strdup_printf("/%s", path ? path : "");

	port = strrchr(host, ':');
	if (port != NULL) {
		char *end;
		int tmp = strtol(port + 1, &end, 10);

		if (*end == '\0') {
			*port = '\0';
			session->port = tmp;
		}
	}

	session->host = g_strdup(host);

	g_free(scheme);

	return 0;
}

static void resolv_result(GResolvResultStatus status,
					char **results, gpointer user_data)
{
	struct web_session *session = user_data;

	if (results == NULL || results[0] == NULL) {
		call_result_func(session, 404);
		return;
	}

	debug(session->web, "address %s", results[0]);

	if (inet_aton(results[0], NULL) == 0) {
		call_result_func(session, 400);
		return;
	}

	session->address = g_strdup(results[0]);

	if (create_transport(session) < 0) {
		call_result_func(session, 409);
		return;
	}
}

static guint do_request(GWeb *web, const char *url,
				const char *type, GWebInputFunc input,
				GWebResultFunc func, gpointer user_data)
{
	struct web_session *session;

	if (web == NULL || url == NULL)
		return 0;

	debug(web, "request %s", url);

	session = g_try_new0(struct web_session, 1);
	if (session == NULL)
		return 0;

	if (parse_url(session, url) < 0) {
		free_session(session);
		return 0;
	}

	debug(web, "host %s:%u", session->host, session->port);
	debug(web, "flags %lu", session->flags);

	if (type != NULL) {
		session->content_type = g_strdup(type);

		debug(web, "content-type %s", session->content_type);
	}

	session->web = web;

	session->result_func = func;
	session->input_func = input;
	session->user_data = user_data;

	session->receive_buffer = g_try_malloc(DEFAULT_BUFFER_SIZE);
	if (session->receive_buffer == NULL) {
		free_session(session);
		return 0;
	}

	session->receive_space = DEFAULT_BUFFER_SIZE;
	session->send_buffer = g_string_sized_new(0);
	session->current_header = g_string_sized_new(0);
	session->header_done = FALSE;
	session->body_done = FALSE;

	if (inet_aton(session->host, NULL) == 0) {
		session->resolv_action = g_resolv_lookup_hostname(web->resolv,
					session->host, resolv_result, session);
		if (session->resolv_action == 0) {
			free_session(session);
			return 0;
		}
	} else {
		session->address = g_strdup(session->host);

		if (create_transport(session) < 0) {
			free_session(session);
			return 0;
		}
	}

	web->session_list = g_list_append(web->session_list, session);

	return web->next_query_id++;
}

guint g_web_request_get(GWeb *web, const char *url,
				GWebResultFunc func, gpointer user_data)
{
	return do_request(web, url, NULL, NULL, func, user_data);
}

guint g_web_request_post(GWeb *web, const char *url,
				const char *type, GWebInputFunc input,
				GWebResultFunc func, gpointer user_data)
{
	return do_request(web, url, type, input, func, user_data);
}

gboolean g_web_cancel_request(GWeb *web, guint id)
{
	if (web == NULL)
		return FALSE;

	return TRUE;
}

guint16 g_web_result_get_status(GWebResult *result)
{
	if (result == NULL)
		return 0;

	return result->status;
}

gboolean g_web_result_get_chunk(GWebResult *result,
				const guint8 **chunk, gsize *length)
{
	if (result == NULL)
		return FALSE;

	if (chunk == NULL)
		return FALSE;

	*chunk = result->buffer;

	if (length != NULL)
		*length = result->length;

	return TRUE;
}

struct _GWebParser {
	gint ref_count;
	char *begin_token;
	char *end_token;
	const char *token_str;
	size_t token_len;
	size_t token_pos;
	gboolean intoken;
	GString *content;
	GWebParserFunc func;
	gpointer user_data;
};

GWebParser *g_web_parser_new(const char *begin, const char *end,
				GWebParserFunc func, gpointer user_data)
{
	GWebParser *parser;

	parser = g_try_new0(GWebParser, 1);
	if (parser == NULL)
		return NULL;

	parser->ref_count = 1;

	parser->begin_token = g_strdup(begin);
	parser->end_token = g_strdup(end);

	if (parser->begin_token == NULL) {
		g_free(parser);
		return NULL;
	}

	parser->func = func;
	parser->user_data = user_data;

	parser->token_str = parser->begin_token;
	parser->token_len = strlen(parser->token_str);
	parser->token_pos = 0;

	parser->intoken = FALSE;
	parser->content = g_string_sized_new(0);

	return parser;
}

GWebParser *g_web_parser_ref(GWebParser *parser)
{
	if (parser == NULL)
		return NULL;

	g_atomic_int_inc(&parser->ref_count);

	return parser;
}

void g_web_parser_unref(GWebParser *parser)
{
	if (parser == NULL)
		return;

	if (g_atomic_int_dec_and_test(&parser->ref_count) == FALSE)
		return;

	g_string_free(parser->content, TRUE);

	g_free(parser->begin_token);
	g_free(parser->end_token);
	g_free(parser);
}

void g_web_parser_feed_data(GWebParser *parser,
				const guint8 *data, gsize length)
{
	const guint8 *ptr = data;

	if (parser == NULL)
		return;

	while (length > 0) {
		guint8 chr = parser->token_str[parser->token_pos];

		if (parser->token_pos == 0) {
			guint8 *pos;

			pos = memchr(ptr, chr, length);
			if (pos == NULL) {
				if (parser->intoken == TRUE)
					g_string_append_len(parser->content,
							(gchar *) ptr, length);
				break;
			}

			if (parser->intoken == TRUE)
				g_string_append_len(parser->content,
						(gchar *) ptr, (pos - ptr) + 1);

			length -= (pos - ptr) + 1;
			ptr = pos + 1;

			parser->token_pos++;
			continue;
		}

		if (parser->intoken == TRUE)
			g_string_append_c(parser->content, ptr[0]);

		if (ptr[0] != chr) {
			length--;
			ptr++;

			parser->token_pos = 0;
			continue;
		}

		length--;
		ptr++;

		parser->token_pos++;

		if (parser->token_pos == parser->token_len) {
			if (parser->intoken == FALSE) {
				g_string_append(parser->content,
							parser->token_str);

				parser->intoken = TRUE;
				parser->token_str = parser->end_token;
				parser->token_len = strlen(parser->end_token);
				parser->token_pos = 0;
			} else {
				char *str;
				str = g_string_free(parser->content, FALSE);
				parser->content = g_string_sized_new(0);
				if (parser->func)
					parser->func(str, parser->user_data);
				g_free(str);

				parser->intoken = FALSE;
				parser->token_str = parser->begin_token;
				parser->token_len = strlen(parser->begin_token);
				parser->token_pos = 0;
			}
		}
	}
}

void g_web_parser_end_data(GWebParser *parser)
{
	if (parser == NULL)
		return;
}
