#include "SshServer.h"
#include <Arduino.h>
#include <libssh/libssh.h>
#include <libssh/server.h>
#include "SPIFFS.h"

SshServer::SshServer(const char* host_key) : host_key(host_key) {
    ssh_init();
}

void SshServer::begin() {
    libssh_begin();

    if (!SPIFFS.begin(false) && !SPIFFS.begin(true)) {
        Serial.println("SPIFFS mount failed");
        return;
    }

    // Flat path (SPIFFS: no true dirs)
    const char *fallbackPath = "/id_ed25519";
    const char *keyPath = (host_key && *host_key) ? host_key : fallbackPath;
    Serial.printf("[SSH] Using host key path: %s\n", keyPath);

    sshbind = ssh_bind_new();
    if (!sshbind) {
        Serial.println("ssh_bind_new failed");
        return;
    }

    // Test on higher port first to avoid any reserved/priv interactions then you can switch back to 22.
    // Change this to 22 after confirming basic accept path works.
    // Use higher port for initial diagnostic; change to 22 later
#ifndef SSH_SERVER_PORT
#define SSH_SERVER_PORT 2222
#endif
    const int port = SSH_SERVER_PORT;
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

    bool loaded = false;
    if (SPIFFS.exists(keyPath)) {
        ssh_key fileKey = nullptr;
        int rc = ssh_pki_import_privkey_file(keyPath, nullptr, nullptr, nullptr, &fileKey);
        if (rc == SSH_OK && fileKey) {
            if (ssh_bind_options_set(sshbind, SSH_BIND_OPTIONS_IMPORT_KEY, fileKey) == SSH_OK) {
                Serial.printf("[SSH] Host key loaded: %s\n", keyPath);
                loaded = true;
            } else {
                Serial.printf("[SSH] Import key option failed: %s\n", ssh_get_error(sshbind));
                ssh_key_free(fileKey);
            }
        } else {
            Serial.printf("[SSH] Import privkey failed rc=%d\n", rc);
        }
    }
    if (!loaded) {
        ssh_key genKey = nullptr;
        if (ssh_pki_generate(SSH_KEYTYPE_ED25519, 0, &genKey) == SSH_OK) {
            if (ssh_bind_options_set(sshbind, SSH_BIND_OPTIONS_IMPORT_KEY, genKey) == SSH_OK) {
                Serial.println("[SSH] Generated ED25519 host key (ephemeral until saved)");
                int rc = ssh_pki_export_privkey_file(genKey, nullptr, nullptr, nullptr, keyPath);
                if (rc == SSH_OK) Serial.printf("[SSH] Saved host key %s\n", keyPath);
                else Serial.printf("[SSH] Save host key failed rc=%d\n", rc);
            } else {
                Serial.printf("[SSH] Import generated key failed: %s\n", ssh_get_error(sshbind));
            }
        } else {
            Serial.println("[SSH] Host key generation failed");
        }
    }

    Serial.printf("[SSH] Calling ssh_bind_listen() on port %d...\n", port);
    if (ssh_bind_listen(sshbind) < 0) {
        Serial.printf("[SSH] Listen failed: %s\n", ssh_get_error(sshbind));
        return;
    }
    Serial.printf("[SSH] Listening OK on port %d (0.0.0.0 / ::)\n", port);
    Serial.println("[SSH] If client still gets 'connection refused', a raw TCP probe will be attempted next iteration.");
    // TODO: Next step if still refused: spin up a WiFiServer on another port to ensure inbound connectivity
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

    // Shell request
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
        if (ssh_message_type(m) == SSH_REQUEST_CHANNEL &&
            ssh_message_subtype(m) == SSH_CHANNEL_REQUEST_SHELL) {
            ssh_message_channel_request_reply_success(m);
            ssh_message_free(m);
            Serial.println("Shell started (echo mode)");
            shellReady = true;
            break;
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
