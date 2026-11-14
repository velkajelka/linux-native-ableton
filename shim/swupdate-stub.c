/* A stand-in for libswupdate.so.0.1, a library of the Push 3's device OS that
 * the recovery image does not carry: the update framework lives in the image's
 * /usr/lib, outside the /opt/push3 tree install.sh copies.
 *
 * The Push 3 companion (Push3, the process Live starts whenever Push hardware
 * is connected) links it to run firmware updates against the device's swupdate
 * daemon. A desktop has neither the daemon nor device firmware to update, so
 * every entry point reports just that: no daemon, no update. Without this
 * library the companion cannot even load, and Push hardware stays dark.
 */
#include <stddef.h>

/* ipc_postupdate: ask the daemon to install an update described by json.
 * Returns -1: the request cannot be served. */
int ipc_postupdate(const char *json) { (void)json; return -1; }

/* progress_ipc_connect: open the daemon's progress socket. Returns -1:
 * there is no daemon to connect to. */
int progress_ipc_connect(int autoreconnect) { (void)autoreconnect; return -1; }

/* progress_ipc_receive: read one progress message into *message.
 * Returns -1: no connection, hence no messages, ever. */
int progress_ipc_receive(void *message) { (void)message; return -1; }

void progress_ipc_cleanup(void) {}

/* ipc_inst_start: start an update streamed over the given descriptor. */
int ipc_inst_start(const char *software, size_t len)
{
    (void)software;
    (void)len;
    return -1;
}

int ipc_wait_for_complete(void) { return -1; }

void ipc_end(int fd) { (void)fd; }
