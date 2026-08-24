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

typedef struct SECItemStr {
	int type;
	unsigned char *data;
	unsigned int len;
} SECItem;

typedef void *(*fn_CERT_GetDefaultCertDB_t)(void);
typedef void *(*fn_CERT_NewTempCertificate_t)(void *certdb, SECItem *derCert, void *nickname, int isPerm, int copyDER);
typedef void *(*fn_CERT_DupCertificate_t)(void *cert);
typedef void (*fn_CERT_DestroyCertificate_t)(void *cert);

typedef void *(*fn_PK11_GetInternalKeySlot_t)(void);
typedef void (*fn_PK11_FreeSlot_t)(void *slot);
typedef int (*fn_PK11_ImportDERPrivateKeyInfoAndReturnKey_t)(void *slot, SECItem *derPKI, void *nickname, void *publicValue, int isPerm, int isPrivate, unsigned int keyUsage, void **privk, void *wincx);
typedef void *(*fn_SECKEY_CopyPrivateKey_t)(void *privk);
typedef void (*fn_SECKEY_DestroyPrivateKey_t)(void *privk);

typedef int (*fn_SSL_GetClientAuthDataHook_t)(void *fd, int (*hook)(void *arg, void *socket, void *caNames, void **pRetCert, void **pRetKey), void *arg);

typedef int (*gnutls_certificate_allocate_credentials_fn)(void **res);
typedef int (*gnutls_set_x509_key_file_fn)(void *res, const char *certfile, const char *keyfile, int type);
typedef int (*gnutls_credentials_set_fn)(void *session, int type, void *cred);

struct _irc_nss_client_auth_data {
	void *cert;
	void *priv_key;
	fn_CERT_DupCertificate_t fn_dup_cert;
	fn_SECKEY_CopyPrivateKey_t fn_copy_key;
};

static int
irc_nss_get_client_auth_data(void *arg, void *socket, void *caNames, void **pRetCert, void **pRetKey)
{
	struct _irc_nss_client_auth_data *data = (struct _irc_nss_client_auth_data *) arg;
	purple_debug_info("irc", "NSS ClientAuth hook triggered during TLS handshake\n");
	if (data && data->cert && data->priv_key && data->fn_dup_cert && data->fn_copy_key) {
		*pRetCert = data->fn_dup_cert(data->cert);
		*pRetKey = data->fn_copy_key(data->priv_key);
		purple_debug_info("irc", "Supplied client certificate and private key to NSS\n");
		return 0; /* SECSuccess */
	}
	purple_debug_warning("irc", "NSS ClientAuth hook called but auth data is missing\n");
	return -1; /* SECFailure */
}

static guchar *
wrap_pkcs1_to_pkcs8(const guchar *pkcs1_der, gsize pkcs1_len, gsize *pkcs8_len_out)
{
	static const guchar rsa_alg_id[] = {
		0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01, 0x05, 0x00
	};
	static const guchar ver[] = { 0x02, 0x01, 0x00 };

	GByteArray *oct = g_byte_array_new();
	g_byte_array_append(oct, ver, sizeof(ver));
	g_byte_array_append(oct, rsa_alg_id, sizeof(rsa_alg_id));

	guint8 oct_tag = 0x04;
	g_byte_array_append(oct, &oct_tag, 1);
	if (pkcs1_len < 128) {
		guint8 l = (guint8) pkcs1_len;
		g_byte_array_append(oct, &l, 1);
	} else if (pkcs1_len < 256) {
		guint8 l[] = { 0x81, (guint8) pkcs1_len };
		g_byte_array_append(oct, l, 2);
	} else if (pkcs1_len < 65536) {
		guint8 l[] = { 0x82, (guint8)(pkcs1_len >> 8), (guint8)(pkcs1_len & 0xff) };
		g_byte_array_append(oct, l, 3);
	} else {
		g_byte_array_free(oct, TRUE);
		return NULL;
	}
	g_byte_array_append(oct, pkcs1_der, pkcs1_len);

	GByteArray *seq = g_byte_array_new();
	guint8 seq_tag = 0x30;
	g_byte_array_append(seq, &seq_tag, 1);
	if (oct->len < 128) {
		guint8 l = (guint8) oct->len;
		g_byte_array_append(seq, &l, 1);
	} else if (oct->len < 256) {
		guint8 l[] = { 0x81, (guint8) oct->len };
		g_byte_array_append(seq, l, 2);
	} else if (oct->len < 65536) {
		guint8 l[] = { 0x82, (guint8)(oct->len >> 8), (guint8)(oct->len & 0xff) };
		g_byte_array_append(seq, l, 3);
	} else {
		g_byte_array_free(oct, TRUE);
		g_byte_array_free(seq, TRUE);
		return NULL;
	}
	g_byte_array_append(seq, oct->data, oct->len);
	g_byte_array_free(oct, TRUE);

	*pkcs8_len_out = seq->len;
	return g_byte_array_free(seq, FALSE);
}

