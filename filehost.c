/**
 * @file filehost.c
 *
 * purple2-ircv3
 *
 * HTTP Filehost extension (draft/FILEHOST, soju.im/FILEHOST, FILEHOST)
 * Specification: https://github.com/ircv3/ircv3-specifications/pull/562
 *                https://codeberg.org/emersion/soju/src/branch/master/doc/ext/filehost.md
 */

#include "filehost.h"
#include <glib/gstdio.h>
#include <sys/stat.h>

struct irc_filehost_xfer {
	PurpleXfer *xfer;
	PurpleConnection *gc;
	struct irc_conn *irc;
	char *target;
	char *host;
	int port;
	char *path;
	gboolean is_ssl;
	PurpleSslConnection *gsc;
	PurpleProxyConnectData *connect_data;
	int fd;
	guint watcher;
	FILE *fp;
	gsize total_size;
	gsize file_sent;
	GString *tx_hdr;
	gsize tx_hdr_sent;
	GString *rx_buf;
	gboolean reading_response;
};

static void irc_filehost_destroy_data(struct irc_filehost_xfer *fh);

static const char *
irc_filehost_guess_mime(const char *filename)
{
	const char *dot;

	if (!filename)
		return "application/octet-stream";

	dot = strrchr(filename, '.');
	if (!dot)
		return "application/octet-stream";

	dot++;
	if (g_ascii_strcasecmp(dot, "png") == 0)
		return "image/png";
	else if (g_ascii_strcasecmp(dot, "jpg") == 0 || g_ascii_strcasecmp(dot, "jpeg") == 0)
		return "image/jpeg";
	else if (g_ascii_strcasecmp(dot, "gif") == 0)
		return "image/gif";
	else if (g_ascii_strcasecmp(dot, "webp") == 0)
		return "image/webp";
	else if (g_ascii_strcasecmp(dot, "svg") == 0)
		return "image/svg+xml";
	else if (g_ascii_strcasecmp(dot, "mp4") == 0)
		return "video/mp4";
	else if (g_ascii_strcasecmp(dot, "webm") == 0)
		return "video/webm";
	else if (g_ascii_strcasecmp(dot, "mp3") == 0)
		return "audio/mpeg";
	else if (g_ascii_strcasecmp(dot, "ogg") == 0)
		return "audio/ogg";
	else if (g_ascii_strcasecmp(dot, "txt") == 0 || g_ascii_strcasecmp(dot, "log") == 0 ||
	         g_ascii_strcasecmp(dot, "c") == 0 || g_ascii_strcasecmp(dot, "h") == 0 ||
	         g_ascii_strcasecmp(dot, "md") == 0)
		return "text/plain; charset=utf-8";
	else if (g_ascii_strcasecmp(dot, "pdf") == 0)
		return "application/pdf";
	else if (g_ascii_strcasecmp(dot, "zip") == 0)
		return "application/zip";
	else if (g_ascii_strcasecmp(dot, "gz") == 0 || g_ascii_strcasecmp(dot, "tgz") == 0)
		return "application/gzip";

	return "application/octet-stream";
}

gboolean
irc_can_receive_file(PurpleConnection *gc, const char *who)
{
	struct irc_conn *irc;

	if (!gc || !gc->proto_data)
		return FALSE;

	irc = gc->proto_data;
	if (irc->filehost_url != NULL)
		return TRUE;

	/* Without FILEHOST, standard DCC SEND only supports 1-to-1 buddy transfers */
	return !irc_ischannel(who);
}

gboolean
irc_chat_can_receive_file(PurpleConnection *gc, int id)
{
	struct irc_conn *irc;

	if (!gc || !gc->proto_data)
		return FALSE;

	irc = gc->proto_data;
	return (irc->filehost_url != NULL);
}

void
irc_chat_send_file(PurpleConnection *gc, int id, const char *filename)
{
	PurpleConversation *conv = purple_find_chat(gc, id);
	if (conv)
		irc_dccsend_send_file(gc, purple_conversation_get_name(conv), filename);
}

