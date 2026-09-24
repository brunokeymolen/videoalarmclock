/*
 * SPDX-FileCopyrightText: 2026 Bruno Keymolen
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of Video Alarm Clock.
 *
 * Video Alarm Clock is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
/*
 * nn20clock_ftp.c - the FTP server (design 12).
 *
 * One acceptor task, one task per session. See the header for what is
 * implemented, what is not, and why this is the one component in the
 * project that owns tasks rather than running on a worker.
 *
 * Paths are the other half of what this file does: every one a client
 * sends goes through resolve_path() below and reaches the callbacks as
 * a canonical relative path. See the header for why that function is
 * the security boundary and nothing else here is.
 *
 * The shape of FTP, briefly, because it is unusual: the client keeps a
 * control connection open and sends text commands; every transfer opens
 * a SECOND connection. In passive mode - the only mode here - the
 * server opens a listening socket, tells the client which port in a
 * "227 Entering Passive Mode (h,h,h,h,p,p)" reply, and the client
 * connects to it. That data socket carries exactly one transfer and is
 * then closed, which is how the client knows the transfer ended.
 */
#include "nn20clock_ftp.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

static const char *TAG = "NN20CLOCK_FTP";

/* One command line. RFC 959 puts no hard limit on this, but a name is
 * capped at 255 and the rest of a line is a verb and a space. */
#define COMMAND_MAX 512

/* Transfer chunk. Larger moves fewer times through the filesystem;
 * 4 KB is a compromise against the stack and heap this device has. */
#define TRANSFER_CHUNK 4096

#define ACCEPTOR_STACK 4096
#define SESSION_STACK  6144
#define FTP_TASK_PRIORITY 4

/* A client that has gone away without closing would otherwise hold a
 * session slot forever. */
#define SESSION_IDLE_TIMEOUT_S 300

typedef struct {
    NN20ClockFtp *server;
    int control;              /* the command connection */
    int data_listener;        /* passive socket, waiting for the client */
    int data;                 /* the accepted data connection */
    char address[INET_ADDRSTRLEN];
    /*
     * Where this client is, as a canonical relative path from the
     * served root; "" is the root itself. Owned by this session's task
     * and read by nothing else.
     */
    char directory[NN20CLOCK_FTP_PATH_MAX];
    /*
     * The source of a rename, between RNFR and RNTO. Empty when there
     * is none - and cleared by every command except RNTO, so a sequence
     * interrupted by anything at all fails rather than renaming
     * something the client has stopped talking about.
     */
    char rename_from[NN20CLOCK_FTP_PATH_MAX];
    atomic_bool in_use;
    /* NN20ClockFtpActivity. Written by this session's own task, read by
     * whoever asks - one atomic value, so a reader sees one operation
     * or the next and never something in between. */
    atomic_int activity;
    TaskHandle_t task;
} Session;

struct NN20ClockFtp {
    NN20ClockFtpOps ops;
    uint16_t port;

    int listener;
    TaskHandle_t acceptor;

    atomic_bool running;
    atomic_bool stopping;
    atomic_uint uploads;
    atomic_uint downloads;
    atomic_uint mutations;

    Session sessions[NN20CLOCK_FTP_MAX_SESSIONS];
};

/* ---------------------------------------------------------- replies -- */

static void reply(Session *session, const char *line)
{
    /* FTP lines end CRLF, and a client that does not see one keeps
     * waiting. */
    char buffer[COMMAND_MAX];
    const int length = snprintf(buffer, sizeof(buffer), "%s\r\n", line);
    if (length > 0) {
        (void)send(session->control, buffer, (size_t)length, 0);
    }
}

static void replyf(Session *session, const char *format, ...)
{
    char line[COMMAND_MAX];
    va_list args;
    va_start(args, format);
    (void)vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    reply(session, line);
}

/* ------------------------------------------------------------- data -- */

static void data_close(Session *session)
{
    if (session->data >= 0) {
        close(session->data);
        session->data = -1;
    }
    if (session->data_listener >= 0) {
        close(session->data_listener);
        session->data_listener = -1;
    }
}

/*
 * Open the passive socket and tell the client where it is.
 *
 * Port 0 asks the stack for any free port, which is then read back -
 * picking a port ourselves would collide with whatever else is
 * listening.
 */
/* Announce what this session is doing. Always paired with a return to
 * IDLE, so a finished transfer does not leave the screen claiming a
 * client is still uploading. */
static void set_activity(Session *session, NN20ClockFtpActivity activity)
{
    atomic_store_explicit(&session->activity, (int)activity,
                          memory_order_relaxed);
}

/* Something served has changed. See nn20clock_ftp_mutations(). */
static void note_mutation(Session *session)
{
    atomic_fetch_add_explicit(&session->server->mutations, 1u,
                              memory_order_relaxed);
}

/*
 * What the client typed, resolved against where it is, into `out`.
 *
 * The security boundary of this server, and one function wide on
 * purpose: nn20clock_media_path_resolve() is where "..", a leading
 * slash and a trailing slash are interpreted, and nothing else here
 * touches the text at all. A refusal is answered 550 and the command
 * ends - there is no repaired path to fall back to, by design.
 */