gboolean
irc_ssl_apply_client_cert(PurpleSslConnection *gsc, const char *cert_path)
{
	purple_debug_info("irc", "irc_ssl_apply_client_cert called for %s\n", cert_path ? cert_path : "(null)");
	if (!gsc || !cert_path || !*cert_path) {
		purple_debug_warning("irc", "irc_ssl_apply_client_cert: gsc or cert_path is invalid\n");
		return FALSE;
	}
	if (!gsc->private_data) {
		purple_debug_warning("irc", "irc_ssl_apply_client_cert: gsc->private_data is NULL\n");
		return FALSE;
	}

	gchar *contents = NULL;
	gsize len = 0;
	GError *err = NULL;
	if (!g_file_get_contents(cert_path, &contents, &len, &err)) {
		purple_debug_error("irc", "Failed to read client certificate file %s: %s\n",
		                   cert_path, err ? err->message : "unknown error");
		if (err) g_error_free(err);
		return FALSE;
	}

	/* Extract Certificate DER */
	guchar *cert_der = NULL;
	gsize cert_der_len = 0;
	const char *cert_begin = strstr(contents, "-----BEGIN CERTIFICATE-----");
	if (cert_begin) {
		cert_begin += 27;
		const char *cert_end = strstr(cert_begin, "-----END CERTIFICATE-----");
		if (cert_end) {
			GString *b64 = g_string_sized_new(cert_end - cert_begin);
			const char *p;
			for (p = cert_begin; p < cert_end; p++) {
				if (!g_ascii_isspace(*p))
					g_string_append_c(b64, *p);
			}
			cert_der = g_base64_decode(b64->str, &cert_der_len);
			g_string_free(b64, TRUE);
		}
	}

	/* Extract Private Key DER */
	guchar *key_der = NULL;
	gsize key_der_len = 0;
	const char *key_begin = strstr(contents, "-----BEGIN PRIVATE KEY-----");
	if (key_begin) {
		key_begin += 27;
		const char *key_end = strstr(key_begin, "-----END PRIVATE KEY-----");
		if (key_end) {
			GString *b64 = g_string_sized_new(key_end - key_begin);
			const char *p;
			for (p = key_begin; p < key_end; p++) {
				if (!g_ascii_isspace(*p))
					g_string_append_c(b64, *p);
			}
			key_der = g_base64_decode(b64->str, &key_der_len);
			g_string_free(b64, TRUE);
		}
	} else if ((key_begin = strstr(contents, "-----BEGIN RSA PRIVATE KEY-----"))) {
		key_begin += 31;
		const char *key_end = strstr(key_begin, "-----END RSA PRIVATE KEY-----");
		if (key_end) {
			GString *b64 = g_string_sized_new(key_end - key_begin);
			const char *p;
			for (p = key_begin; p < key_end; p++) {
				if (!g_ascii_isspace(*p))
					g_string_append_c(b64, *p);
			}
			gsize raw_key_len = 0;
			guchar *raw_key = g_base64_decode(b64->str, &raw_key_len);
			g_string_free(b64, TRUE);
			if (raw_key && raw_key_len > 0) {
				key_der = wrap_pkcs1_to_pkcs8(raw_key, raw_key_len, &key_der_len);
				g_free(raw_key);
			}
		}
	}
	g_free(contents);

	if (!cert_der || cert_der_len == 0 || !key_der || key_der_len == 0) {
		purple_debug_error("irc", "Failed to parse PEM certificate or private key from %s (cert=%u bytes, key=%u bytes)\n",
		                   cert_path, (guint)cert_der_len, (guint)key_der_len);
		g_free(cert_der);
		g_free(key_der);
		return FALSE;
	}

	purple_debug_info("irc", "Parsed PEM cert (%u bytes DER) and key (%u bytes PKCS#8 DER)\n",
	                  (guint)cert_der_len, (guint)key_der_len);

	/* Try resolving NSS symbols */
	void *nss_dll = NULL;
	void *ssl_dll = NULL;

#ifdef _WIN32
	nss_dll = purple_dlopen("nss3.dll", 0);
	if (!nss_dll) nss_dll = purple_dlopen("libnss3.dll", 0);
	ssl_dll = purple_dlopen("ssl3.dll", 0);
	if (!ssl_dll) ssl_dll = purple_dlopen("libssl3.dll", 0);
#else
	nss_dll = purple_dlopen("libnss3.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!nss_dll) nss_dll = purple_dlopen("libnss3.so.1d", RTLD_LAZY | RTLD_GLOBAL);
	ssl_dll = purple_dlopen("libssl3.so", RTLD_LAZY | RTLD_GLOBAL);
	if (!ssl_dll) ssl_dll = purple_dlopen("libssl3.so.1d", RTLD_LAZY | RTLD_GLOBAL);
#endif
	if (!nss_dll) nss_dll = purple_dlopen(NULL, 0);
	if (!ssl_dll) ssl_dll = purple_dlopen(NULL, 0);

	fn_CERT_GetDefaultCertDB_t fn_CERT_GetDefaultCertDB = NULL;
	fn_CERT_NewTempCertificate_t fn_CERT_NewTempCertificate = NULL;
	fn_CERT_DupCertificate_t fn_CERT_DupCertificate = NULL;
	fn_PK11_GetInternalKeySlot_t fn_PK11_GetInternalKeySlot = NULL;
	fn_PK11_FreeSlot_t fn_PK11_FreeSlot = NULL;
	fn_PK11_ImportDERPrivateKeyInfoAndReturnKey_t fn_PK11_ImportDERPrivateKeyInfoAndReturnKey = NULL;
	fn_SECKEY_CopyPrivateKey_t fn_SECKEY_CopyPrivateKey = NULL;
	fn_SSL_GetClientAuthDataHook_t fn_SSL_GetClientAuthDataHook = NULL;

	if (nss_dll) {
		fn_CERT_GetDefaultCertDB = (fn_CERT_GetDefaultCertDB_t) purple_dlsym(nss_dll, "CERT_GetDefaultCertDB");
		fn_CERT_NewTempCertificate = (fn_CERT_NewTempCertificate_t) purple_dlsym(nss_dll, "CERT_NewTempCertificate");
		fn_CERT_DupCertificate = (fn_CERT_DupCertificate_t) purple_dlsym(nss_dll, "CERT_DupCertificate");
		fn_PK11_GetInternalKeySlot = (fn_PK11_GetInternalKeySlot_t) purple_dlsym(nss_dll, "PK11_GetInternalKeySlot");
		fn_PK11_FreeSlot = (fn_PK11_FreeSlot_t) purple_dlsym(nss_dll, "PK11_FreeSlot");
		fn_PK11_ImportDERPrivateKeyInfoAndReturnKey = (fn_PK11_ImportDERPrivateKeyInfoAndReturnKey_t) purple_dlsym(nss_dll, "PK11_ImportDERPrivateKeyInfoAndReturnKey");
		fn_SECKEY_CopyPrivateKey = (fn_SECKEY_CopyPrivateKey_t) purple_dlsym(nss_dll, "SECKEY_CopyPrivateKey");
	}
	if (ssl_dll) {
		fn_SSL_GetClientAuthDataHook = (fn_SSL_GetClientAuthDataHook_t) purple_dlsym(ssl_dll, "SSL_GetClientAuthDataHook");
	}

	purple_debug_info("irc", "NSS functions resolved: CertDB=%p, TempCert=%p, KeySlot=%p, ImportKey=%p, AuthHook=%p\n",
	                  fn_CERT_GetDefaultCertDB, fn_CERT_NewTempCertificate, fn_PK11_GetInternalKeySlot,
	                  fn_PK11_ImportDERPrivateKeyInfoAndReturnKey, fn_SSL_GetClientAuthDataHook);

	if (fn_CERT_GetDefaultCertDB && fn_CERT_NewTempCertificate && fn_CERT_DupCertificate &&
	    fn_PK11_GetInternalKeySlot && fn_PK11_FreeSlot && fn_PK11_ImportDERPrivateKeyInfoAndReturnKey &&
	    fn_SECKEY_CopyPrivateKey && fn_SSL_GetClientAuthDataHook) {

		struct _PurpleSslNssDataLayout {
			void *fd;
			void *in;
			guint handshake_handler;
			guint handshake_timer;
		} *nss_data = (struct _PurpleSslNssDataLayout *) gsc->private_data;

		if (!nss_data || !nss_data->in) {
			purple_debug_error("irc", "NSS socket (nss_data->in) is NULL\n");
			g_free(cert_der);
			g_free(key_der);
			return FALSE;
		}

		SECItem cert_item;
		cert_item.type = 0; /* siBuffer */
		cert_item.data = cert_der;
		cert_item.len = cert_der_len;

		void *certdb = fn_CERT_GetDefaultCertDB();
		void *cert = fn_CERT_NewTempCertificate(certdb, &cert_item, NULL, 0 /* PR_FALSE */, 1 /* PR_TRUE */);
		if (!cert) {
			purple_debug_error("irc", "CERT_NewTempCertificate failed\n");
			g_free(cert_der);
			g_free(key_der);
			return FALSE;
		}

		SECItem key_item;
		key_item.type = 0; /* siBuffer */
		key_item.data = key_der;
		key_item.len = key_der_len;

		void *slot = fn_PK11_GetInternalKeySlot();
		void *priv_key = NULL;
		int rv = fn_PK11_ImportDERPrivateKeyInfoAndReturnKey(slot, &key_item, NULL, NULL,
		                                                     0 /* isPerm: PR_FALSE */,
		                                                     1 /* isPrivate: PR_TRUE */,
		                                                     0xffffffff /* KU_ALL */,
		                                                     &priv_key, NULL);
		fn_PK11_FreeSlot(slot);

		if (rv != 0 || !priv_key) {
			purple_debug_error("irc", "PK11_ImportDERPrivateKeyInfoAndReturnKey failed (result: %d)\n", rv);
			g_free(cert_der);
			g_free(key_der);
			return FALSE;
		}

		struct _irc_nss_client_auth_data *auth_data = g_new0(struct _irc_nss_client_auth_data, 1);
		auth_data->cert = cert;
		auth_data->priv_key = priv_key;
		auth_data->fn_dup_cert = fn_CERT_DupCertificate;
		auth_data->fn_copy_key = fn_SECKEY_CopyPrivateKey;

		int hook_ret = fn_SSL_GetClientAuthDataHook(nss_data->in, irc_nss_get_client_auth_data, auth_data);
		purple_debug_info("irc", "SSL_GetClientAuthDataHook installed on NSS socket (result: %d)\n", hook_ret);

		g_free(cert_der);
		g_free(key_der);
		return (hook_ret == 0);
	}

	g_free(cert_der);
	g_free(key_der);

	/* Try GnuTLS fallback */
	void *gnutls_dll = purple_dlopen(NULL, 0);
#ifdef _WIN32
	if (!gnutls_dll) gnutls_dll = purple_dlopen("libgnutls-30.dll", 0);
	if (!gnutls_dll) gnutls_dll = purple_dlopen("libgnutls-28.dll", 0);
#else
	if (!gnutls_dll) gnutls_dll = purple_dlopen("libgnutls.so.30", RTLD_LAZY | RTLD_GLOBAL);
	if (!gnutls_dll) gnutls_dll = purple_dlopen("libgnutls.so.28", RTLD_LAZY | RTLD_GLOBAL);
#endif
	if (gnutls_dll) {
		gnutls_certificate_allocate_credentials_fn fn_cred_alloc = (gnutls_certificate_allocate_credentials_fn) purple_dlsym(gnutls_dll, "gnutls_certificate_allocate_credentials");
		gnutls_set_x509_key_file_fn fn_set_key = (gnutls_set_x509_key_file_fn) purple_dlsym(gnutls_dll, "gnutls_certificate_set_x509_key_file");
		gnutls_credentials_set_fn fn_cred_set = (gnutls_credentials_set_fn) purple_dlsym(gnutls_dll, "gnutls_credentials_set");

		if (fn_cred_alloc && fn_set_key && fn_cred_set) {
			void *xcred = NULL;
			fn_cred_alloc(&xcred);
			int ret = fn_set_key(xcred, cert_path, cert_path, 1 /* GNUTLS_X509_FMT_PEM */);
			struct _PurpleSslGnutlsDataLayout {
				void *session;
				guint handshake_handler;
				guint handshake_timer;
			} *gdata = (struct _PurpleSslGnutlsDataLayout *) gsc->private_data;

			if (gdata && gdata->session) {
				int sret = fn_cred_set(gdata->session, 1 /* GNUTLS_CRD_CERTIFICATE */, xcred);
				purple_debug_info("irc", "Applied GnuTLS client certificate (%s), key_ret: %d, cred_ret: %d\n", cert_path, ret, sret);
				return (ret == 0 && sret == 0);
			}
		}
	}

	purple_debug_warning("irc", "Unable to hook SSL plugin for client certificate (%s)\n", cert_path);
	return FALSE;
}

gboolean
irc_cert_get_fingerprints(const char *cert_path, char **sha1_out, char **sha256_out, char **sha512_out)
{
	gchar *contents = NULL;
	gsize len = 0;
	if (sha1_out) *sha1_out = NULL;
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

	if (sha1_out)
		*sha1_out = g_compute_checksum_for_data(G_CHECKSUM_SHA1, der, der_len);

	if (sha256_out)
		*sha256_out = g_compute_checksum_for_data(G_CHECKSUM_SHA256, der, der_len);

	if (sha512_out)
		*sha512_out = g_compute_checksum_for_data(G_CHECKSUM_SHA512, der, der_len);

	g_free(der);
	return TRUE;
}

char *
irc_cert_get_fingerprint(const char *cert_path)
{
	char *sha256 = NULL;
	irc_cert_get_fingerprints(cert_path, NULL, &sha256, NULL);
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
