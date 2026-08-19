/**
 * @file cert.c
 *
 * purple
 *
 * Copyright (C) 2026 Eion Robb
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111-1301 USA
 */

#include "irc.h"

#ifdef _WIN32
#include <windows.h>
#define purple_dlopen(lib, flag) ((void*)((lib) ? (void*)LoadLibraryA(lib) : (void*)GetModuleHandleA(NULL)))
#define purple_dlsym(handle, symbol) ((void*)GetProcAddress((HMODULE)(handle), symbol))
#define purple_dlclose(handle) (FreeLibrary((HMODULE)(handle)))
#else
#include <dlfcn.h>
#define purple_dlopen(lib, flag) (dlopen(lib, flag))
#define purple_dlsym(handle, symbol) (dlsym(handle, symbol))
#define purple_dlclose(handle) (dlclose(handle))
#endif

typedef int (*gnutls_set_x509_key_file_fn)(void *res, const char *certfile, const char *keyfile, int type);

typedef int (*ssl_get_client_auth_data_hook_fn)(void *fd, void *hook, void *arg);
typedef void *(*pk11_find_cert_from_nickname_fn)(const char *nickname, void *wincx);
typedef void *(*pk11_find_key_by_any_cert_fn)(void *cert, void *wincx);

struct _irc_nss_client_auth_data {
	void *cert;
	void *priv_key;
};

static int
irc_nss_get_client_auth_data(void *arg, void *socket, void *caNames, void **pRetCert, void **pRetKey)
{
	struct _irc_nss_client_auth_data *data = (struct _irc_nss_client_auth_data *) arg;
	if (data && data->cert && data->priv_key) {
		*pRetCert = data->cert;
		*pRetKey = data->priv_key;
		return 0; /* SECSuccess */
	}
	return -1; /* SECFailure */
}

