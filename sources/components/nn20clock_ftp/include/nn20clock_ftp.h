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
 * nn20clock_ftp.h - a small FTP server (design 12).
 *
 * FIRMWARE ONLY: it is a socket server on the device's network stack.
 *
 * Design 12 asks for a reusable component, so this knows nothing about
 * clocks or alarms. It serves a directory tree through a set of
 * callbacks and never touches the SD component directly. Its one
 * dependency beyond the platform shim is nn20clock_media, for the path
 * rules: turning what a client typed into a safe relative path is the
 * security boundary of this server, and duplicating that logic here so
 * the component could claim to stand alone would be the wrong trade.
 *
 * ---------------------------------------------------------------
 * What it does and does not implement
 * ---------------------------------------------------------------
 *
 * Implemented: USER/PASS (accepted from anyone - see below), SYST,
 * FEAT, TYPE, PWD, CWD, CDUP, PASV, EPSV, LIST, NLST, RETR, STOR, DELE,
 * MKD, RMD, RNFR, RNTO, SIZE, SITE, NOOP, QUIT.
 *
 * ---------------------------------------------------------------
 * SITE HIDE and SITE UNHIDE - an extension, and why
 * ---------------------------------------------------------------
 *
 *     SITE HIDE TRASH-~1
 *     SITE UNHIDE TRASH-~1
 *
 * FTP has no command for file attributes. RFC 959 has no notion of
 * them, and RFC 3659 stops at facts a server reports rather than ones a
 * client sets. What it does have is SITE, which is explicitly the place
 * for commands a particular server offers and nobody else does - which
 * is exactly what this is.
 *
 * It exists because the owner's listing skips entries carrying FAT's
 * hidden bit, and a card that has been in a desktop machine collects
 * directories nobody wants to see in a picker. Some of them carry the
 * bit and some do not, so there has to be a way to say "not this one"
 * without a code change.
 *
 * Hidden means not listed. It is not a permission: RETR, DELE, RMD and
 * rename all still work on a hidden path, which is what makes it
 * possible to undo.
 *
 * Any other SITE verb is refused with 504, not ignored.
 *
 * Not implemented, deliberately:
 *
 *   PORT/EPRT (active mode)  The server would have to open a connection
 *                            back to the client, which home routers and
 *                            firewalls block anyway. Passive is what
 *                            every modern client uses; PORT is refused
 *                            with a clear message rather than left to
 *                            time out.
 *   Recursive RMD            RMD removes an empty directory and says so
 *                            otherwise. "Delete this and everything
 *                            under it" is one mistyped path away from
 *                            an empty card, and clients send it without
 *                            asking.
 *   Overwriting RNTO         A rename onto an existing name is refused.
 *                            The card holds the only copy of a clip.
 *
 * ---------------------------------------------------------------
 * Paths
 * ---------------------------------------------------------------
 *
 * Each session owns a current directory, held as a canonical relative
 * path from the served root - "" for the root itself, "morning",
 * "weekend/kids". Every path a client sends is resolved against it by
 * nn20clock_media_path_resolve(), which is the one place "..", a
 * leading slash and a trailing slash mean anything.
 *
 * ".." may walk towards the root and never past it. "/../etc" is
 * refused, not repaired into "etc" - and what reaches the callbacks is
 * a canonical relative path with no "..", no leading slash and no
 * trailing slash left in it. The callbacks are still expected to
 * validate what they are given; two checks of the same rule is the
 * point, not a redundancy to remove.
 *
 * ---------------------------------------------------------------
 * Security
 * ---------------------------------------------------------------
 *
 * THERE IS NO AUTHENTICATION. Any device on the network can list,
 * upload, download and delete files in the served directory. That is a
 * deliberate choice for a clock on a home LAN: FTP sends credentials in
 * clear text, so a password would be theatre rather than protection,
 * and it is documented here and in the README instead of implied.
 *
 * What limits the exposure is time rather than a password. The clock
 * starts this server only while its media management screen is open and
 * stops it on the way out, so the window is a deliberate act at the
 * device rather than the whole uptime. That policy belongs to the
 * application, not here - this component listens when it is told to -
 * but it is the reason no authentication is an acceptable answer.
 *
 * What it does guarantee is that a client cannot escape the served
 * directory: every path from the wire is canonicalised by the media
 * rules before any callback sees it, the callback owner checks it
 * again, and no text from the wire is ever concatenated into a
 * filesystem path here.
 *
 * ---------------------------------------------------------------
 * Threading
 * ---------------------------------------------------------------
 *
 * This is the one part of the project that does NOT run on a worker.
 * A worker executes short callbacks in order; an FTP server sits
 * blocked in accept() and then in recv() for as long as a client stays
 * connected, which would occupy a worker forever and defeat its whole
 * purpose. So it owns FreeRTOS tasks: one acceptor, and one per
 * session.
 *
 * Its callbacks are therefore invoked on those tasks, not on any
 * worker. An implementation that touches worker-owned state must post,
 * exactly as any other thread would.
 */