static bool resolve_path(Session *session, const char *input, char *out,
                         size_t size)
{
    if (nn20clock_media_path_resolve(session->directory, input, out,
                                     size) != ESP_OK) {
        reply(session, "550 No such file or directory.");
        return false;
    }
    return true;
}

static esp_err_t enter_passive(Session *session, bool extended)
{
    data_close(session);

    session->data_listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (session->data_listener < 0) {
        reply(session, "425 Cannot open data connection.");
        return ESP_FAIL;
    }

    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = 0,
    };
    if (bind(session->data_listener, (struct sockaddr *)&address,
             sizeof(address)) != 0 ||
        listen(session->data_listener, 1) != 0) {
        data_close(session);
        reply(session, "425 Cannot open data connection.");
        return ESP_FAIL;
    }

    socklen_t length = sizeof(address);
    if (getsockname(session->data_listener, (struct sockaddr *)&address,
                    &length) != 0) {
        data_close(session);
        reply(session, "425 Cannot open data connection.");
        return ESP_FAIL;
    }
    const uint16_t port = ntohs(address.sin_port);

    if (extended) {
        /* EPSV carries only the port; the client reuses the control
         * connection's address. */
        replyf(session, "229 Entering Extended Passive Mode (|||%u|)",
               (unsigned)port);
        return ESP_OK;
    }

    /*
     * PASV needs the address the client reached us on - not INADDR_ANY,
     * and not a guess. Taking it from the control socket is what makes
     * this work on any interface.
     */
    struct sockaddr_in local = {0};
    length = sizeof(local);
    if (getsockname(session->control, (struct sockaddr *)&local, &length)
        != 0) {
        data_close(session);
        reply(session, "425 Cannot open data connection.");
        return ESP_FAIL;
    }

    const uint32_t host = ntohl(local.sin_addr.s_addr);
    replyf(session, "227 Entering Passive Mode (%u,%u,%u,%u,%u,%u)",
           (unsigned)((host >> 24) & 0xFF), (unsigned)((host >> 16) & 0xFF),
           (unsigned)((host >> 8) & 0xFF), (unsigned)(host & 0xFF),
           (unsigned)(port >> 8), (unsigned)(port & 0xFF));
    return ESP_OK;
}

/* Wait for the client to connect to the passive socket. */
static int data_accept(Session *session)
{
    if (session->data_listener < 0) {
        reply(session, "425 Use PASV first.");
        return -1;
    }

    struct sockaddr_in peer;
    socklen_t length = sizeof(peer);
    session->data = accept(session->data_listener,
                           (struct sockaddr *)&peer, &length);
    if (session->data < 0) {
        reply(session, "425 Cannot open data connection.");
        data_close(session);
        return -1;
    }
    return session->data;
}

/* --------------------------------------------------------- commands -- */

/*
 * "-rw-rw-rw- 1 owner group SIZE Jan 01 00:00 name" - the Unix ls
 * format every client parses, with a leading "d" for a directory. The
 * date is fixed: FAT timestamps are not worth the code here, and no
 * client depends on them for a listing.
 *
 * `path` is what the client asked for, already canonical. LIST takes
 * either a directory, whose contents are sent, or a single file, whose
 * one line is - which is what `ls` does and what several clients use to
 * probe for a file's existence.
 */
static void send_listing(Session *session, const char *path, bool names_only)
{
    NN20ClockFtp *server = session->server;

    bool is_dir = false;
    uint32_t size_bytes = 0u;
    if (server->ops.stat(server->ops.ctx, path, &is_dir, &size_bytes)
        != ESP_OK) {
        reply(session, "550 No such file or directory.");
        return;
    }

    NN20ClockFtpEntry *entries = NULL;
    size_t count = 0;
    if (is_dir) {
        entries = calloc(64, sizeof(*entries));
        if (entries == NULL) {
            reply(session, "451 Out of memory.");
            return;
        }
        set_activity(session, NN20CLOCK_FTP_ACTIVITY_LISTING);
        count = server->ops.list(server->ops.ctx, path, entries, 64);
    }

    reply(session, "150 Here comes the directory listing.");
    if (data_accept(session) < 0) {
        free(entries);
        set_activity(session, NN20CLOCK_FTP_ACTIVITY_IDLE);
        return;
    }

    if (!is_dir) {
        /* One file. The name a client expects back is the one it asked
         * about, which is the last component of the path. */
        const char *const name = nn20clock_media_path_name(path);
        char line[COMMAND_MAX];
        const int length =
            names_only
                ? snprintf(line, sizeof(line), "%s\r\n", name)
                : snprintf(line, sizeof(line),
                           "-rw-rw-rw- 1 nn20 nn20 %10u Jan 01 00:00 %s\r\n",
                           (unsigned)size_bytes, name);
        if (length > 0) {
            (void)send(session->data, line, (size_t)length, 0);
        }
    }

    for (size_t i = 0; i < count; i++) {
        char line[COMMAND_MAX];
        int length;
        if (names_only) {
            length = snprintf(line, sizeof(line), "%s\r\n", entries[i].name);
        } else {
            /* The leading character is the whole reason NN20ClockFtpEntry
             * carries is_dir: a directory reported as a file is one a
             * client will try to download. */
            length = snprintf(line, sizeof(line),
                              "%crw-rw-rw- 1 nn20 nn20 %10u Jan 01 00:00 %s\r\n",
                              entries[i].is_dir ? 'd' : '-',
                              (unsigned)entries[i].size_bytes,
                              entries[i].name);
        }
        if (length > 0) {
            (void)send(session->data, line, (size_t)length, 0);
        }
    }

    free(entries);
    data_close(session);   /* closing is how the client knows it ended */
    set_activity(session, NN20CLOCK_FTP_ACTIVITY_IDLE);
    reply(session, "226 Directory send OK.");
}

