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

#include "common/platform.h"
#include "slogger/slogger.h"

#include <malloc.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "common/tls_session.h"

namespace {

int checkX509Errors(int preverify_ok, X509_STORE_CTX *x509_ctx) {
	if (preverify_ok == 0) {
		int error = X509_STORE_CTX_get_error(x509_ctx);
		safs::log_err("X509 verification error ({}): {}", error, X509_verify_cert_error_string(error));
	}
	return preverify_ok;
}

}  // namespace

TlsSession::TlsSession(int socket, bool isServer, const std::string &keyFile,
                       const std::string &certFile, const std::string &trustedCAFile,
                       const std::string &expectedHostname) {
	const SSL_METHOD *method = isServer ? TLS_server_method() : TLS_client_method();

	ctx_.reset(SSL_CTX_new(method));
	if (!ctx_) {
		throw OpenSslException("SSL_CTX_new failed: " + opensslErrorString(SSL_ERROR_SSL));
	}

	// Modern minimum protocol
	SSL_CTX_set_min_proto_version(ctx_.get(), TLS1_3_VERSION);

	// Load certificate/key if provided (server or client in mTLS)
	if (certFile != kNoFile) {
		if (SSL_CTX_use_certificate_file(ctx_.get(), certFile.c_str(), SSL_FILETYPE_PEM) != 1) {
			throw OpenSslException("Failed to load certificate: " + certFile + ": " +
			                       opensslErrorString(SSL_ERROR_SSL));
		}
	}

	if (keyFile != kNoFile) {
		if (SSL_CTX_use_PrivateKey_file(ctx_.get(), keyFile.c_str(), SSL_FILETYPE_PEM) != 1) {
			throw OpenSslException("Failed to load private key: " + keyFile + ": " +
			                       opensslErrorString(SSL_ERROR_SSL));
		}

		if (certFile != kNoFile && SSL_CTX_check_private_key(ctx_.get()) != 1) {
			throw OpenSslException("Private key does not match certificate: " +
			                       opensslErrorString(SSL_ERROR_SSL));
		}
	}

	// Trust store CA certificate file
	if (trustedCAFile != kNoFile) {
		if (SSL_CTX_load_verify_locations(ctx_.get(), trustedCAFile.c_str(), nullptr) != 1) {
			throw OpenSslException("Failed to load CA trust file: " + trustedCAFile + ": " +
			                       opensslErrorString(SSL_ERROR_SSL));
		}
	} else {
		// Use system default CA bundle if no explicit CA file is given
		if (SSL_CTX_set_default_verify_paths(ctx_.get()) != 1) {
			throw OpenSslException("Failed to load system default CA trust store: " +
			                       opensslErrorString(SSL_ERROR_SSL));
		}
	}

	int verify = SSL_VERIFY_PEER | (isServer ? SSL_VERIFY_FAIL_IF_NO_PEER_CERT : 0);
	SSL_CTX_set_verify(ctx_.get(), verify, &checkX509Errors);

	// Create SSL object and attach the socket; handshake is done by the caller
	ssl_.reset(SSL_new(ctx_.get()));
	if (!ssl_) { throw OpenSslException("SSL_new failed: " + opensslErrorString(SSL_ERROR_SSL)); }

	if (SSL_set_fd(ssl_.get(), socket) != 1) { throw OpenSslException("SSL_set_fd failed"); }

	// Hostname/IP verification for clients
	if (!isServer && !expectedHostname.empty()) {
		const bool looksLikeIp =
		    (expectedHostname.find_first_not_of("0123456789.") == std::string::npos);

		if (!looksLikeIp) {
			// DNS hostname: enable stricter wildcard handling and set expected host
			SSL_set_hostflags(ssl_.get(), X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
			if (SSL_set1_host(ssl_.get(), expectedHostname.c_str()) != 1) {
				throw OpenSslException("SSL_set1_host failed:" + opensslErrorString(SSL_ERROR_SSL));
			}
			if (SSL_set_tlsext_host_name(ssl_.get(), expectedHostname.c_str()) != 1) {
				throw OpenSslException("SSL_set_tlsext_host_name failed: " +
				                       opensslErrorString(SSL_ERROR_SSL));
			}
		} else {
			// IP literal: set expected peer IP
			X509_VERIFY_PARAM *param = SSL_get0_param(ssl_.get());
			SSL_set_hostflags(ssl_.get(), X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
			X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
			if (X509_VERIFY_PARAM_set1_ip_asc(param, expectedHostname.c_str()) != 1) {
				throw OpenSslException("X509_VERIFY_PARAM_set1_ip_asc failed: " +
				                       opensslErrorString(SSL_ERROR_SSL));
			}
		}
	}
}
