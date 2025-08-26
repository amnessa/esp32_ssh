#include "SshServer.h"
#include <Arduino.h>

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

    ssh_callbacks_struct cb;
    memset(&cb, 0, sizeof(cb));
    ssh_callbacks_init(&cb);
    cb.auth_password_function = auth_password;
    ssh_set_callbacks(session, &cb);

    if (ssh_handle_key_exchange(session)) {
        return;
    }

    ssh_set_auth_methods(session, SSH_AUTH_METHOD_PASSWORD);
    ssh_event event;
    while ((event = ssh_event_poll(NULL, 0)) != NULL) {
        if (ssh_event_get_type(event) == SSH_EVENT_SESSION_AUTHENTICATE) {
            if (ssh_event_get_return_code(event) == SSH_AUTH_SUCCESS) {
                break;
            }
        }
    }

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