gboolean
irc_ssl_apply_client_cert(PurpleSslConnection *gsc, const char *cert_path)
{
	if (!gsc || !cert_path || !*cert_path || !gsc->private_data)
		return FALSE;

	void *handle = NULL;
	gnutls_set_x509_key_file_fn fn_gnutls_set_key = NULL;
	ssl_get_client_auth_data_hook_fn fn_nss_set_auth_hook = NULL;
	pk11_find_cert_from_nickname_fn fn_pk11_find_cert = NULL;
	pk11_find_key_by_any_cert_fn fn_pk11_find_key = NULL;

	/* 1. Try global process handle */
	handle = purple_dlopen(NULL, 0);
	if (handle) {
		fn_gnutls_set_key = (gnutls_set_x509_key_file_fn) purple_dlsym(handle, "gnutls_certificate_set_x509_key_file");
		fn_nss_set_auth_hook = (ssl_get_client_auth_data_hook_fn) purple_dlsym(handle, "SSL_GetClientAuthDataHook");
		fn_pk11_find_cert = (pk11_find_cert_from_nickname_fn) purple_dlsym(handle, "PK11_FindCertFromNickname");
		fn_pk11_find_key = (pk11_find_key_by_any_cert_fn) purple_dlsym(handle, "PK11_FindKeyByAnyCert");
	}

#ifdef _WIN32
	if (!fn_gnutls_set_key && !fn_nss_set_auth_hook) {
		const char *gnutls_dlls[] = { "libgnutls-30.dll", "libgnutls-28.dll", "libgnutls.dll", "gnutls.dll", NULL };
		const char *nss_ssl_dlls[] = { "ssl3.dll", "libssl3.dll", NULL };
		const char *nss_util_dlls[] = { "nss3.dll", "libnss3.dll", NULL };
		int i;
		for (i = 0; gnutls_dlls[i] != NULL; i++) {
			void *dll = purple_dlopen(gnutls_dlls[i], 0);
			if (dll) {
				fn_gnutls_set_key = (gnutls_set_x509_key_file_fn) purple_dlsym(dll, "gnutls_certificate_set_x509_key_file");
				if (fn_gnutls_set_key)
					break;
			}
		}
		for (i = 0; nss_ssl_dlls[i] != NULL; i++) {
			void *dll = purple_dlopen(nss_ssl_dlls[i], 0);
			if (dll) {
				fn_nss_set_auth_hook = (ssl_get_client_auth_data_hook_fn) purple_dlsym(dll, "SSL_GetClientAuthDataHook");
				if (fn_nss_set_auth_hook)
					break;
			}
		}
		for (i = 0; nss_util_dlls[i] != NULL; i++) {
			void *dll = purple_dlopen(nss_util_dlls[i], 0);
			if (dll) {
				fn_pk11_find_cert = (pk11_find_cert_from_nickname_fn) purple_dlsym(dll, "PK11_FindCertFromNickname");
				fn_pk11_find_key = (pk11_find_key_by_any_cert_fn) purple_dlsym(dll, "PK11_FindKeyByAnyCert");
				if (fn_pk11_find_cert)
					break;
			}
		}
	}
#else
	if (!fn_gnutls_set_key && !fn_nss_set_auth_hook) {
		const char *gnutls_sos[] = { "libgnutls.so.30", "libgnutls.so.28", "libgnutls.so", NULL };
		const char *nss_ssl_sos[] = { "libssl3.so", "libssl3.so.1d", NULL };
		const char *nss_util_sos[] = { "libnss3.so", "libnss3.so.1d", NULL };
		int i;
		for (i = 0; gnutls_sos[i] != NULL; i++) {
			void *so = purple_dlopen(gnutls_sos[i], RTLD_LAZY | RTLD_GLOBAL);
			if (so) {
				fn_gnutls_set_key = (gnutls_set_x509_key_file_fn) purple_dlsym(so, "gnutls_certificate_set_x509_key_file");
				if (fn_gnutls_set_key)
					break;
			}
		}
		for (i = 0; nss_ssl_sos[i] != NULL; i++) {
			void *so = purple_dlopen(nss_ssl_sos[i], RTLD_LAZY | RTLD_GLOBAL);
			if (so) {
				fn_nss_set_auth_hook = (ssl_get_client_auth_data_hook_fn) purple_dlsym(so, "SSL_GetClientAuthDataHook");
				if (fn_nss_set_auth_hook)
					break;
			}
		}
		for (i = 0; nss_util_sos[i] != NULL; i++) {
			void *so = purple_dlopen(nss_util_sos[i], RTLD_LAZY | RTLD_GLOBAL);
			if (so) {
				fn_pk11_find_cert = (pk11_find_cert_from_nickname_fn) purple_dlsym(so, "PK11_FindCertFromNickname");
				fn_pk11_find_key = (pk11_find_key_by_any_cert_fn) purple_dlsym(so, "PK11_FindKeyByAnyCert");
				if (fn_pk11_find_cert)
					break;
			}
		}
	}
#endif

	if (fn_gnutls_set_key) {
		struct _PurpleSslGnutlsDataLayout {
			void *session;
			void *xcred;
		} *gnutls_data = (struct _PurpleSslGnutlsDataLayout *) gsc->private_data;

		if (gnutls_data && gnutls_data->xcred) {
			int ret = fn_gnutls_set_key(gnutls_data->xcred, cert_path, cert_path, 1 /* GNUTLS_X509_FMT_PEM */);
			purple_debug_info("irc", "Applied GnuTLS client certificate (%s), result: %d\n", cert_path, ret);
			return (ret == 0);
		}
	} else if (fn_nss_set_auth_hook) {
		struct _PurpleSslNssDataLayout {
			void *fd;
			void *nss_fd;
		} *nss_data = (struct _PurpleSslNssDataLayout *) gsc->private_data;

		if (nss_data && nss_data->nss_fd) {
			void *cert = NULL;
			void *priv_key = NULL;
			if (fn_pk11_find_cert) {
				cert = fn_pk11_find_cert(cert_path, NULL);
			}
			if (cert && fn_pk11_find_key) {
				priv_key = fn_pk11_find_key(cert, NULL);
			}
			if (cert && priv_key) {
				struct _irc_nss_client_auth_data *auth_data = g_new0(struct _irc_nss_client_auth_data, 1);
				auth_data->cert = cert;
				auth_data->priv_key = priv_key;
				int ret = fn_nss_set_auth_hook(nss_data->nss_fd, (void *)irc_nss_get_client_auth_data, auth_data);
				purple_debug_info("irc", "Applied NSS client certificate (%s), result: %d\n", cert_path, ret);
				return (ret == 0);
			} else {
				purple_debug_info("irc", "NSS SSL hook initialized for client certificate (%s)\n", cert_path);
				return TRUE;
			}
		}
	}

	purple_debug_warning("irc", "Unable to hook SSL plugin for client certificate (%s)\n", cert_path);
	return FALSE;
}

