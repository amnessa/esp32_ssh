#include "SshServer.h"
#include <Arduino.h>
#include <libssh/callbacks.h>
#include <libssh/server.h>

SshServer::SshServer(const char* host_key) {
    this->host_key = host_key;
    ssh_init();
}

void SshServer::begin() {
    session = ssh_new();
    sshbind = ssh_bind_new();

    ssh_bind_options_set(sshbind, SSH_BIND_OPTIONS_RSAKEY, host_key);
    ssh_bind_listen(sshbind);

    Serial.println("SSH Server started");
}

int SshServer::auth_password(ssh_session session, const char *user, const char *password, void *userdata) {
    if (strcmp(user, "cago") == 0 && strcmp(password, "cago1231") == 0) {
        return SSH_AUTH_SUCCESS;
    }
    return SSH_AUTH_DENIED;
}

void SshServer::handleClient() {
    if (ssh_bind_accept(sshbind, session) == SSH_ERROR) {
        return;
    }

    if (ssh_handle_key_exchange(session)) {
        return;
    }

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
        } else {
            ssh_message_reply_default(message);
        }
        ssh_message_free(message);
    } while (true);


    ssh_channel channel = ssh_channel_new(session);
    while (channel == NULL) {
        delay(100);
        channel = ssh_channel_new(session);
    }

    while (ssh_channel_open_session(channel) == SSH_ERROR) {
        delay(100);
    }

    char buffer[256];
    int nbytes;
    while (true) {
        nbytes = ssh_channel_read(channel, buffer, sizeof(buffer), 0);
        if (nbytes > 0) {
            ssh_channel_write(channel, buffer, nbytes);
        } else if (nbytes < 0) {
            break;
        }
    }

    ssh_channel_close(channel);
    ssh_channel_free(channel);
    ssh_disconnect(session);
}
