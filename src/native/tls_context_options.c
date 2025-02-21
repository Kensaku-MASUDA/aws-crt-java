/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <jni.h>

#include <aws/common/string.h>
#include <aws/io/tls_channel_handler.h>

#include "crt.h"
#include "java_class_ids.h"
#include "tls_context_pkcs11_options.h"

#include "custom_key_op_handler.h"

/* Have to wrap the native struct so we can manage string lifetime */
struct jni_tls_ctx_options {
    /* Must be first thing in the structure so casts to aws_tls_ctx_options work */
    struct aws_tls_ctx_options options;
    /* these strings get copied from java, so we don't have to pin and track references */
    struct aws_string *ca_file;
    struct aws_string *ca_path;
    struct aws_string *alpn_list;
    struct aws_string *certificate_path;
    struct aws_string *private_key_path;
    struct aws_string *pkcs12_path;
    struct aws_string *pkcs12_password;
    struct aws_string *certificate;
    struct aws_string *private_key;
    struct aws_string *windows_cert_store_path;
    struct aws_string *ca_root;

    struct aws_tls_ctx_pkcs11_options *pkcs11_options;

    struct aws_custom_key_op_handler *custom_key_op_handler;
};

/* on 32-bit platforms, casting pointers to longs throws a warning we don't need */
#if UINTPTR_MAX == 0xffffffff
#    if defined(_MSC_VER)
#        pragma warning(push)
#        pragma warning(disable : 4305) /* 'type cast': truncation from 'jlong' to 'jni_tls_ctx_options *' */
#    else
#        pragma GCC diagnostic push
#        pragma GCC diagnostic ignored "-Wpointer-to-int-cast"
#        pragma GCC diagnostic ignored "-Wint-to-pointer-cast"
#    endif
#endif

static void s_jni_tls_ctx_options_destroy(struct jni_tls_ctx_options *tls) {
    if (tls == NULL) {
        return;
    }

    aws_string_destroy(tls->ca_file);
    aws_string_destroy(tls->ca_path);
    aws_string_destroy(tls->alpn_list);
    aws_string_destroy(tls->certificate_path);
    aws_string_destroy(tls->private_key_path);
    aws_string_destroy(tls->pkcs12_path);
    aws_string_destroy_secure(tls->pkcs12_password);
    aws_string_destroy(tls->certificate);
    aws_string_destroy_secure(tls->private_key);
    aws_string_destroy(tls->windows_cert_store_path);
    aws_string_destroy(tls->ca_root);

    aws_tls_ctx_pkcs11_options_from_java_destroy(tls->pkcs11_options);
    aws_custom_key_op_handler_java_release(tls->custom_key_op_handler);
    aws_tls_ctx_options_clean_up(&tls->options);

    struct aws_allocator *allocator = aws_jni_get_allocator();
    aws_mem_release(allocator, tls);
}