#ifndef NN20CLOCK_FTP_H
#define NN20CLOCK_FTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nn20clock_media.h"
#include "nn20clock_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NN20ClockFtp NN20ClockFtp;

/* The standard control port. */
#define NN20CLOCK_FTP_DEFAULT_PORT 21

/*
 * Clients served at once.
 *
 * Two, because graphical clients routinely open a second connection
 * while browsing, and one would refuse it in a way that looks like a
 * broken server. More than that has no purpose on a device with one
 * card and one SD bus.
 */
#define NN20CLOCK_FTP_MAX_SESSIONS 2

/* An entry the server reports to a client. `is_dir` is not cosmetic:
 * LIST's leading "d" is how every graphical client decides what is a
 * folder, and a directory reported as a file cannot be opened. */
typedef struct {
    char name[256];
    uint32_t size_bytes;
    bool is_dir;
} NN20ClockFtpEntry;

/* The longest directory path a session can be in. The server holds one
 * per session and passes it to `list`; it is the media layer's cap,
 * because that is what the owner can actually address. */
#define NN20CLOCK_FTP_PATH_MAX NN20CLOCK_MEDIA_PATH_MAX

/*
 * What the server needs from its owner. All are called on the server's
 * own tasks.
 *
 * Every `dir` and `path` is a canonical relative path from the served
 * root, already through nn20clock_media_path_resolve(). The owner must
 * still validate it: the server guarantees the shape of the string, not
 * that the owner is willing to serve it.
 *
 * `list` writes up to `max` entries for the directory it is given and
 * returns how many.
 */
typedef struct {
    size_t (*list)(void *ctx, const char *dir, NN20ClockFtpEntry *entries,
                   size_t max);
    /* What is at a path: ESP_OK and the two out values, or an error
     * when there is nothing there. This is how CWD knows a directory
     * exists and how LIST knows it was given a file. */
    esp_err_t (*stat)(void *ctx, const char *path, bool *out_is_dir,
                      uint32_t *out_size);
    /* Open for reading or writing; return NULL to refuse. The server
     * closes what it is given. */
    void *(*open_read)(void *ctx, const char *path);
    void *(*open_write)(void *ctx, const char *path);
    size_t (*read)(void *ctx, void *handle, void *buffer, size_t size);
    size_t (*write)(void *ctx, void *handle, const void *buffer,
                    size_t size);
    void (*close)(void *ctx, void *handle);
    esp_err_t (*remove_file)(void *ctx, const char *path);
    esp_err_t (*make_dir)(void *ctx, const char *path);
    /* Empty directories only - see the header comment. */
    esp_err_t (*remove_dir)(void *ctx, const char *path);
    /* Both ends are under the served root, so this may move between
     * directories. Refusing to overwrite is the owner's call. */
    esp_err_t (*rename)(void *ctx, const char *from, const char *to);
    /*
     * Show or hide one entry - SITE HIDE / SITE UNHIDE above. Optional:
     * leave it NULL and the server answers 502, which is the honest
     * reply from an owner whose storage has no such idea.
     */
    esp_err_t (*set_hidden)(void *ctx, const char *path, bool hidden);
    void *ctx;
} NN20ClockFtpOps;

/* --------------------------------------------------------- lifecycle -- */

/*
 * `ops` is copied. Every function in it is required; a partial set is
 * refused rather than discovered halfway through a transfer.
 */
