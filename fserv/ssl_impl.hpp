#pragma once

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
        std::strncpy(buf, password->c_str(), password->size());
        return static_cast<int>(password->size());
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

        int certret = 1;
        int keyret = 1;

        if (!certificate_password.empty()) {
            auto* ptr = static_cast<void*>(&certificate_password);
            SSL_CTX_set_default_passwd_cb(ssl_ctx, init_ssl_pem_passwd_cb);
            SSL_CTX_set_default_passwd_cb_userdata(ssl_ctx, ptr);
            certret = SSL_use_certificate_chain_file(
                ssl, certificate_file_path.c_str());
        }

        if (!private_key_password.empty()) {
            auto* ptr = static_cast<void*>(&private_key_password);
            SSL_set_default_passwd_cb(ssl, init_ssl_pem_passwd_cb);
            SSL_set_default_passwd_cb_userdata(ssl, ptr);
            keyret = SSL_use_PrivateKey_file(
                ssl, private_key_file_path.c_str(), SSL_FILETYPE_PEM);
        }

        ret = certret == 1 && keyret == 1;

        SSL_free(ssl);
        SSL_CTX_free(ssl_ctx);
        return ret;
    }

    /* Helper
     */
    inline bool accept_ssl(SSL* ssl)
    {
        for (;;) {
            int ret = SSL_accept(ssl);
            if (ret == 1) {
                break;
            }

            switch (SSL_get_error(ssl, ret)) {
                case SSL_ERROR_WANT_READ:
                    [[fallthrough]];
                case SSL_ERROR_WANT_CONNECT:
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
