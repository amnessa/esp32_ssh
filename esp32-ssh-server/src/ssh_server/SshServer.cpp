#include "SshServer.h"
#include <Arduino.h>
#include <libssh/callbacks.h>
#include <libssh/server.h>
#include <sys/stat.h>
#include "SPIFFS.h"

SshServer::SshServer(const char* host_key) {
    this->host_key = host_key;
    ssh_init();
}

void SshServer::begin() {
    libssh_begin();
    sshbind = ssh_bind_new();
    if(!sshbind){
        Serial.println("ssh_bind_new failed");
        return;
    }

    // Set port 22 explicitly.
    int port = 22;
    if (ssh_bind_options_set(sshbind, SSH_BIND_OPTIONS_BINDPORT, &port) != SSH_OK) {
        Serial.printf("Failed set port: %s\n", ssh_get_error(sshbind));
    }

    bool haveFile = false;
    if (host_key && strlen(host_key) && SPIFFS.begin(true)) {
        haveFile = SPIFFS.exists(host_key);
    }

    if (haveFile) {
        // Pick option based on filename suffix.
        const char *p = host_key;
        if (strstr(p, "ed25519") != nullptr) {
            if (ssh_bind_options_set(sshbind, SSH_BIND_OPTIONS_ED25519KEY, host_key) != SSH_OK) {
                Serial.printf("Set ED25519 key failed: %s\n", ssh_get_error(sshbind));
            }
        } else if (strstr(p, "rsa") != nullptr) {
            if (ssh_bind_options_set(sshbind, SSH_BIND_OPTIONS_RSAKEY, host_key) != SSH_OK) {
                Serial.printf("Set RSA key failed: %s\n", ssh_get_error(sshbind));
            }
        } else {
            // Fallback assume ed25519
            if (ssh_bind_options_set(sshbind, SSH_BIND_OPTIONS_ED25519KEY, host_key) != SSH_OK) {
                Serial.printf("Set (fallback) ED25519 key failed: %s\n", ssh_get_error(sshbind));
            }
        }
    } else {
        // Generate an ephemeral ED25519 host key in RAM if no file.
        ssh_key genKey = nullptr;
        if (ssh_pki_generate(SSH_KEYTYPE_ED25519, 0, &genKey) == SSH_OK) {
            if (ssh_bind_options_set(sshbind, SSH_BIND_OPTIONS_IMPORT_KEY, genKey) != SSH_OK) {
                Serial.printf("Import generated key failed: %s\n", ssh_get_error(sshbind));
            }
            // (Optional) Could write to SPIFFS later.
        } else {
            Serial.println("Host key generation failed");
        }
    }

    if (ssh_bind_listen(sshbind) < 0) {
        Serial.printf("Error listening: %s\n", ssh_get_error(sshbind));
        return;
    }
    Serial.println("SSH Server listening on port 22");
}

int SshServer::auth_password(ssh_session session, const char *user, const char *password, void *userdata) {
    if (strcmp(user, "cago") == 0 && strcmp(password, "cago1231") == 0) {
        return SSH_AUTH_SUCCESS;
    }
    return SSH_AUTH_DENIED;
}

void SshServer::handleClient() {
    session = ssh_new();
    if (session == NULL) {
        return;
    }

    if (ssh_bind_accept(sshbind, session) == SSH_ERROR) {
        Serial.printf("Accept failed: %s\n", ssh_get_error(sshbind));
        ssh_free(session);
        return;
    }

    if (ssh_handle_key_exchange(session)) {
        Serial.printf("Key exchange failed: %s\n", ssh_get_error(session));
        ssh_disconnect(session);
        ssh_free(session);
        return;
    }

    ssh_set_auth_methods(session, SSH_AUTH_METHOD_PASSWORD);

    ssh_message message;
    do {
        message = ssh_message_get(session);
        if(!message)
            break;
        if(ssh_message_type(message) == SSH_REQUEST_AUTH) {
            if(ssh_message_subtype(message) == SSH_AUTH_METHOD_PASSWORD) {
                if(auth_password(session,
                   ssh_message_auth_user(message),
                   ssh_message_auth_password(message),
                   NULL) == SSH_AUTH_SUCCESS) {
                    ssh_message_auth_reply_success(message, 0);
                    ssh_message_free(message);
                    break;
                }
            }
            ssh_message_auth_set_methods(message, SSH_AUTH_METHOD_PASSWORD);
            ssh_message_reply_default(message);
            ssh_message_free(message);
            // Disconnect on auth failure
            ssh_disconnect(session);
            ssh_free(session);
            return;
        } else {
            ssh_message_reply_default(message);
            ssh_message_free(message);
        }
    } while (true);


    ssh_channel channel = NULL;
    do {
        message = ssh_message_get(session);
        if (message != NULL) {
            if (ssh_message_type(message) == SSH_REQUEST_CHANNEL_OPEN && ssh_message_subtype(message) == SSH_CHANNEL_SESSION) {
                channel = ssh_message_channel_request_open_reply_accept(message);
                ssh_message_free(message);
                break;
            } else {
                ssh_message_reply_default(message);
                ssh_message_free(message);
            }
        }
    } while (!channel);


    char buffer[256];
    int i;
    do {
        message = ssh_message_get(session);
        if (message != NULL) {
            if (ssh_message_type(message) == SSH_REQUEST_CHANNEL) {
                if (ssh_message_subtype(message) == SSH_CHANNEL_REQUEST_SHELL) {
                    ssh_message_channel_request_reply_success(message);
                    ssh_message_free(message);
                    break;
                }
            }
            ssh_message_reply_default(message);
            ssh_message_free(message);
        }
    } while (true);


    while (ssh_channel_is_open(channel) && !ssh_channel_is_eof(channel)) {
        int nbytes = ssh_channel_read(channel, buffer, sizeof(buffer), 0);
        if (nbytes > 0) {
            ssh_channel_write(channel, buffer, nbytes);
        }
    }
    ssh_channel_free(channel);
    ssh_disconnect(session);
    ssh_free(session);
}