static void send_file(Session *session, const char *name)
{
    NN20ClockFtp *server = session->server;

    void *handle = server->ops.open_read(server->ops.ctx, name);
    if (handle == NULL) {
        reply(session, "550 File not found.");
        return;
    }

    reply(session, "150 Opening data connection.");
    if (data_accept(session) < 0) {
        server->ops.close(server->ops.ctx, handle);
        return;
    }

    uint8_t *buffer = malloc(TRANSFER_CHUNK);
    if (buffer == NULL) {
        server->ops.close(server->ops.ctx, handle);
        data_close(session);
        reply(session, "451 Out of memory.");
        return;
    }

    bool ok = true;
    for (;;) {
        const size_t got = server->ops.read(server->ops.ctx, handle, buffer,
                                            TRANSFER_CHUNK);
        if (got == 0) {
            break;   /* end of file */
        }

        size_t sent = 0;
        while (sent < got) {
            const int written = send(session->data, buffer + sent,
                                     got - sent, 0);
            if (written <= 0) {
                ok = false;   /* client went away mid-transfer */
                break;
            }
            sent += (size_t)written;
        }
        if (!ok) {
            break;
        }
    }

    free(buffer);
    server->ops.close(server->ops.ctx, handle);
    data_close(session);

    if (ok) {
        atomic_fetch_add_explicit(&server->downloads, 1u,
                                  memory_order_relaxed);
        reply(session, "226 Transfer complete.");
        ESP_LOGI(TAG, "sent %s", name);
    } else {
        reply(session, "426 Transfer aborted.");
    }
}

static void receive_file(Session *session, const char *name)
{
    NN20ClockFtp *server = session->server;

    void *handle = server->ops.open_write(server->ops.ctx, name);
    if (handle == NULL) {
        reply(session, "550 Cannot create file.");
        return;
    }

    reply(session, "150 Ready to receive.");
    if (data_accept(session) < 0) {
        server->ops.close(server->ops.ctx, handle);
        return;
    }

    uint8_t *buffer = malloc(TRANSFER_CHUNK);
    if (buffer == NULL) {
        server->ops.close(server->ops.ctx, handle);
        data_close(session);
        reply(session, "451 Out of memory.");
        return;
    }

    bool ok = true;
    for (;;) {
        const int got = recv(session->data, buffer, TRANSFER_CHUNK, 0);
        if (got == 0) {
            break;   /* the client closed: the transfer is complete */
        }
        if (got < 0) {
            ok = false;
            break;
        }
        if (server->ops.write(server->ops.ctx, handle, buffer,
                              (size_t)got) != (size_t)got) {
            /* A short write means the card is full or failing. Better to
             * fail the transfer than leave a truncated clip that looks
             * fine in a listing. */
            ok = false;
            break;
        }
    }

    free(buffer);
    server->ops.close(server->ops.ctx, handle);
    data_close(session);

    if (ok) {
        atomic_fetch_add_explicit(&server->uploads, 1u, memory_order_relaxed);
        note_mutation(session);
        reply(session, "226 Transfer complete.");
        ESP_LOGI(TAG, "received %s", name);
    } else {
        /* Remove the partial file: a half-uploaded clip that plays for
         * two seconds at 7am is worse than no clip. */
        (void)server->ops.remove_file(server->ops.ctx, name);
        reply(session, "451 Transfer failed.");
        ESP_LOGW(TAG, "upload of %s failed; partial file removed", name);
    }
}

/*
 * PWD's reply, and the one place the session's relative directory is
 * turned back into the absolute path a client expects: "" is "/",
 * "morning" is "/morning".
 */
static void reply_current_directory(Session *session)
{
    replyf(session, "257 \"/%s\" is the current directory.",
           session->directory);
}

/* Splits "VERB argument" and dispatches. Returns false to end the
 * session. */