static void
irc_filehost_send_result_message(struct irc_filehost_xfer *fh, const char *file_url)
{
	struct irc_conn *irc = fh->irc;
	const char *target = fh->target;

	if (!target || !*target || !file_url || !*file_url)
		return;

	if (irc_ischannel(target)) {
		PurpleConversation *conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, target, irc->account);
		if (conv) {
			purple_conv_chat_send(PURPLE_CONV_CHAT(conv), file_url);
		} else {
			const char *args[2];
			args[0] = target;
			args[1] = file_url;
			irc_cmd_privmsg(irc, "msg", NULL, args);
		}
	} else {
		PurpleConversation *conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, target, irc->account);
		if (conv) {
			purple_conv_im_send(PURPLE_CONV_IM(conv), file_url);
		} else {
			const char *args[2];
			args[0] = target;
			args[1] = file_url;
			irc_cmd_privmsg(irc, "msg", NULL, args);
		}
	}
}

static void
irc_filehost_finish_upload(struct irc_filehost_xfer *fh)
{
	PurpleXfer *xfer = fh->xfer;
	PurpleConnection *gc = fh->gc;
	const char *rx = fh->rx_buf ? fh->rx_buf->str : "";
	int status_code = 0;
	const char *loc_hdr;
	char *file_url = NULL;

	if (sscanf(rx, "HTTP/%*s %d", &status_code) == 1 && status_code >= 200 && status_code < 300) {
		loc_hdr = purple_strcasestr(rx, "\nLocation:");
		if (!loc_hdr)
			loc_hdr = purple_strcasestr(rx, "\rLocation:");
		if (!loc_hdr && strncmp(rx, "Location:", 9) == 0)
			loc_hdr = rx;

		if (loc_hdr) {
			const char *start = loc_hdr + (loc_hdr == rx ? 9 : 10);
			const char *end;
			while (*start == ' ' || *start == '\t')
				start++;
			end = strpbrk(start, "\r\n");
			if (end)
				file_url = g_strndup(start, end - start);
			else
				file_url = g_strdup(start);
			g_strstrip(file_url);
		}

		if (file_url && *file_url) {
			char *full_url;
			if (strstr(file_url, "://") == NULL) {
				/* Relative URL: resolve with host and scheme */
				if (*file_url == '/') {
					if ((fh->is_ssl && fh->port == 443) || (!fh->is_ssl && fh->port == 80))
						full_url = g_strdup_printf("%s://%s%s", fh->is_ssl ? "https" : "http", fh->host, file_url);
					else
						full_url = g_strdup_printf("%s://%s:%d%s", fh->is_ssl ? "https" : "http", fh->host, fh->port, file_url);
				} else {
					if ((fh->is_ssl && fh->port == 443) || (!fh->is_ssl && fh->port == 80))
						full_url = g_strdup_printf("%s://%s/%s", fh->is_ssl ? "https" : "http", fh->host, file_url);
					else
						full_url = g_strdup_printf("%s://%s:%d/%s", fh->is_ssl ? "https" : "http", fh->host, fh->port, file_url);
				}
				g_free(file_url);
				file_url = full_url;
			}

			purple_debug_info("irc", "Filehost upload succeeded: %s\n", file_url);
			irc_filehost_send_result_message(fh, file_url);
			g_free(file_url);

			purple_xfer_set_completed(xfer, TRUE);
			purple_xfer_end(xfer);
			return;
		}
	}

	purple_debug_error("irc", "Filehost upload failed (HTTP %d):\n%s\n", status_code, rx);
	purple_notify_error(gc, _("File Transfer Failed"), _("Failed to upload file to HTTP file host."),
	                    status_code > 0 ? g_strdup_printf(_("Server returned HTTP %d"), status_code) : _("No response from server"));
	purple_xfer_cancel_local(xfer);
}

