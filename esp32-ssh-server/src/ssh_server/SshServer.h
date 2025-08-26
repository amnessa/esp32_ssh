#ifndef SSH_SERVER_H
#define SSH_SERVER_H

#include <libssh.h>

class SshServer {
public:
    SshServer(const char* host_key);
    void begin();
    void handleClient();

private:
    ssh_session session;
    ssh_bind sshbind;
    const char* host_key;
    static int auth_password(ssh_session session, const char *user, const char *password, void *userdata);
};

#endif // SSH_SERVER_H