JNIEXPORT
jlong JNICALL Java_software_amazon_awssdk_crt_io_TlsContextOptions_tlsContextOptionsNew(
    JNIEnv *env,
    jclass jni_class,
    jint jni_min_tls_version,
    jint jni_cipher_pref,
    jstring jni_alpn,
    jstring jni_certificate,
    jstring jni_private_key,
    jstring jni_cert_path,
    jstring jni_key_path,
    jstring jni_ca,
    jstring jni_ca_filepath,
    jstring jni_ca_dirpath,
    jboolean jni_verify_peer,
    jstring jni_pkcs12_path,
    jstring jni_pkcs12_password,
    jobject jni_pkcs11_options,
    jobject jni_custom_key_op,
    jstring jni_windows_cert_store_path) {
    (void)jni_class;

    struct aws_allocator *allocator = aws_jni_get_allocator();
    struct jni_tls_ctx_options *tls = aws_mem_calloc(allocator, 1, sizeof(struct jni_tls_ctx_options));
    AWS_FATAL_ASSERT(tls);
    aws_tls_ctx_options_init_default_client(&tls->options, allocator);

    /* Certs or paths will cause an init, which overwrites other fields, so do those first */
    if (jni_certificate && jni_private_key) {
        tls->certificate = aws_jni_new_string_from_jstring(env, jni_certificate);
        if (!tls->certificate) {
            aws_jni_throw_runtime_exception(env, "failed to get certificate string");
            goto on_error;
        }
        tls->private_key = aws_jni_new_string_from_jstring(env, jni_private_key);
        if (!tls->private_key) {
            aws_jni_throw_runtime_exception(env, "failed to get privateKey string");
            goto on_error;
        }

        struct aws_byte_cursor cert_cursor = aws_byte_cursor_from_string(tls->certificate);
        struct aws_byte_cursor key_cursor = aws_byte_cursor_from_string(tls->private_key);

        if (aws_tls_ctx_options_init_client_mtls(&tls->options, allocator, &cert_cursor, &key_cursor)) {
            aws_jni_throw_runtime_exception(env, "aws_tls_ctx_options_init_client_mtls failed");
            goto on_error;
        }
    } else if (jni_cert_path && jni_key_path) {
        tls->certificate_path = aws_jni_new_string_from_jstring(env, jni_cert_path);
        if (!tls->certificate_path) {
            aws_jni_throw_runtime_exception(env, "failed to get certificatePath string");
            goto on_error;
        }
        tls->private_key_path = aws_jni_new_string_from_jstring(env, jni_key_path);
        if (!tls->private_key_path) {
            aws_jni_throw_runtime_exception(env, "failed to get privateKeyPath string");
            goto on_error;
        }

        if (aws_tls_ctx_options_init_client_mtls_from_path(
                &tls->options,
                allocator,
                aws_string_c_str(tls->certificate_path),
                aws_string_c_str(tls->private_key_path))) {
            aws_jni_throw_runtime_exception(env, "aws_tls_ctx_options_init_client_mtls_from_path failed");
            goto on_error;
        }
    } else if (jni_pkcs11_options) {
        tls->pkcs11_options = aws_tls_ctx_pkcs11_options_from_java_new(env, jni_pkcs11_options);
        if (tls->pkcs11_options == NULL) {
            /* exception already thrown */
            goto on_error;
        }

        if (aws_tls_ctx_options_init_client_mtls_with_pkcs11(&tls->options, allocator, tls->pkcs11_options)) {
            aws_jni_throw_runtime_exception(env, "aws_tls_ctx_options_init_client_mtls_with_pkcs11 failed");
            goto on_error;
        }
    } else if (jni_custom_key_op) {

        jobject jni_custom_key_op_handle = (*env)->GetObjectField(
            env, jni_custom_key_op, tls_context_custom_key_operation_options_properties.operation_handler_field_id);
        if (!jni_custom_key_op_handle) {
            aws_jni_throw_runtime_exception(
                env, "could not get custom operation handler from jni_custom_key_op_handle!");
            goto on_error;
        }

        tls->custom_key_op_handler = aws_custom_key_op_handler_java_new(env, jni_custom_key_op_handle);

        /* Certificate needs to be set, but there are multiple ways to get it */
        jstring jni_custom_key_op_cert_path = (*env)->GetObjectField(
            env, jni_custom_key_op, tls_context_custom_key_operation_options_properties.certificate_file_path_field_id);
        jstring jni_custom_key_op_cert_contents = (*env)->GetObjectField(
            env,
            jni_custom_key_op,
            tls_context_custom_key_operation_options_properties.certificate_file_contents_field_id);

        if (jni_custom_key_op_cert_path && jni_custom_key_op_cert_contents) {
            /* Cannot have both a certificate path and data - use one or the other */
            aws_jni_throw_runtime_exception(
                env, "Custom key operation handler: cannot have both certificate file path and certificate contents!");
            goto on_error;
        } else if (jni_custom_key_op_cert_contents) {
            /* If we have the certificate contents, then use it directly */
            tls->certificate = aws_jni_new_string_from_jstring(env, jni_custom_key_op_cert_contents);
            if (!tls->certificate) {
                aws_jni_throw_runtime_exception(
                    env, "Custom key operation handler: failed to get certificate contents string");
                goto on_error;
            }
            struct aws_byte_cursor certificate_byte_cursor = aws_byte_cursor_from_string(tls->certificate);

            /* Initialize the client with a custom key operation */
            if (aws_tls_ctx_options_init_client_mtls_with_custom_key_operations(
                    &tls->options, allocator, tls->custom_key_op_handler, &certificate_byte_cursor) != AWS_OP_SUCCESS) {
                aws_jni_throw_runtime_exception(
                    env, "aws_tls_ctx_options_init_client_mtls_with_custom_key_operations failed");
                goto on_error;
            }
        } else if (jni_custom_key_op_cert_path) {
            /* If we have a certificate path, we need to get the certificate data from it and use that */
            tls->certificate_path = aws_jni_new_string_from_jstring(env, jni_custom_key_op_cert_path);
            if (!tls->certificate_path) {
                aws_jni_throw_runtime_exception(
                    env, "Custom key operation handler: failed to get certificate path string");
                goto on_error;
            }
            struct aws_byte_buf tmp_byte_buf;
            int op = aws_byte_buf_init_from_file(&tmp_byte_buf, allocator, aws_string_c_str(tls->certificate_path));
            if (op != AWS_OP_SUCCESS) {
                aws_jni_throw_runtime_exception(
                    env, "Custom key operation handler: failed to get certificate path string");
                aws_byte_buf_clean_up(&tmp_byte_buf);
                goto on_error;
            }
            struct aws_byte_cursor certificate_byte_cursor = aws_byte_cursor_from_buf(&tmp_byte_buf);

            /* Initialize the client with a custom key operation */
            if (aws_tls_ctx_options_init_client_mtls_with_custom_key_operations(
                    &tls->options, allocator, tls->custom_key_op_handler, &certificate_byte_cursor) != AWS_OP_SUCCESS) {
                aws_jni_throw_runtime_exception(
                    env, "aws_tls_ctx_options_init_client_mtls_with_custom_key_operations failed");
                aws_byte_buf_clean_up(&tmp_byte_buf);
                goto on_error;
            }
            aws_byte_buf_clean_up(&tmp_byte_buf);
        } else {
            aws_jni_throw_runtime_exception(env, "Custom key operation handler: No certificate set!");
            goto on_error;
        }

    } else if (jni_pkcs12_path && jni_pkcs12_password) {
        tls->pkcs12_path = aws_jni_new_string_from_jstring(env, jni_pkcs12_path);
        if (!tls->pkcs12_path) {
            aws_jni_throw_runtime_exception(env, "failed to get pkcs12Path string");
            goto on_error;
        }
        tls->pkcs12_password = aws_jni_new_string_from_jstring(env, jni_pkcs12_password);
        if (!tls->pkcs12_password) {
            aws_jni_throw_runtime_exception(env, "failed to get pkcs12Password string");
            goto on_error;
        }

        struct aws_byte_cursor password = aws_byte_cursor_from_string(tls->pkcs12_password);
        if (aws_tls_ctx_options_init_client_mtls_pkcs12_from_path(
                &tls->options, allocator, aws_string_c_str(tls->pkcs12_path), &password)) {
            aws_jni_throw_runtime_exception(env, "aws_tls_ctx_options_init_client_mtls_pkcs12_from_path failed");
            goto on_error;
        }
    } else if (jni_windows_cert_store_path) {
        tls->windows_cert_store_path = aws_jni_new_string_from_jstring(env, jni_windows_cert_store_path);
        if (!tls->windows_cert_store_path) {
            aws_jni_throw_runtime_exception(env, "failed to get windowsCertStorePath string");
            goto on_error;
        }

        if (aws_tls_ctx_options_init_client_mtls_from_system_path(
                &tls->options, allocator, aws_string_c_str(tls->windows_cert_store_path))) {

            aws_jni_throw_runtime_exception(env, "aws_tls_ctx_options_init_client_mtls_from_system_path failed");
            goto on_error;
        }
    }

    if (jni_ca) {
        tls->ca_root = aws_jni_new_string_from_jstring(env, jni_ca);
        if (!tls->ca_root) {
            aws_jni_throw_runtime_exception(env, "failed to get caRoot string");
            goto on_error;
        }
        struct aws_byte_cursor ca_cursor = aws_byte_cursor_from_string(tls->ca_root);
        if (aws_tls_ctx_options_override_default_trust_store(&tls->options, &ca_cursor)) {
            aws_jni_throw_runtime_exception(env, "aws_tls_ctx_options_override_default_trust_store failed");
            goto on_error;
        }
    } else if (jni_ca_filepath || jni_ca_dirpath) {
        const char *ca_file = NULL;
        const char *ca_path = NULL;
        if (jni_ca_filepath) {
            tls->ca_file = aws_jni_new_string_from_jstring(env, jni_ca_filepath);
            if (!tls->ca_file) {
                aws_jni_throw_runtime_exception(env, "failed to get caFile string");
                goto on_error;
            }

            ca_file = aws_string_c_str(tls->ca_file);
        }
        if (jni_ca_dirpath) {
            tls->ca_path = aws_jni_new_string_from_jstring(env, jni_ca_dirpath);
            if (!tls->ca_path) {
                aws_jni_throw_runtime_exception(env, "failed to get caPath string");
                goto on_error;
            }

            ca_path = aws_string_c_str(tls->ca_path);
        }

        if (aws_tls_ctx_options_override_default_trust_store_from_path(&tls->options, ca_path, ca_file)) {
            aws_jni_throw_runtime_exception(env, "aws_tls_ctx_options_override_default_trust_store_from_path failed");
            goto on_error;
        }
    }

    /* apply the rest of the non-init settings */
    tls->options.minimum_tls_version = (enum aws_tls_versions)jni_min_tls_version;
    tls->options.cipher_pref = (enum aws_tls_cipher_pref)jni_cipher_pref;
    tls->options.verify_peer = jni_verify_peer != 0;

    if (jni_alpn) {
        tls->alpn_list = aws_jni_new_string_from_jstring(env, jni_alpn);
        if (!tls->alpn_list) {
            aws_jni_throw_runtime_exception(env, "failed to get alpnList string");
            goto on_error;
        }

        if (aws_tls_ctx_options_set_alpn_list(&tls->options, aws_string_c_str(tls->alpn_list))) {
            aws_jni_throw_runtime_exception(env, "aws_tls_ctx_options_set_alpn_list failed");
            goto on_error;
        }
    }

    return (jlong)tls;

on_error:

    s_jni_tls_ctx_options_destroy(tls);

    return (jlong)0;
}