static void
irc_filehost_write_step(struct irc_filehost_xfer *fh)
{
	PurpleXfer *xfer = fh->xfer;
	PurpleXferUiOps *ui_ops = purple_xfer_get_ui_ops(xfer);

	/* 1. Send HTTP request headers */
	if (fh->tx_hdr_sent < fh->tx_hdr->len) {
		gsize to_send = fh->tx_hdr->len - fh->tx_hdr_sent;
		gssize sent;

		if (fh->is_ssl)
			sent = purple_ssl_write(fh->gsc, fh->tx_hdr->str + fh->tx_hdr_sent, to_send);
		else
			sent = write(fh->fd, fh->tx_hdr->str + fh->tx_hdr_sent, to_send);

		if (sent < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;
			purple_debug_error("irc", "Filehost header write failed: %s\n", g_strerror(errno));
			purple_xfer_cancel_local(xfer);
			return;
		}
		fh->tx_hdr_sent += sent;
		if (fh->tx_hdr_sent < fh->tx_hdr->len)
			return;
	}

	/* 2. Stream file body */
	if (fh->file_sent < fh->total_size) {
		guchar buf[16384];
		gsize to_read = MIN(sizeof(buf), fh->total_size - fh->file_sent);
		size_t read_bytes = fread(buf, 1, to_read, fh->fp);
		gssize sent;

		if (read_bytes == 0) {
			purple_debug_error("irc", "Filehost read error on local file\n");
			purple_xfer_cancel_local(xfer);
			return;
		}

		if (fh->is_ssl)
			sent = purple_ssl_write(fh->gsc, buf, read_bytes);
		else
			sent = write(fh->fd, buf, read_bytes);

		if (sent < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				fseek(fh->fp, fh->file_sent, SEEK_SET);
				return;
			}
			purple_debug_error("irc", "Filehost body write failed: %s\n", g_strerror(errno));
			purple_xfer_cancel_local(xfer);
			return;
		}

		if ((size_t)sent < read_bytes)
			fseek(fh->fp, fh->file_sent + sent, SEEK_SET);

		fh->file_sent += sent;
		purple_xfer_set_bytes_sent(xfer, fh->file_sent);

		if (ui_ops && ui_ops->update_progress)
			ui_ops->update_progress(xfer, purple_xfer_get_progress(xfer));

		if (fh->file_sent < fh->total_size)
			return;
	}

	/* 3. Done writing request body; switch to reading HTTP response */
	if (!fh->reading_response) {
		fh->reading_response = TRUE;
		if (fh->fp) {
			fclose(fh->fp);
			fh->fp = NULL;
		}

		if (!fh->is_ssl) {
			if (fh->watcher) {
				purple_input_remove(fh->watcher);
				fh->watcher = 0;
			}
			/* Listen for incoming HTTP response */
			/* Wait until readable */
		}
	}
}

static void
irc_filehost_ssl_input_cb(gpointer data, PurpleSslConnection *gsc, PurpleInputCondition cond)
{
	struct irc_filehost_xfer *fh = data;

	if (!fh->reading_response) {
		irc_filehost_write_step(fh);
		return;
	}

	/* Reading response */
	for (;;) {
		char buf[4096];
		gssize len = purple_ssl_read(gsc, buf, sizeof(buf) - 1);
		if (len > 0) {
			buf[len] = '\0';
			if (!fh->rx_buf)
				fh->rx_buf = g_string_new(NULL);
			g_string_append_len(fh->rx_buf, buf, len);
		} else if (len == 0) {
			/* Server closed connection, response complete */
			irc_filehost_finish_upload(fh);
			return;
		} else {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;
			/* Connection finished or closed */
			irc_filehost_finish_upload(fh);
			return;
		}
	}
}

static void
irc_filehost_fd_io_cb(gpointer data, gint source, PurpleInputCondition cond)
{
	struct irc_filehost_xfer *fh = data;

	if ((cond & PURPLE_INPUT_WRITE) && !fh->reading_response) {
		irc_filehost_write_step(fh);
		if (fh->reading_response) {
			if (fh->watcher)
				purple_input_remove(fh->watcher);
			fh->watcher = purple_input_add(fh->fd, PURPLE_INPUT_READ, irc_filehost_fd_io_cb, fh);
		}
		return;
	}

	if (cond & PURPLE_INPUT_READ) {
		for (;;) {
			char buf[4096];
			gssize len = read(fh->fd, buf, sizeof(buf) - 1);
			if (len > 0) {
				buf[len] = '\0';
				if (!fh->rx_buf)
					fh->rx_buf = g_string_new(NULL);
				g_string_append_len(fh->rx_buf, buf, len);
			} else if (len == 0) {
				/* EOF */
				irc_filehost_finish_upload(fh);
				return;
			} else {
				if (errno == EAGAIN || errno == EWOULDBLOCK)
					return;
				irc_filehost_finish_upload(fh);
				return;
			}
		}
	}
}

static void
irc_filehost_ssl_connect_cb(gpointer data, PurpleSslConnection *gsc, PurpleInputCondition cond)
{
	struct irc_filehost_xfer *fh = data;

	purple_debug_info("irc", "Filehost SSL connected to %s:%d\n", fh->host, fh->port);
	purple_ssl_input_add(gsc, irc_filehost_ssl_input_cb, fh);
	irc_filehost_write_step(fh);
}

