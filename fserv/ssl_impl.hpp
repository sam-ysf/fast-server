#pragma once

#include <chrono>
#include <cstring>
#include <filesystem>
#include <openssl/ssl.h>
#include <string>

namespace fserv::detail {

    /* Helper
     */
    inline int init_ssl_pem_passwd_cb(char* buf, int size, int, void* data)
    {
        const std::string* password = static_cast<std::string*>(data);

        std::memset(buf, 0, static_cast<std::size_t>(size));

        int final_size = std::min(static_cast<int>(password->size()), size);
        std::strncpy(
            buf, password->c_str(), static_cast<std::size_t>(final_size));
        return final_size;
    }

    /* Helper
     */
    inline SSL_CTX* init_ssl_ctx(const std::string& certificate_path,
                                 const std::string& key_file_path,
                                 std::string certificate_password,
                                 std::string key_file_password)
    {
        SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
        if (ctx == nullptr) {
            return nullptr;
        }

        if (!certificate_password.empty()) {
            auto* ptr = static_cast<void*>(&certificate_password);
            SSL_CTX_set_default_passwd_cb(ctx, init_ssl_pem_passwd_cb);
            SSL_CTX_set_default_passwd_cb_userdata(ctx, ptr);
            if (SSL_CTX_use_certificate_chain_file(ctx,
                                                   certificate_path.c_str())
                != 1) {
                SSL_CTX_free(ctx);
                return nullptr;
            }
        } else if (SSL_CTX_use_certificate_chain_file(ctx,
                                                      certificate_path.c_str())
                   != 1) {
            SSL_CTX_free(ctx);
            return nullptr;
        }

        if (!key_file_password.empty()) {
            auto* ptr = static_cast<void*>(&key_file_password);
            SSL_CTX_set_default_passwd_cb(ctx, init_ssl_pem_passwd_cb);
            SSL_CTX_set_default_passwd_cb_userdata(ctx, ptr);
            if (SSL_CTX_use_PrivateKey_file(
                    ctx, key_file_path.c_str(), SSL_FILETYPE_PEM)
                != 1) {
                SSL_CTX_free(ctx);
                return nullptr;
            }
        } else if (SSL_CTX_use_PrivateKey_file(
                       ctx, key_file_path.c_str(), SSL_FILETYPE_PEM)
                   != 1) {
            SSL_CTX_free(ctx);
            return nullptr;
        }

        return ctx;
    }

    /* Helper
     */
    inline bool check_ssl_integrity(const std::string& certificate_file_path,
                                    const std::string& private_key_file_path,
                                    std::string certificate_password,
                                    std::string private_key_password)
    {
        if (!std::filesystem::exists(certificate_file_path)
            || !std::filesystem::exists(private_key_file_path)) {
            return false;
        }

        bool ret = false;

        // Init ssl engine and check integrity
        SSL_CTX* ssl_ctx = SSL_CTX_new(TLS_server_method());
        if (ssl_ctx == nullptr) {
            SSL_CTX_free(ssl_ctx);
            return ret;
        }

        SSL* ssl = SSL_new(ssl_ctx);
        if (ssl == nullptr) {
            SSL_CTX_free(ssl_ctx);
            return ret;
        }

        auto* cert_ptr = static_cast<void*>(&certificate_password);
        SSL_CTX_set_default_passwd_cb(ssl_ctx, init_ssl_pem_passwd_cb);
        SSL_CTX_set_default_passwd_cb_userdata(ssl_ctx, cert_ptr);
        bool certret
            = SSL_use_certificate_chain_file(ssl, certificate_file_path.c_str())
              == 1;

        auto* key_ptr = static_cast<void*>(&private_key_password);
        SSL_set_default_passwd_cb(ssl, init_ssl_pem_passwd_cb);
        SSL_set_default_passwd_cb_userdata(ssl, key_ptr);
        bool keyret = SSL_use_PrivateKey_file(
                          ssl, private_key_file_path.c_str(), SSL_FILETYPE_PEM)
                      == 1;

        bool keycertret = SSL_check_private_key(ssl) == 1;

        SSL_free(ssl);
        SSL_CTX_free(ssl_ctx);
        return keycertret && certret && keyret;
    }

    /* Helper
     */
    inline bool accept_ssl(SSL* ssl, unsigned stall_timeout_interval)
    {
        const auto then = std::chrono::steady_clock::now();

        for (;;) {
            int ret = SSL_accept(ssl);
            if (ret == 1) {
                break;
            }

            if (stall_timeout_interval > 0) {
                // Add timeout for stalled SSL clients
                auto delta
                    = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - then);
                if (delta.count() > stall_timeout_interval) {
                    return false;
                }
            }

            switch (SSL_get_error(ssl, ret)) {
                case SSL_ERROR_WANT_CONNECT:
                case SSL_ERROR_WANT_READ:
                case SSL_ERROR_WANT_WRITE:
                {
                    continue;
                }

                default:
                {
                    return false;
                }
            }
        }

        return true;
    }
} // namespace fserv::detail