JNIEXPORT
void JNICALL Java_software_amazon_awssdk_crt_io_TlsContextOptions_tlsContextOptionsDestroy(
    JNIEnv *env,
    jclass jni_class,
    jlong jni_tls) {
    (void)env;
    (void)jni_class;

    s_jni_tls_ctx_options_destroy((struct jni_tls_ctx_options *)jni_tls);
}

JNIEXPORT
jboolean JNICALL Java_software_amazon_awssdk_crt_io_TlsContextOptions_tlsContextOptionsIsAlpnAvailable(
    JNIEnv *env,
    jclass jni_class) {
    (void)env;
    (void)jni_class;
    return aws_tls_is_alpn_available();
}

JNIEXPORT
jboolean JNICALL Java_software_amazon_awssdk_crt_io_TlsContextOptions_tlsContextOptionsIsCipherPreferenceSupported(
    JNIEnv *env,
    jclass jni_class,
    jint jni_cipher_pref) {

    (void)env;
    (void)jni_class;

    if (jni_cipher_pref < 0 || AWS_IO_TLS_CIPHER_PREF_END_RANGE <= jni_cipher_pref) {
        aws_jni_throw_runtime_exception(
            env,
            "TlsContextOptions.tlsContextOptionsSetCipherPreference: TlsCipherPreference is out of range: %d",
            (int)jni_cipher_pref);
        return false;
    }

    return aws_tls_is_cipher_pref_supported((enum aws_tls_cipher_pref)jni_cipher_pref);
}

#if UINTPTR_MAX == 0xffffffff
#    if defined(_MSC_VER)
#        pragma warning(pop)
#    else
#        pragma GCC diagnostic pop
#    endif
#endif