static void
irc_filehost_ssl_error_cb(PurpleSslConnection *gsc, PurpleSslErrorType error, gpointer data)
{
	struct irc_filehost_xfer *fh = data;

	purple_debug_error("irc", "Filehost SSL connection error: %d\n", error);
	purple_notify_error(fh->gc, _("File Transfer Failed"), _("Unable to connect to HTTP file host."),
	                    purple_ssl_strerror(error));
	purple_xfer_cancel_local(fh->xfer);
}

static void
irc_filehost_proxy_connect_cb(gpointer data, gint source, const gchar *error_message)
{
	struct irc_filehost_xfer *fh = data;

	fh->connect_data = NULL;
	if (source < 0) {
		purple_debug_error("irc", "Filehost proxy connection failed: %s\n", error_message ? error_message : "unknown");
		purple_notify_error(fh->gc, _("File Transfer Failed"), _("Unable to connect to HTTP file host."),
		                    error_message ? error_message : _("Connection failed"));
		purple_xfer_cancel_local(fh->xfer);
		return;
	}

	fh->fd = source;
	purple_debug_info("irc", "Filehost plain HTTP connected to %s:%d (fd %d)\n", fh->host, fh->port, fh->fd);
	fh->watcher = purple_input_add(fh->fd, PURPLE_INPUT_WRITE, irc_filehost_fd_io_cb, fh);
	irc_filehost_write_step(fh);
}

static void
irc_filehost_destroy_data(struct irc_filehost_xfer *fh)
{
	if (!fh)
		return;

	if (fh->watcher) {
		purple_input_remove(fh->watcher);
		fh->watcher = 0;
	}

	if (fh->connect_data) {
		purple_proxy_connect_cancel(fh->connect_data);
		fh->connect_data = NULL;
	}

	if (fh->gsc) {
		purple_ssl_close(fh->gsc);
		fh->gsc = NULL;
	}

	if (fh->fd >= 0) {
		close(fh->fd);
		fh->fd = -1;
	}

	if (fh->fp) {
		fclose(fh->fp);
		fh->fp = NULL;
	}

	if (fh->tx_hdr) {
		g_string_free(fh->tx_hdr, TRUE);
		fh->tx_hdr = NULL;
	}

	if (fh->rx_buf) {
		g_string_free(fh->rx_buf, TRUE);
		fh->rx_buf = NULL;
	}

	g_free(fh->target);
	g_free(fh->host);
	g_free(fh->path);
	g_free(fh);
}

void
irc_filehost_destroy(PurpleXfer *xfer)
{
	struct irc_filehost_xfer *fh;

	if (!xfer || !xfer->data)
		return;

	fh = xfer->data;
	xfer->data = NULL;
	irc_filehost_destroy_data(fh);
}

