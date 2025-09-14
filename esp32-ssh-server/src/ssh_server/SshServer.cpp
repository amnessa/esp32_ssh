#include "SshServer.h"
#include <Arduino.h>
#include <libssh/libssh.h>
#include <libssh/server.h>
// No SPIFFS/host-key storage; ephemeral in-memory host key

SshServer::SshServer(const char* host_key) : host_key(host_key) {
    ssh_init();
}

void SshServer::begin() {
    libssh_begin();

    sshbind = ssh_bind_new();
    if (!sshbind) {
        Serial.println("ssh_bind_new failed");
        return;
    }

    // Bind to port 22 (standard SSH)
    const int port = 22;
    const char *addr4 = "0.0.0.0";
    char portStr[6];
    snprintf(portStr, sizeof(portStr), "%d", port);
    int rcOpt;

    rcOpt = ssh_bind_options_set(sshbind, SSH_BIND_OPTIONS_BINDADDR, addr4);
    Serial.printf("[SSH] opt BINDADDR(%s) rc=%d\n", addr4, rcOpt);
    rcOpt = ssh_bind_options_set(sshbind, SSH_BIND_OPTIONS_BINDPORT, &port);
    Serial.printf("[SSH] opt BINDPORT(%d) rc=%d\n", port, rcOpt);
    rcOpt = ssh_bind_options_set(sshbind, SSH_BIND_OPTIONS_BINDPORT_STR, portStr);
    Serial.printf("[SSH] opt BINDPORT_STR(%s) rc=%d\n", portStr, rcOpt);
    rcOpt = ssh_bind_options_set(sshbind, SSH_BIND_OPTIONS_HOSTKEY, "ssh-ed25519");
    Serial.printf("[SSH] opt HOSTKEY(ed25519) rc=%d\n", rcOpt);
#ifdef SSH_BIND_OPTIONS_BINDADDR6
    const char *addr6 = "::";
    ssh_bind_options_set(sshbind, SSH_BIND_OPTIONS_BINDADDR6, addr6);
#endif

    // Generate ephemeral ED25519 host key in memory (no filesystem)
    ssh_key genKey = nullptr;
    if (ssh_pki_generate(SSH_KEYTYPE_ED25519, 0, &genKey) == SSH_OK) {
        if (ssh_bind_options_set(sshbind, SSH_BIND_OPTIONS_IMPORT_KEY, genKey) == SSH_OK) {
            Serial.println("[SSH] Using ephemeral ED25519 host key");
        } else {
            Serial.printf("[SSH] Import generated key failed: %s\n", ssh_get_error(sshbind));
        }
    } else {
        Serial.println("[SSH] Host key generation failed");
    }

    Serial.printf("[SSH] Calling ssh_bind_listen() on port %d...\n", port);
    if (ssh_bind_listen(sshbind) < 0) {
        Serial.printf("[SSH] Listen failed: %s\n", ssh_get_error(sshbind));
        return;
    }
    Serial.printf("[SSH] Listening OK on port %d (0.0.0.0 / ::)\n", port);
    // No SPIFFS; no key saving
}

int SshServer::auth_password(ssh_session session, const char *user, const char *password, void *userdata) {
    if (strcmp(user, "cago") == 0 && strcmp(password, "cago1231") == 0) {
        return SSH_AUTH_SUCCESS;
    }
    return SSH_AUTH_DENIED;
}

