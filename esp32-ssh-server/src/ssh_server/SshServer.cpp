#include "SshServer.h"
#include <Arduino.h>
#include <libssh/libssh.h>
#include <libssh/server.h>
// No SPIFFS/host-key storage; ephemeral in-memory host key

#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

static void ssh_write_str(ssh_channel ch, const char* s){
    if (!ch) return;
    if (s && *s) ssh_channel_write(ch, s, strlen(s));
}

static void ssh_write_line(ssh_channel ch, const char* s){
    if (s) ssh_channel_write(ch, s, strlen(s));
    ssh_channel_write(ch, "\r\n", 2);
}

static int ssh_read_nonblocking(ssh_channel ch, uint8_t* buf, int maxlen){
    if (!ssh_channel_is_open(ch) || ssh_channel_is_eof(ch)) return -1;
    int avail = ssh_channel_poll(ch, 0);
    if (avail <= 0) return 0;
    if (avail > maxlen) avail = maxlen;
    int r = ssh_channel_read_nonblocking(ch, buf, avail, 0);
    return r > 0 ? r : 0;
}

// Read lines without blocking; returns true when a full line is available in outLine
static bool ssh_read_line(ssh_channel ch, String &inBuf, String &outLine){
    uint8_t tmp[64];
    int r = ssh_read_nonblocking(ch, tmp, sizeof(tmp));
    if (r <= 0) return false;
    for (int i=0;i<r;i++){
        char c = (char)tmp[i];
        if (c == '\r') continue; // normalize CRLF
        if (c == '\n'){
            outLine = inBuf;
            inBuf = "";
            return true;
        }
        if (c == 0x7f || c == 0x08){ // backspace
            if (inBuf.length() > 0){ inBuf.remove(inBuf.length()-1); }
            continue;
        }
        if (inBuf.length() < 256) inBuf += c;
    }
    return false;
}

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

    // Interactive app menu
    auto runEchoMode = [&](ssh_channel ch){
        ssh_write_line(ch, "Echo mode: type text and press Enter. Press 'q' then Enter to return to menu.");
        ssh_write_str(ch, "> ");
        String buf; String line;
        while (ssh_channel_is_open(ch) && !ssh_channel_is_eof(ch)){
            if (ssh_read_line(ch, buf, line)){
                if (line == "q"){
                    ssh_write_line(ch, "(leaving echo mode)");
                    return;
                }
                ssh_write_line(ch, line.c_str());
                ssh_write_str(ch, "> ");
            }
            delay(10);
        }
    };

    auto runBlinkApp = [&](ssh_channel ch){
        pinMode(LED_BUILTIN, OUTPUT);
        bool ledOn = false;
        float freq = 2.0f; // Hz
        const float minF = 0.1f, maxF = 20.0f;
        unsigned long lastToggle = millis();
        unsigned long halfPeriodMs = (unsigned long)(500.0f / freq);
        auto recompute = [&](){ halfPeriodMs = (unsigned long)(500.0f / freq); if (halfPeriodMs < 1) halfPeriodMs = 1; };
        ssh_write_line(ch, "Blinking LED app: default 2.0 Hz.");
        ssh_write_line(ch, "Commands: '+' faster, '-' slower, a number sets Hz (e.g. 5), 'q' to return.");
        ssh_write_str(ch, "> ");
        String buf; String line;
        while (ssh_channel_is_open(ch) && !ssh_channel_is_eof(ch)){
            // Toggle LED
            unsigned long now = millis();
            if (now - lastToggle >= halfPeriodMs){
                ledOn = !ledOn;
                digitalWrite(LED_BUILTIN, ledOn ? HIGH : LOW);
                lastToggle = now;
            }
            // Input processing
            if (ssh_read_line(ch, buf, line)){
                if (line == "q"){
                    ssh_write_line(ch, "(stopping blink, returning to menu)");
                    digitalWrite(LED_BUILTIN, LOW);
                    return;
                }
                if (line == "+") { freq += 0.5f; if (freq > maxF) freq = maxF; recompute(); }
                else if (line == "-") { freq -= 0.5f; if (freq < minF) freq = minF; recompute(); }
                else {
                    char *end=nullptr; double v = strtod(line.c_str(), &end);
                    if (end && *end == '\0') { freq = (float)v; if (freq < minF) freq = minF; if (freq > maxF) freq = maxF; recompute(); }
                }
                char msg[48]; snprintf(msg, sizeof(msg), "freq = %.2f Hz", (double)freq);
                ssh_write_line(ch, msg);
                ssh_write_str(ch, "> ");
            }
            delay(5);
        }
        digitalWrite(LED_BUILTIN, LOW);
    };

    auto runMenu = [&](ssh_channel ch){
        ssh_write_line(ch, "=== ESP32 Apps ===");
        ssh_write_line(ch, "1) echo_mode");
        ssh_write_line(ch, "2) blinking_led");
        ssh_write_line(ch, "Type number and Enter to run, or 'quit' to disconnect.");
        ssh_write_str(ch, "> ");
        String buf; String line;
        while (ssh_channel_is_open(ch) && !ssh_channel_is_eof(ch)){
            if (ssh_read_line(ch, buf, line)){
                if (line == "quit"){
                    ssh_write_line(ch, "Goodbye.");
                    return false; // close session
                }
                if (line == "1"){
                    runEchoMode(ch);
                    // show menu again after return
                    ssh_write_line(ch, "=== ESP32 Apps ===");
                    ssh_write_line(ch, "1) echo_mode");
                    ssh_write_line(ch, "2) blinking_led");
                    ssh_write_line(ch, "Type number and Enter to run, or 'quit' to disconnect.");
                    ssh_write_str(ch, "> ");
                } else if (line == "2"){
                    runBlinkApp(ch);
                    ssh_write_line(ch, "=== ESP32 Apps ===");
                    ssh_write_line(ch, "1) echo_mode");
                    ssh_write_line(ch, "2) blinking_led");
                    ssh_write_line(ch, "Type number and Enter to run, or 'quit' to disconnect.");
                    ssh_write_str(ch, "> ");
                } else {
                    ssh_write_line(ch, "Unknown option. Choose 1, 2 or 'quit'.");
                    ssh_write_str(ch, "> ");
                }
            }
            delay(10);
        }
        return false;
    };

    // Run the menu; closing returns to caller and ends session
    (void)runMenu(ch);

    if (ch) ssh_channel_free(ch);
    ssh_disconnect(sess);
    ssh_free(sess);
    Serial.println("Session closed");
}