static bool handle_command(Session *session, char *line)
{
    NN20ClockFtp *server = session->server;

    char *argument = strchr(line, ' ');
    if (argument != NULL) {
        *argument = '\0';
        argument++;
        while (*argument == ' ') {
            argument++;
        }
    } else {
        argument = "";
    }

    /* Verbs are case-insensitive per RFC 959. */
    for (char *c = line; *c != '\0'; c++) {
        if (*c >= 'a' && *c <= 'z') {
            *c = (char)(*c - 'a' + 'A');
        }
    }

    /*
     * A pending RNFR survives exactly one command: the RNTO that
     * follows it. Cleared here, before the dispatch, so every path out
     * of every branch below leaves it cleared without having to
     * remember to - the simple rule RFC 959 allows and the one that
     * cannot be got wrong by adding a command later.
     */
    char rename_from[NN20CLOCK_FTP_PATH_MAX];
    (void)snprintf(rename_from, sizeof(rename_from), "%s",
                   session->rename_from);
    if (strcmp(line, "RNTO") != 0) {
        session->rename_from[0] = '\0';
    }

    /* Where a path argument lands. One buffer, because no command here
     * needs two at once except RNTO, which has the copy above. */
    char path[NN20CLOCK_FTP_PATH_MAX];

    if (strcmp(line, "USER") == 0) {
        /* No authentication - see the header. Accepting any user is
         * honest about that; asking for a password we do not check
         * would be worse. */
        reply(session, "230 Login successful.");
    } else if (strcmp(line, "PASS") == 0) {
        reply(session, "230 Login successful.");
    } else if (strcmp(line, "SYST") == 0) {
        /* Clients parse this to decide how to read listings. */
        reply(session, "215 UNIX Type: L8");
    } else if (strcmp(line, "FEAT") == 0) {
        reply(session, "211-Features:");
        reply(session, " SIZE");
        reply(session, " UTF8");
        reply(session, " EPSV");
        /* RFC 2389 says a SITE line lists the SITE verbs on offer, so a
         * client asking what this server does gets a real answer rather
         * than having to read the source. */
        reply(session, " SITE HIDE UNHIDE");
        reply(session, "211 End");
    } else if (strcmp(line, "OPTS") == 0) {
        reply(session, "200 OK.");
    } else if (strcmp(line, "TYPE") == 0) {
        /* Everything here is binary; announcing otherwise would corrupt
         * media on clients that translate line endings. */
        reply(session, "200 Type set to I.");
    } else if (strcmp(line, "PWD") == 0 || strcmp(line, "XPWD") == 0) {
        reply_current_directory(session);
    } else if (strcmp(line, "CWD") == 0 || strcmp(line, "XCWD") == 0 ||
               strcmp(line, "CDUP") == 0 || strcmp(line, "XCUP") == 0) {
        /* CDUP is CWD ".." and is implemented as exactly that rather
         * than as its own arithmetic - one place decides what happens
         * at the root, and it is the resolver. */
        const bool up = (strcmp(line, "CDUP") == 0 ||
                         strcmp(line, "XCUP") == 0);
        const char *const where = up ? ".." : argument;
        if (resolve_path(session, where, path, sizeof(path))) {
            bool is_dir = false;
            uint32_t size_bytes = 0u;
            if (server->ops.stat(server->ops.ctx, path, &is_dir,
                                 &size_bytes) != ESP_OK || !is_dir) {
                reply(session, "550 No such directory.");
            } else {
                (void)snprintf(session->directory,
                               sizeof(session->directory), "%s", path);
                reply(session, "250 Directory changed.");
            }
        }
    } else if (strcmp(line, "PASV") == 0) {
        (void)enter_passive(session, false);
    } else if (strcmp(line, "EPSV") == 0) {
        (void)enter_passive(session, true);
    } else if (strcmp(line, "PORT") == 0 || strcmp(line, "EPRT") == 0) {
        /* Active mode would need the server to connect back to the
         * client. Refused clearly rather than left to time out. */
        reply(session, "502 Active mode is not supported; use passive.");
    } else if (strcmp(line, "LIST") == 0 || strcmp(line, "NLST") == 0) {
        /*
         * Clients send options here - "LIST -la" - and they are not
         * paths. Anything starting with a dash is dropped, which leaves
         * the current directory, and that is what those clients meant.
         */
        const char *const where = (argument[0] == '-') ? "" : argument;
        if (resolve_path(session, where, path, sizeof(path))) {
            send_listing(session, path, line[0] == 'N');
        }
    } else if (strcmp(line, "RETR") == 0) {
        if (resolve_path(session, argument, path, sizeof(path))) {
            set_activity(session, NN20CLOCK_FTP_ACTIVITY_DOWNLOADING);
            send_file(session, path);
            set_activity(session, NN20CLOCK_FTP_ACTIVITY_IDLE);
        }
    } else if (strcmp(line, "STOR") == 0) {
        if (resolve_path(session, argument, path, sizeof(path))) {
            set_activity(session, NN20CLOCK_FTP_ACTIVITY_UPLOADING);
            receive_file(session, path);
            set_activity(session, NN20CLOCK_FTP_ACTIVITY_IDLE);
        }
    } else if (strcmp(line, "DELE") == 0) {
        if (resolve_path(session, argument, path, sizeof(path))) {
            set_activity(session, NN20CLOCK_FTP_ACTIVITY_DELETING);
            const esp_err_t removed = server->ops.remove_file(server->ops.ctx,
                                                              path);
            set_activity(session, NN20CLOCK_FTP_ACTIVITY_IDLE);
            if (removed == ESP_OK) {
                note_mutation(session);
                reply(session, "250 File deleted.");
            } else {
                reply(session, "550 Cannot delete file.");
            }
        }
    } else if (strcmp(line, "MKD") == 0 || strcmp(line, "XMKD") == 0) {
        if (resolve_path(session, argument, path, sizeof(path))) {
            set_activity(session, NN20CLOCK_FTP_ACTIVITY_CREATING_DIR);
            const esp_err_t made = server->ops.make_dir(server->ops.ctx,
                                                        path);
            set_activity(session, NN20CLOCK_FTP_ACTIVITY_IDLE);
            if (made == ESP_OK) {
                note_mutation(session);
                /* 257 carries the name back, which is what a client
                 * shows in its listing before it refreshes. */
                replyf(session, "257 \"/%s\" created.", path);
            } else {
                reply(session, "550 Cannot create directory.");
            }
        }
    } else if (strcmp(line, "RMD") == 0 || strcmp(line, "XRMD") == 0) {
        if (resolve_path(session, argument, path, sizeof(path))) {
            set_activity(session, NN20CLOCK_FTP_ACTIVITY_DELETING);
            const esp_err_t removed = server->ops.remove_dir(server->ops.ctx,
                                                             path);
            set_activity(session, NN20CLOCK_FTP_ACTIVITY_IDLE);
            if (removed == ESP_OK) {
                note_mutation(session);
                reply(session, "250 Directory deleted.");
            } else {
                /* Almost always "not empty" - see the header. Said
                 * plainly, because a client showing "550 failed" for a
                 * directory the user can see files in is a mystery. */
                reply(session, "550 Cannot delete directory; it must be "
                               "empty.");
            }
        }
    } else if (strcmp(line, "RNFR") == 0) {
        if (resolve_path(session, argument, path, sizeof(path))) {
            bool is_dir = false;
            uint32_t size_bytes = 0u;
            if (path[0] == '\0' ||
                server->ops.stat(server->ops.ctx, path, &is_dir,
                                 &size_bytes) != ESP_OK) {
                /* Refused now rather than at RNTO: a client that cannot
                 * rename a missing file should hear so while it still
                 * knows which file it asked about. */
                reply(session, "550 No such file or directory.");
            } else {
                (void)snprintf(session->rename_from,
                               sizeof(session->rename_from), "%s", path);
                reply(session, "350 Ready for RNTO.");
            }
        }
    } else if (strcmp(line, "RNTO") == 0) {
        /* The copy taken at the top of this function: the session's own
         * field was left alone only for this branch. */
        session->rename_from[0] = '\0';
        if (rename_from[0] == '\0') {
            reply(session, "503 Bad sequence of commands; send RNFR first.");
        } else if (resolve_path(session, argument, path, sizeof(path))) {
            set_activity(session, NN20CLOCK_FTP_ACTIVITY_RENAMING);
            const esp_err_t renamed =
                (path[0] == '\0')
                    ? ESP_ERR_INVALID_ARG   /* the served root itself */
                    : server->ops.rename(server->ops.ctx, rename_from, path);
            set_activity(session, NN20CLOCK_FTP_ACTIVITY_IDLE);
            if (renamed == ESP_OK) {
                note_mutation(session);
                reply(session, "250 Rename successful.");
            } else {
                reply(session, "553 Cannot rename; the target may already "
                               "exist.");
            }
        }
    } else if (strcmp(line, "SIZE") == 0) {
        if (resolve_path(session, argument, path, sizeof(path))) {
            bool is_dir = false;
            uint32_t size = 0u;
            if (server->ops.stat(server->ops.ctx, path, &is_dir, &size)
                    == ESP_OK && !is_dir) {
                replyf(session, "213 %u", (unsigned)size);
            } else {
                /* Directories included: SIZE is defined for files, and
                 * a number for a directory is a number a client will
                 * use as a byte count. */
                reply(session, "550 File not found.");
            }
        }
    } else if (strcmp(line, "SITE") == 0) {
        /*
         * The one place FTP allows a server its own commands. Here that
         * is HIDE and UNHIDE, and nothing else - see the header.
         *
         * The verb and its argument are split the same way the command
         * line already was, which keeps "SITE HIDE a b.avi" working:
         * everything after the verb is the path, spaces included.
         */
        char *target = strchr(argument, ' ');
        if (target != NULL) {
            *target = '\0';
            target++;
            while (*target == ' ') {
                target++;
            }
        } else {
            target = "";
        }
        for (char *c = argument; *c != '\0'; c++) {
            if (*c >= 'a' && *c <= 'z') {
                *c = (char)(*c - 'a' + 'A');
            }
        }

        const bool hide = (strcmp(argument, "HIDE") == 0);
        const bool unhide = (strcmp(argument, "UNHIDE") == 0);
        if (!hide && !unhide) {
            reply(session, "504 SITE takes HIDE or UNHIDE.");
        } else if (server->ops.set_hidden == NULL) {
            /* An owner whose storage has no such idea. Said plainly
             * rather than accepted and quietly dropped. */
            reply(session, "502 SITE HIDE is not supported here.");
        } else if (target[0] == '\0') {
            /* Distinct from "that path cannot be hidden": a missing
             * argument is the client's mistake, and 501 is where a
             * client looks for it. */
            reply(session, "501 SITE HIDE needs a path.");
        } else if (resolve_path(session, target, path, sizeof(path))) {
            set_activity(session, NN20CLOCK_FTP_ACTIVITY_HIDING);
            const esp_err_t changed =
                (path[0] == '\0')
                    ? ESP_ERR_INVALID_ARG   /* the served root itself */
                    : server->ops.set_hidden(server->ops.ctx, path, hide);
            set_activity(session, NN20CLOCK_FTP_ACTIVITY_IDLE);
            if (changed == ESP_OK) {
                note_mutation(session);
                replyf(session, "200 /%s is now %s.", path,
                       hide ? "hidden" : "visible");
            } else {
                reply(session, "550 Cannot change that.");
            }
        }
    } else if (strcmp(line, "NOOP") == 0) {
        reply(session, "200 OK.");
    } else if (strcmp(line, "QUIT") == 0) {
        reply(session, "221 Goodbye.");
        return false;
    } else {
        replyf(session, "502 %s not implemented.", line);
    }

    return true;
}