void
irc_filehost_send_init(PurpleXfer *xfer)
{
	PurpleAccount *account;
	PurpleConnection *gc;
	struct irc_conn *irc;
	struct irc_filehost_xfer *fh;
	const char *local_file;
	char *basename;
	GStatBuf st;
	char *host = NULL;
	int port = 0;
	char *path = NULL;
	char *user = NULL;
	char *passwd = NULL;
	gboolean is_ssl;
	const char *mime_type;
	GString *hdr;
	char *safe_filename;

	account = purple_xfer_get_account(xfer);
	gc = purple_account_get_connection(account);
	if (!gc || !gc->proto_data) {
		purple_xfer_cancel_local(xfer);
		return;
	}
	irc = gc->proto_data;

	if (!irc->filehost_url || !*irc->filehost_url) {
		purple_notify_error(gc, _("File Transfer Failed"), _("Server does not advertise an HTTP file host."), NULL);
		purple_xfer_cancel_local(xfer);
		return;
	}

	local_file = purple_xfer_get_local_filename(xfer);
	if (!local_file || g_stat(local_file, &st) != 0 || !S_ISREG(st.st_mode)) {
		purple_notify_error(gc, _("File Transfer Failed"), _("Could not read local file."), local_file);
		purple_xfer_cancel_local(xfer);
		return;
	}

	basename = g_path_get_basename(local_file);
	g_free(xfer->filename);
	xfer->filename = basename;
	purple_xfer_set_size(xfer, st.st_size);

	if (!purple_url_parse(irc->filehost_url, &host, &port, &path, &user, &passwd)) {
		purple_notify_error(gc, _("File Transfer Failed"), _("Invalid FILEHOST URL advertised by server."), irc->filehost_url);
		g_free(host);
		g_free(path);
		g_free(user);
		g_free(passwd);
		purple_xfer_cancel_local(xfer);
		return;
	}
	g_free(user);
	g_free(passwd);

	is_ssl = (purple_strcasestr(irc->filehost_url, "https://") == irc->filehost_url);
	if (is_ssl && !purple_ssl_is_supported()) {
		purple_notify_error(gc, _("File Transfer Failed"), _("Server requires SSL/TLS for filehost, but SSL is not supported."), NULL);
		g_free(host);
		g_free(path);
		purple_xfer_cancel_local(xfer);
		return;
	}

	mime_type = irc_filehost_guess_mime(xfer->filename);
	safe_filename = g_path_get_basename(local_file);

	hdr = g_string_new(NULL);
	g_string_append_printf(hdr, "POST %s HTTP/1.1\r\n", (path && *path) ? path : "/");
	if ((is_ssl && port == 443) || (!is_ssl && port == 80))
		g_string_append_printf(hdr, "Host: %s\r\n", host);
	else
		g_string_append_printf(hdr, "Host: %s:%d\r\n", host, port);

	g_string_append_printf(hdr, "User-Agent: %s\r\n", purple_core_get_ui());
	g_string_append_printf(hdr, "Content-Type: %s\r\n", mime_type);
	g_string_append_printf(hdr, "Content-Disposition: inline; filename=\"%s\"\r\n", safe_filename);
	g_string_append_printf(hdr, "Content-Length: %" G_GSIZE_FORMAT "\r\n", (gsize)st.st_size);
	g_free(safe_filename);

	/* Spec: If client authenticated via SASL, include Authorization header */
	if (irc->sasl_auth_mech) {
		if (g_ascii_strcasecmp(irc->sasl_auth_mech, "PLAIN") == 0) {
			const char *saslname = purple_account_get_string(account, "saslname", NULL);
			const char *password = purple_account_get_password(account);
			if (!saslname || !*saslname)
				saslname = purple_connection_get_display_name(gc);
			if (saslname && password) {
				char *userpass = g_strdup_printf("%s:%s", saslname, password);
				char *b64 = g_base64_encode((const guchar *)userpass, strlen(userpass));
				g_string_append_printf(hdr, "Authorization: Basic %s\r\n", b64);
				g_free(b64);
				g_free(userpass);
			}
		} else if (g_ascii_strcasecmp(irc->sasl_auth_mech, "OAUTHBEARER") == 0) {
			const char *token = purple_account_get_password(account);
			if (token && *token)
				g_string_append_printf(hdr, "Authorization: Bearer %s\r\n", token);
		}
	}

	g_string_append(hdr, "Connection: close\r\n\r\n");

	fh = g_new0(struct irc_filehost_xfer, 1);
	fh->xfer = xfer;
	fh->gc = gc;
	fh->irc = irc;
	fh->target = g_strdup(xfer->who);
	fh->host = host;
	fh->port = port;
	fh->path = path;
	fh->is_ssl = is_ssl;
	fh->fd = -1;
	fh->total_size = st.st_size;
	fh->tx_hdr = hdr;

	fh->fp = g_fopen(local_file, "rb");
	if (!fh->fp) {
		purple_notify_error(gc, _("File Transfer Failed"), _("Unable to open local file for reading."), local_file);
		irc_filehost_destroy_data(fh);
		purple_xfer_cancel_local(xfer);
		return;
	}

	xfer->data = fh;
	purple_xfer_ref(xfer);
	xfer->status = PURPLE_XFER_STATUS_STARTED;

	if (is_ssl) {
		fh->gsc = purple_ssl_connect(account, host, port, irc_filehost_ssl_connect_cb,
		                             irc_filehost_ssl_error_cb, fh);
		if (!fh->gsc) {
			purple_notify_error(gc, _("File Transfer Failed"), _("Unable to initiate SSL connection to file host."), host);
			purple_xfer_unref(xfer);
			irc_filehost_destroy(xfer);
			purple_xfer_cancel_local(xfer);
			return;
		}
	} else {
		fh->connect_data = purple_proxy_connect(NULL, account, host, port,
		                                        irc_filehost_proxy_connect_cb, fh);
		if (!fh->connect_data) {
			purple_notify_error(gc, _("File Transfer Failed"), _("Unable to initiate connection to file host."), host);
			purple_xfer_unref(xfer);
			irc_filehost_destroy(xfer);
			purple_xfer_cancel_local(xfer);
			return;
		}
	}
}
