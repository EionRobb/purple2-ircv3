#ifndef IRC_FILEHOST_H
#define IRC_FILEHOST_H

#include "irc.h"

/**
 * Check if the target (channel or buddy) can receive a file transfer.
 * If FILEHOST is advertised by the server, both channels and buddies can.
 */
gboolean irc_can_receive_file(PurpleConnection *gc, const char *who);
gboolean irc_chat_can_receive_file(PurpleConnection *gc, int id);
void irc_chat_send_file(PurpleConnection *gc, int id, const char *filename);

/**
 * Initialize a file transfer using the HTTP FILEHOST service.
 */
void irc_filehost_send_init(PurpleXfer *xfer);

/**
 * Cancel or clean up a filehost transfer.
 */
void irc_filehost_destroy(PurpleXfer *xfer);

#endif /* IRC_FILEHOST_H */