void SshServer::handleClient() {
    ssh_session sess = ssh_new();
    if (!sess) return;

    Serial.println("[SSH] Waiting for incoming connection (accept blocking)...");
    if (ssh_bind_accept(sshbind, sess) == SSH_ERROR) {
        Serial.printf("Accept failed: %s\n", ssh_get_error(sshbind));
        ssh_free(sess);
        return;
    }
    Serial.println("[SSH] TCP accepted, doing key exchange");
    if (ssh_handle_key_exchange(sess)) {
        Serial.printf("Key exchange failed: %s\n", ssh_get_error(sess));
        ssh_disconnect(sess);
        ssh_free(sess);
        return;
    }

    ssh_set_auth_methods(sess, SSH_AUTH_METHOD_PASSWORD);

    // Authentication loop
    bool authed = false;
    while (!authed) {
        ssh_message m = ssh_message_get(sess);
        if (!m) {
            Serial.println("Auth: no message (client closed)");
            ssh_disconnect(sess);
            ssh_free(sess);
            return;
        }
        if (ssh_message_type(m) == SSH_REQUEST_AUTH &&
            ssh_message_subtype(m) == SSH_AUTH_METHOD_PASSWORD) {

            if (auth_password(sess,
                              ssh_message_auth_user(m),
                              ssh_message_auth_password(m),
                              nullptr) == SSH_AUTH_SUCCESS) {
                ssh_message_auth_reply_success(m, 0);
                ssh_message_free(m);
                Serial.println("Authenticated");
                authed = true;
                break;
            }
            ssh_message_auth_set_methods(m, SSH_AUTH_METHOD_PASSWORD);
            ssh_message_reply_default(m);
            ssh_message_free(m);
            Serial.println("Auth failed (closing)");
            ssh_disconnect(sess);
            ssh_free(sess);
            return;
        }
        ssh_message_reply_default(m);
        ssh_message_free(m);
    }

    // Channel open
    ssh_channel ch = nullptr;
    while (!ch) {
        ssh_message m = ssh_message_get(sess);
        if (!m) {
            Serial.println("Channel: no message");
            ssh_disconnect(sess);
            ssh_free(sess);
            return;
        }
        if (ssh_message_type(m) == SSH_REQUEST_CHANNEL_OPEN &&
            ssh_message_subtype(m) == SSH_CHANNEL_SESSION) {
            ch = ssh_message_channel_request_open_reply_accept(m);
            ssh_message_free(m);
            Serial.println("Channel opened");
            break;
        }
        ssh_message_reply_default(m);
        ssh_message_free(m);
    }

    // PTY and Shell request
    bool shellReady = false;
    while (!shellReady) {
        ssh_message m = ssh_message_get(sess);
        if (!m) {
            Serial.println("Shell: no message");
            if (ch) ssh_channel_free(ch);
            ssh_disconnect(sess);
            ssh_free(sess);
            return;
        }
        if (ssh_message_type(m) == SSH_REQUEST_CHANNEL) {
            int subtype = ssh_message_subtype(m);
            if (subtype == SSH_CHANNEL_REQUEST_PTY) {
                ssh_message_channel_request_reply_success(m);
                ssh_message_free(m);
                Serial.println("PTY allocated");
                continue; // wait for shell/exec
            }
            if (subtype == SSH_CHANNEL_REQUEST_ENV) {
                // Accept env to keep clients happy (ignored)
                ssh_message_channel_request_reply_success(m);
                ssh_message_free(m);
                continue;
            }
            if (subtype == SSH_CHANNEL_REQUEST_SHELL) {
                ssh_message_channel_request_reply_success(m);
                ssh_message_free(m);
                Serial.println("Shell started (echo mode)");
                shellReady = true;
                break;
            }
        }
        ssh_message_reply_default(m);
        ssh_message_free(m);
    }

    // Echo loop
    char buf[256];
    while (ssh_channel_is_open(ch) && !ssh_channel_is_eof(ch)) {
        int r = ssh_channel_read(ch, buf, sizeof(buf), 0);
        if (r > 0) {
            ssh_channel_write(ch, buf, r);
        } else if (r == SSH_ERROR) {
            Serial.println("Channel read error");
            break;
        }
        delay(1);
    }

    if (ch) ssh_channel_free(ch);
    ssh_disconnect(sess);
    ssh_free(sess);
    Serial.println("Session closed");
}