NN20ClockFtp *nn20clock_ftp_ctor(NN20ClockFtpOps ops, uint16_t port);

/* Stops first if running. Safe with NULL. */
void nn20clock_ftp_dtor(NN20ClockFtp *pthis);

/*
 * Begin listening. Needs the network up and something to serve; the
 * caller decides when that is true and for how long it should last.
 */
esp_err_t nn20clock_ftp_start(NN20ClockFtp *pthis);

/*
 * Stop listening and disconnect any client. Blocks until the tasks have
 * gone, so it is safe to free what the callbacks use afterwards.
 */
esp_err_t nn20clock_ftp_stop(NN20ClockFtp *pthis);

bool nn20clock_ftp_is_running(const NN20ClockFtp *pthis);

/* The control port it was constructed with. Zero for NULL. A status
 * screen has to print the port the server is actually on, not the one
 * the default happens to be. */
uint16_t nn20clock_ftp_port(const NN20ClockFtp *pthis);

/* ---------------------------------------------------------- sessions -- */

/*
 * Clients connected right now. Design 12 asks for this so a media
 * management screen can show who is connected.
 */
size_t nn20clock_ftp_session_count(const NN20ClockFtp *pthis);

/*
 * The address of a connected client, e.g. "192.168.0.31". Returns
 * ESP_ERR_NOT_FOUND when there is no session at that index; `index`
 * runs from 0 to NN20CLOCK_FTP_MAX_SESSIONS - 1.
 */
esp_err_t nn20clock_ftp_session_address(const NN20ClockFtp *pthis,
                                        size_t index, char *out, size_t size);

/*
 * What a session is doing right now, for design 11's media-management
 * screen: "current operation when known".
 *
 * Deliberately an operation and not a file name. The name would have to
 * be a buffer written by the session's task and read by the UiWorker,
 * and there is no lock in this project to make that copy consistent -
 * the reader could see half of one name and half of the next. An
 * enumeration is a single value the reader either sees before or after
 * the change, which is the whole reason it is shaped this way.
 */
typedef enum {
    NN20CLOCK_FTP_ACTIVITY_IDLE,          /* connected, between commands */
    NN20CLOCK_FTP_ACTIVITY_LISTING,
    NN20CLOCK_FTP_ACTIVITY_DOWNLOADING,   /* the client is reading a file */
    NN20CLOCK_FTP_ACTIVITY_UPLOADING,     /* the client is writing one */
    NN20CLOCK_FTP_ACTIVITY_DELETING,      /* a file or an empty directory */
    NN20CLOCK_FTP_ACTIVITY_CREATING_DIR,
    NN20CLOCK_FTP_ACTIVITY_RENAMING,
    NN20CLOCK_FTP_ACTIVITY_HIDING
} NN20ClockFtpActivity;

/*
 * What session `index` is doing. ESP_ERR_NOT_FOUND when there is no
 * session there, exactly as nn20clock_ftp_session_address().
 *
 * A snapshot: the session's own task moves this as it works, so it can
 * change the instant after it is read. That is fine for a status line
 * and is not something to decide anything on.
 */
esp_err_t nn20clock_ftp_session_activity(const NN20ClockFtp *pthis,
                                         size_t index,
                                         NN20ClockFtpActivity *out_activity);

/* "idle", "listing", "downloading", ... - for the screen and the log. */
const char *nn20clock_ftp_activity_name(NN20ClockFtpActivity activity);

/* Files uploaded and downloaded since the server started, for the log
 * and for diagnostics. */
uint32_t nn20clock_ftp_uploads(const NN20ClockFtp *pthis);
uint32_t nn20clock_ftp_downloads(const NN20ClockFtp *pthis);

/*
 * Every change a client has made to what is served: an upload that
 * completed, a delete, a directory created or removed, a rename.
 *
 * One counter rather than one per operation, because what a status
 * screen actually asks is "is what I last read still true" - and
 * inferring that from session disconnects, which is what this replaces,
 * misses a client that changes something and stays connected.
 */
uint32_t nn20clock_ftp_mutations(const NN20ClockFtp *pthis);

#ifdef __cplusplus
}
#endif

#endif /* NN20CLOCK_FTP_H */