/* --------------------------------------------------------- sessions -- */

static void session_task(void *argument)
{
    Session *session = argument;
    NN20ClockFtp *server = session->server;

    ESP_LOGI(TAG, "client %s connected", session->address);
    reply(session, "220 NN20Clock media server");

    /* A client that vanishes without closing would hold this slot for
     * good; the timeout is what releases it. */
    const struct timeval timeout = { .tv_sec = SESSION_IDLE_TIMEOUT_S };
    (void)setsockopt(session->control, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                     sizeof(timeout));

    char line[COMMAND_MAX];
    size_t used = 0;
    bool keep_going = true;

    while (keep_going &&
           !atomic_load_explicit(&server->stopping, memory_order_acquire)) {
        const int got = recv(session->control, line + used,
                             sizeof(line) - used - 1, 0);
        if (got <= 0) {
            break;   /* closed, or idle for too long */
        }
        used += (size_t)got;
        line[used] = '\0';

        /* Commands are CRLF-delimited and a read can split or merge
         * them, so complete lines are taken out one at a time and any
         * remainder is kept for the next read. */
        char *start = line;
        for (;;) {
            char *end = strstr(start, "\r\n");
            if (end == NULL) {
                break;
            }
            *end = '\0';

            if (start[0] != '\0') {
                keep_going = handle_command(session, start);
                if (!keep_going) {
                    break;
                }
            }
            start = end + 2;
        }

        const size_t remaining = used - (size_t)(start - line);
        memmove(line, start, remaining);
        used = remaining;
        if (used >= sizeof(line) - 1) {
            /* A line longer than the buffer is not a command this
             * server understands; drop it rather than grow. */
            reply(session, "500 Line too long.");
            used = 0;
        }
    }

    ESP_LOGI(TAG, "client %s disconnected", session->address);

    data_close(session);
    close(session->control);
    session->control = -1;
    session->task = NULL;
    atomic_store_explicit(&session->in_use, false, memory_order_release);

    vTaskDelete(NULL);
}

