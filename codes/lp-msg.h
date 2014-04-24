/*
 * Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
*/

#ifndef LP_MSG_H
#define LP_MSG_H

#include "ross.h"

/* It is good practice to always include the src LPID, a unique message
 * identifier, and a "magic" number to ensure that the LP type receiving the
 * message is the intended recipient. This is formalized in the following
 * struct and is used in a few places where LP-to-LP communication is
 * abstracted */

typedef struct msg_header_s {
    tw_lpid src;
    int event_type;
    int magic; 
} msg_header;

/* data structure utilities */
void msg_set_header(int magic, int event_type, tw_lpid src, msg_header *h);

#endif /* end of include guard: LP_MSG_H */

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
