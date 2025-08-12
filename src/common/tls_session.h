/*
   Copyright 2026 Leil Storage OÜ

   This file is part of SaunaFS.

   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "common/platform.h"

#include <memory>
#include <string>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "common/exception.h"

SAUNAFS_CREATE_EXCEPTION_CLASS(OpenSslException, Exception);

inline std::string opensslErrorString(int sslErrorCode) {
	switch (sslErrorCode) {
	case SSL_ERROR_WANT_READ:
		return "WANT_READ";
	case SSL_ERROR_WANT_WRITE:
		return "WANT_WRITE";
	case SSL_ERROR_ZERO_RETURN:
		return "PEER_CLOSED";
	case SSL_ERROR_SYSCALL:
		return "SYSCALL_ERROR";
	case SSL_ERROR_SSL: {
		unsigned long err = ERR_get_error();
		if (err) {
			char buf[256];
			ERR_error_string_n(err, buf, sizeof(buf));
			return std::string("SSL_PROTOCOL_ERROR: ") + buf;
		}
		return "SSL_PROTOCOL_ERROR";
	}
	default:
		return "UNKNOWN_ERROR";
	}
}

class TlsSession {
public:
	// NOLINTNEXTLINE(readability-redundant-string-init)
	static constexpr std::string_view kNoFile = "";

	/**
	 * Constructs a TLS session for the given socket.
	 * @param socket The underlying connected socket file descriptor.
	 * @param isServer True if this session is for a server, false for a client.
	 * @param keyFile Path to the private key file in PEM format, or kNoFile to skip.
	 * @param certFile Path to the certificate file in PEM format, or kNoFile to skip.
	 * @param trustedCAFile Path to the CA trust file, or kNoFile to skip.
	 * @param expectedHostname Expected hostname for server certificate verification (client mode
	 * only)
	 **/
	TlsSession(int socket, bool isServer, const std::string &keyFile, const std::string &certFile,
	           const std::string &trustedCAFile, const std::string &expectedHostname = "");

	// Returns the underlying OpenSSL session (use SSL_connect/SSL_accept outside)
	SSL *session() const { return ssl_.get(); }

	// Returns the context (for advanced tweaks if needed)
	SSL_CTX *context() const { return ctx_.get(); }

	// Returns the context (for advanced tweaks if needed)

private:
	struct CtxDeleter {
		void operator()(SSL_CTX *ctx) const {
			if (ctx) SSL_CTX_free(ctx);
		}
	};
	struct SslDeleter {
		void operator()(SSL *ssl) const {
			if (ssl) SSL_free(ssl);
		}
	};

	using OpenSslCtx = std::unique_ptr<SSL_CTX, CtxDeleter>;
	using OpenSsl = std::unique_ptr<SSL, SslDeleter>;

	OpenSslCtx ctx_;
	OpenSsl ssl_;
};