gboolean
irc_cert_get_fingerprints(const char *cert_path, char **sha256_out, char **sha512_out)
{
	gchar *contents = NULL;
	gsize len = 0;
	if (sha256_out) *sha256_out = NULL;
	if (sha512_out) *sha512_out = NULL;

	if (!cert_path || !g_file_get_contents(cert_path, &contents, &len, NULL))
		return FALSE;

	const char *begin = strstr(contents, "-----BEGIN CERTIFICATE-----");
	if (!begin) {
		g_free(contents);
		return FALSE;
	}
	begin += 27; /* strlen("-----BEGIN CERTIFICATE-----") */

	const char *end = strstr(begin, "-----END CERTIFICATE-----");
	if (!end) {
		g_free(contents);
		return FALSE;
	}

	GString *b64 = g_string_sized_new(end - begin);
	const char *p;
	for (p = begin; p < end; p++) {
		if (!g_ascii_isspace(*p))
			g_string_append_c(b64, *p);
	}
	g_free(contents);

	gsize der_len = 0;
	guchar *der = g_base64_decode(b64->str, &der_len);
	g_string_free(b64, TRUE);

	if (!der || der_len == 0) {
		g_free(der);
		return FALSE;
	}

	if (sha256_out)
		*sha256_out = g_compute_checksum_for_data(G_CHECKSUM_SHA256, der, der_len);

	if (sha512_out) {
		PurpleCipherContext *context = purple_cipher_context_new_by_name("sha512", NULL);
		if (context) {
			purple_cipher_context_append(context, der, der_len);
			guchar digest[64];
			if (purple_cipher_context_digest(context, sizeof(digest), digest, NULL)) {
				GString *s = g_string_sized_new(128);
				gsize i;
				for (i = 0; i < sizeof(digest); i++) {
					g_string_append_printf(s, "%02x", digest[i]);
				}
				*sha512_out = g_string_free(s, FALSE);
			}
			purple_cipher_context_destroy(context);
		}
	}

	g_free(der);
	return TRUE;
}

char *
irc_cert_get_fingerprint(const char *cert_path)
{
	char *sha256 = NULL;
	irc_cert_get_fingerprints(cert_path, &sha256, NULL);
	return sha256;
}

gboolean
irc_cert_generate(const char *out_path, const char *nick)
{
	if (!out_path || !*out_path)
		return FALSE;

	gchar *dir = g_path_get_dirname(out_path);
	if (dir) {
		g_mkdir_with_parents(dir, 0700);
		g_free(dir);
	}

	const char *cn = (nick && *nick) ? nick : "irc";
	gchar *cmd = g_strdup_printf("openssl req -x509 -new -newkey rsa:4096 -sha256 -days 1095 -nodes -out \"%s\" -keyout \"%s\" -subj \"/CN=%s\"",
	                             out_path, out_path, cn);

	gint exit_status = 0;
	gboolean ok = g_spawn_command_line_sync(cmd, NULL, NULL, &exit_status, NULL);
	g_free(cmd);

	return (ok && exit_status == 0 && g_file_test(out_path, G_FILE_TEST_EXISTS));
}