static Session *claim_session(NN20ClockFtp *pthis)
{
    for (size_t i = 0; i < NN20CLOCK_FTP_MAX_SESSIONS; i++) {
        bool expected = false;
        if (atomic_compare_exchange_strong(&pthis->sessions[i].in_use,
                                           &expected, true)) {
            /* A slot is reused, so it can still carry whatever the
             * previous client was doing when it disconnected. */
            atomic_store_explicit(&pthis->sessions[i].activity,
                                  (int)NN20CLOCK_FTP_ACTIVITY_IDLE,
                                  memory_order_relaxed);
            /* And whatever directory it was in, and whatever rename it
             * had half-finished. A new client starts at the root. */
            pthis->sessions[i].directory[0] = '\0';
            pthis->sessions[i].rename_from[0] = '\0';
            return &pthis->sessions[i];
        }
    }
    return NULL;
}

static void acceptor_task(void *argument)
{
    NN20ClockFtp *pthis = argument;

    while (!atomic_load_explicit(&pthis->stopping, memory_order_acquire)) {
        struct sockaddr_in peer;
        socklen_t length = sizeof(peer);
        const int client = accept(pthis->listener, (struct sockaddr *)&peer,
                                  &length);
        if (client < 0) {
            if (atomic_load_explicit(&pthis->stopping,
                                     memory_order_acquire)) {
                break;   /* the listener was closed to wake us */
            }
            continue;
        }

        Session *session = claim_session(pthis);
        if (session == NULL) {
            /* Every slot busy. Say so rather than leave the client
             * waiting on a connection nobody will read. */
            const char *busy = "421 Too many connections.\r\n";
            (void)send(client, busy, strlen(busy), 0);
            close(client);
            continue;
        }

        session->server = pthis;
        session->control = client;
        session->data = -1;
        session->data_listener = -1;
        inet_ntoa_r(peer.sin_addr, session->address,
                    sizeof(session->address));

        if (xTaskCreate(session_task, "nn20-ftp-sess", SESSION_STACK, session,
                        FTP_TASK_PRIORITY, &session->task) != pdPASS) {
            ESP_LOGE(TAG, "cannot start a session task");
            close(client);
            session->control = -1;
            atomic_store_explicit(&session->in_use, false,
                                  memory_order_release);
        }
    }

    pthis->acceptor = NULL;
    vTaskDelete(NULL);
}

