/*
MBEDTLS_XBOX_CONFIG.H

The Xbox's changes to Mbed TLS's default configuration
(port/third_party/mbedtls/include/mbedtls/mbedtls_config.h), read after it
(MBEDTLS_USER_CONFIG_FILE, tools/xbox_build.py). The Xbox uses Mbed TLS only
for the self-updater's downloads (port/linux/src/posix_update.c,
port/xbox/src/nxdk_update.c): a TLS 1.2 and 1.3 client that checks a
server's certificate against the certificate authorities the program
carries. Everything else is left out, since the program's code shares the
low 64 MB with the caches.

- No server, no DTLS (internet play's DTLS with browsers is not built for
  the Xbox: nxdk_p2p.c), no pre-shared keys, no static or finite field
  Diffie-Hellman: ECDHE with RSA or ECDSA certificates, as every web server.
- The curves certificate authorities and servers use: P-256, P-384 and
  X25519 (P-521 is rare in chains and is left out).
- AES-GCM and ChaCha20-Poly1305; no CBC suites, no legacy ciphers.
- No certificate dates: an Xbox's clock is whatever its owner set, often
  never; a certificate's chain and host name are still checked.
- No files, sockets or timers of Mbed TLS's own: the certificates are in
  the program, the connection is lwIP's (nxdk_update.c), and the entropy is
  the Xbox's (mbedtls_hardware_poll, nxdk_update.c).
*/

#undef MBEDTLS_HAVE_TIME_DATE
#undef MBEDTLS_FS_IO
#undef MBEDTLS_NET_C
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef MBEDTLS_PSA_ITS_FILE_C
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT

/* (instructions the Pentium III does not have) */
#undef MBEDTLS_AESNI_C
#undef MBEDTLS_AESCE_C
#undef MBEDTLS_PADLOCK_C

#undef MBEDTLS_SELF_TEST
#undef MBEDTLS_DEBUG_C
#undef MBEDTLS_VERSION_FEATURES
#undef MBEDTLS_SSL_ALL_ALERT_MESSAGES
#undef MBEDTLS_GENPRIME

/* the server side, DTLS and session state kept for later */
#undef MBEDTLS_SSL_SRV_C
#undef MBEDTLS_SSL_CACHE_C
#undef MBEDTLS_SSL_COOKIE_C
#undef MBEDTLS_SSL_TICKET_C
#undef MBEDTLS_SSL_SESSION_TICKETS
#undef MBEDTLS_SSL_PROTO_DTLS
#undef MBEDTLS_SSL_DTLS_ANTI_REPLAY
#undef MBEDTLS_SSL_DTLS_HELLO_VERIFY
#undef MBEDTLS_SSL_DTLS_CLIENT_PORT_REUSE
#undef MBEDTLS_SSL_DTLS_CONNECTION_ID
#undef MBEDTLS_SSL_DTLS_CONNECTION_ID_COMPAT
#undef MBEDTLS_SSL_CONTEXT_SERIALIZATION
#undef MBEDTLS_SSL_RENEGOTIATION
#undef MBEDTLS_SSL_KEYING_MATERIAL_EXPORT
#undef MBEDTLS_SSL_ALPN
#undef MBEDTLS_SSL_ENCRYPT_THEN_MAC

/* ECDHE only */
#undef MBEDTLS_KEY_EXCHANGE_PSK_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_DHE_PSK_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_ECDHE_PSK_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_RSA_PSK_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_RSA_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_DHE_RSA_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_ECDH_ECDSA_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_ECDH_RSA_ENABLED
#undef MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_ENABLED
#undef MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_PSK_EPHEMERAL_ENABLED
#undef MBEDTLS_DHM_C
#undef MBEDTLS_ECJPAKE_C

#undef MBEDTLS_ECP_DP_SECP192R1_ENABLED
#undef MBEDTLS_ECP_DP_SECP224R1_ENABLED
#undef MBEDTLS_ECP_DP_SECP521R1_ENABLED
#undef MBEDTLS_ECP_DP_SECP192K1_ENABLED
#undef MBEDTLS_ECP_DP_SECP224K1_ENABLED
#undef MBEDTLS_ECP_DP_SECP256K1_ENABLED
#undef MBEDTLS_ECP_DP_BP256R1_ENABLED
#undef MBEDTLS_ECP_DP_BP384R1_ENABLED
#undef MBEDTLS_ECP_DP_BP512R1_ENABLED
#undef MBEDTLS_ECP_DP_CURVE448_ENABLED

#undef MBEDTLS_CIPHER_MODE_CBC
#undef MBEDTLS_CIPHER_MODE_CFB
#undef MBEDTLS_CIPHER_MODE_OFB
#undef MBEDTLS_CIPHER_MODE_XTS
#undef MBEDTLS_CIPHER_PADDING_PKCS7
#undef MBEDTLS_CIPHER_PADDING_ONE_AND_ZEROS
#undef MBEDTLS_CIPHER_PADDING_ZEROS_AND_LEN
#undef MBEDTLS_CIPHER_PADDING_ZEROS
#undef MBEDTLS_CAMELLIA_C
#undef MBEDTLS_ARIA_C
#undef MBEDTLS_DES_C
#undef MBEDTLS_CCM_C
#undef MBEDTLS_CMAC_C
#undef MBEDTLS_NIST_KW_C
#undef MBEDTLS_LMS_C
#undef MBEDTLS_SHA3_C
#undef MBEDTLS_RIPEMD160_C
#undef MBEDTLS_MD5_C
#undef MBEDTLS_HMAC_DRBG_C
#undef MBEDTLS_ECDSA_DETERMINISTIC

/* certificates read, never written */
#undef MBEDTLS_PEM_WRITE_C
#undef MBEDTLS_PK_WRITE_C
#undef MBEDTLS_PK_RSA_ALT_SUPPORT
#undef MBEDTLS_PKCS5_C
#undef MBEDTLS_PKCS7_C
#undef MBEDTLS_PKCS12_C
#undef MBEDTLS_X509_CRL_PARSE_C
#undef MBEDTLS_X509_CSR_PARSE_C
#undef MBEDTLS_X509_CREATE_C
#undef MBEDTLS_X509_CRT_WRITE_C
#undef MBEDTLS_X509_CSR_WRITE_C

/* the milliseconds clock: the Xbox's tick count (nxdk_update.c) */
#define MBEDTLS_PLATFORM_MS_TIME_ALT