/* --------------------------------------------------------- lifecycle -- */

NN20ClockFtp *nn20clock_ftp_ctor(NN20ClockFtpOps ops, uint16_t port)
{
    /* A partial set would be discovered halfway through a transfer,
     * which is the worst possible moment. */
    if (ops.list == NULL || ops.stat == NULL || ops.open_read == NULL ||
        ops.open_write == NULL || ops.read == NULL || ops.write == NULL ||
        ops.close == NULL || ops.remove_file == NULL ||
        ops.make_dir == NULL || ops.remove_dir == NULL ||
        ops.rename == NULL) {
        ESP_LOGE(TAG, "incomplete operations");
        return NULL;
    }

    NN20ClockFtp *pthis = calloc(1, sizeof(*pthis));
    if (pthis == NULL) {
        ESP_LOGE(TAG, "out of memory");
        return NULL;
    }

    pthis->ops = ops;
    pthis->port = (port != 0) ? port : NN20CLOCK_FTP_DEFAULT_PORT;
    pthis->listener = -1;
    atomic_init(&pthis->running, false);
    atomic_init(&pthis->stopping, false);
    atomic_init(&pthis->uploads, 0u);
    atomic_init(&pthis->downloads, 0u);
    atomic_init(&pthis->mutations, 0u);
    for (size_t i = 0; i < NN20CLOCK_FTP_MAX_SESSIONS; i++) {
        atomic_init(&pthis->sessions[i].in_use, false);
        atomic_init(&pthis->sessions[i].activity,
                    (int)NN20CLOCK_FTP_ACTIVITY_IDLE);
        pthis->sessions[i].control = -1;
        pthis->sessions[i].data = -1;
        pthis->sessions[i].data_listener = -1;
    }
    return pthis;
}

void nn20clock_ftp_dtor(NN20ClockFtp *pthis)
{
    if (pthis == NULL) {
        return;
    }
    if (nn20clock_ftp_is_running(pthis)) {
        (void)nn20clock_ftp_stop(pthis);
    }
    free(pthis);
}

esp_err_t nn20clock_ftp_start(NN20ClockFtp *pthis)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (nn20clock_ftp_is_running(pthis)) {
        return ESP_ERR_INVALID_STATE;
    }

    pthis->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (pthis->listener < 0) {
        ESP_LOGE(TAG, "cannot create the listening socket");
        return ESP_FAIL;
    }

    /* Without this, restarting the server fails for a minute or two
     * while the old socket sits in TIME_WAIT. */
    const int reuse = 1;
    (void)setsockopt(pthis->listener, SOL_SOCKET, SO_REUSEADDR, &reuse,
                     sizeof(reuse));

    const struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(pthis->port),
    };
    if (bind(pthis->listener, (const struct sockaddr *)&address,
             sizeof(address)) != 0) {
        ESP_LOGE(TAG, "cannot bind port %u", (unsigned)pthis->port);
        close(pthis->listener);
        pthis->listener = -1;
        return ESP_FAIL;
    }
    if (listen(pthis->listener, NN20CLOCK_FTP_MAX_SESSIONS) != 0) {
        ESP_LOGE(TAG, "cannot listen on port %u", (unsigned)pthis->port);
        close(pthis->listener);
        pthis->listener = -1;
        return ESP_FAIL;
    }

    atomic_store_explicit(&pthis->stopping, false, memory_order_release);
    atomic_store_explicit(&pthis->running, true, memory_order_release);

    if (xTaskCreate(acceptor_task, "nn20-ftp", ACCEPTOR_STACK, pthis,
                    FTP_TASK_PRIORITY, &pthis->acceptor) != pdPASS) {
        ESP_LOGE(TAG, "cannot start the acceptor task");
        atomic_store_explicit(&pthis->running, false, memory_order_release);
        close(pthis->listener);
        pthis->listener = -1;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "listening on port %u, no authentication",
             (unsigned)pthis->port);
    return ESP_OK;
}

esp_err_t nn20clock_ftp_stop(NN20ClockFtp *pthis)
{
    if (pthis == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!nn20clock_ftp_is_running(pthis)) {
        return ESP_ERR_INVALID_STATE;
    }

    atomic_store_explicit(&pthis->stopping, true, memory_order_release);

    /* Closing the listener is what wakes the acceptor out of accept();
     * there is no other way to interrupt it. */
    if (pthis->listener >= 0) {
        close(pthis->listener);
        pthis->listener = -1;
    }

    /* And closing each control socket wakes its session out of recv().
     * The tasks delete themselves once they notice. */
    for (size_t i = 0; i < NN20CLOCK_FTP_MAX_SESSIONS; i++) {
        Session *session = &pthis->sessions[i];
        if (atomic_load_explicit(&session->in_use, memory_order_acquire) &&
            session->control >= 0) {
            shutdown(session->control, SHUT_RDWR);
        }
    }

    /*
     * Wait for them to go. The caller is about to free what the
     * callbacks use - the SD card handle, in this application - so
     * returning while a transfer is still running would be a
     * use-after-free with a network connection attached to it.
     */
    for (int attempt = 0; attempt < 100; attempt++) {
        if (pthis->acceptor == NULL &&
            nn20clock_ftp_session_count(pthis) == 0u) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    atomic_store_explicit(&pthis->running, false, memory_order_release);
    ESP_LOGI(TAG, "stopped");
    return ESP_OK;
}

bool nn20clock_ftp_is_running(const NN20ClockFtp *pthis)
{
    return pthis != NULL &&
           atomic_load_explicit(&pthis->running, memory_order_acquire);
}

/* ---------------------------------------------------------- sessions -- */

size_t nn20clock_ftp_session_count(const NN20ClockFtp *pthis)
{
    if (pthis == NULL) {
        return 0u;
    }

    size_t count = 0;
    for (size_t i = 0; i < NN20CLOCK_FTP_MAX_SESSIONS; i++) {
        if (atomic_load_explicit(&pthis->sessions[i].in_use,
                                 memory_order_acquire)) {
            count++;
        }
    }
    return count;
}

esp_err_t nn20clock_ftp_session_address(const NN20ClockFtp *pthis,
                                        size_t index, char *out, size_t size)
{
    if (pthis == NULL || out == NULL || size < INET_ADDRSTRLEN ||
        index >= NN20CLOCK_FTP_MAX_SESSIONS) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    if (!atomic_load_explicit(&pthis->sessions[index].in_use,
                              memory_order_acquire)) {
        return ESP_ERR_NOT_FOUND;
    }

    snprintf(out, size, "%s", pthis->sessions[index].address);
    return ESP_OK;
}

uint16_t nn20clock_ftp_port(const NN20ClockFtp *pthis)
{
    return (pthis == NULL) ? 0u : pthis->port;
}

esp_err_t nn20clock_ftp_session_activity(const NN20ClockFtp *pthis,
                                         size_t index,
                                         NN20ClockFtpActivity *out_activity)
{
    if (pthis == NULL || out_activity == NULL ||
        index >= NN20CLOCK_FTP_MAX_SESSIONS) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_activity = NN20CLOCK_FTP_ACTIVITY_IDLE;

    if (!atomic_load_explicit(&pthis->sessions[index].in_use,
                              memory_order_acquire)) {
        return ESP_ERR_NOT_FOUND;
    }

    *out_activity = (NN20ClockFtpActivity)atomic_load_explicit(
        &pthis->sessions[index].activity, memory_order_relaxed);
    return ESP_OK;
}

const char *nn20clock_ftp_activity_name(NN20ClockFtpActivity activity)
{
    switch (activity) {
    case NN20CLOCK_FTP_ACTIVITY_IDLE:         return "idle";
    case NN20CLOCK_FTP_ACTIVITY_LISTING:      return "listing";
    case NN20CLOCK_FTP_ACTIVITY_DOWNLOADING:  return "downloading";
    case NN20CLOCK_FTP_ACTIVITY_UPLOADING:    return "uploading";
    case NN20CLOCK_FTP_ACTIVITY_DELETING:     return "deleting";
    case NN20CLOCK_FTP_ACTIVITY_CREATING_DIR: return "creating a folder";
    case NN20CLOCK_FTP_ACTIVITY_RENAMING:     return "renaming";
    case NN20CLOCK_FTP_ACTIVITY_HIDING:       return "hiding";
    }
    return "unknown";
}

uint32_t nn20clock_ftp_uploads(const NN20ClockFtp *pthis)
{
    return (pthis == NULL)
               ? 0u
               : atomic_load_explicit(&pthis->uploads, memory_order_relaxed);
}

uint32_t nn20clock_ftp_downloads(const NN20ClockFtp *pthis)
{
    return (pthis == NULL)
               ? 0u
               : atomic_load_explicit(&pthis->downloads, memory_order_relaxed);
}

uint32_t nn20clock_ftp_mutations(const NN20ClockFtp *pthis)
{
    return (pthis == NULL)
               ? 0u
               : atomic_load_explicit(&pthis->mutations, memory_order_relaxed);
}
