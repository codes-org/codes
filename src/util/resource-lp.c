/*
 * Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
*/

#include "codes/resource-lp.h"
#include "codes/resource.h"
#include "codes/codes_mapping.h"
#include "codes/jenkins-hash.h"
#include "ross.h"
#include <assert.h>
#include <stdio.h>


/**** BEGIN SIMULATION DATA STRUCTURES ****/

static int resource_magic; /* use this as sanity check on events */
/* TODO: we currently use a single config value to initialize the resource unit
 * count for all resources in the system. Later on, we'll want to do this on a
 * per-group basis */
static uint64_t avail_global;

typedef struct resource_state resource_state;
typedef struct resource_msg resource_msg;

#define TOKEN_DUMMY ((resource_token_t)-1)

/* event types */
enum resource_event
{
    RESOURCE_INIT = 100,
    RESOURCE_GET,
    RESOURCE_FREE,
    RESOURCE_RESERVE,
    RESOURCE_GET_RESERVED,
    RESOURCE_FREE_RESERVED
};

struct resource_state {
    resource r;
    int is_init;
};

struct resource_msg {
    msg_header h;
    /* request data */
    uint64_t req;
    resource_token_t tok; /* only for reserved calls */
    /* callback data */
    msg_header h_callback;
    int msg_size;
    int msg_header_offset;
    int msg_callback_offset;
};

/**** END SIMULATION DATA STRUCTURES ****/

/**** BEGIN LP, EVENT PROCESSING FUNCTION DECLS ****/

/* ROSS LP processing functions */  
static void resource_lp_ind_init(
    resource_state * ns,
    tw_lp * lp);
static void resource_event_handler(
    resource_state * ns,
    tw_bf * b,
    resource_msg * m,
    tw_lp * lp);
static void resource_rev_handler(
    resource_state * ns,
    tw_bf * b,
    resource_msg * m,
    tw_lp * lp);
static void resource_finalize(
    resource_state * ns,
    tw_lp * lp);

/* ROSS function pointer table for this LP */
static tw_lptype resource_lp = {
     (init_f) resource_lp_ind_init,
     (event_f) resource_event_handler,
     (revent_f) resource_rev_handler,
     (final_f)  resource_finalize, 
     (map_f) codes_mapping,
     sizeof(resource_state),
};

/**** END LP, EVENT PROCESSING FUNCTION DECLS ****/

/**** BEGIN IMPLEMENTATIONS ****/

void resource_lp_ind_init(
        resource_state * ns,
        tw_lp * lp){
    /* currently use global to initialize, may need to have other LPs init */
    ns->is_init = 1;
    resource_init(avail_global, &ns->r);
}

void resource_event_handler(
        resource_state * ns,
        tw_bf * b,
        resource_msg * m,
        tw_lp * lp){
    assert(m->h.magic == resource_magic);
    
    int send_ack = 0;
    int ret;
    resource_token_t tok = TOKEN_DUMMY;
    switch (m->h.event_type){
        case RESOURCE_INIT:
            assert(0);/* this should not be called */
            assert(ns->is_init == 0);
            resource_init(m->req, &ns->r);
            ns->is_init = 1;
            break;
        case RESOURCE_GET:
            assert(ns->is_init);
            ret = resource_get(m->req, &ns->r);
            send_ack = 1;
            break;
        case RESOURCE_FREE:
            assert(ns->is_init);
            resource_free(m->req, &ns->r);
            break;
        case RESOURCE_RESERVE:
            assert(ns->is_init);
            ret = resource_reserve(m->req, &tok, &ns->r);
            send_ack = 1;
            /* even though we don't expect reserve to RC, set the tok in the
             * message anyways */
            m->tok = tok;
            break;
        case RESOURCE_GET_RESERVED:
            assert(ns->is_init);
            ret = reserved_get(m->req, m->tok, &ns->r);
            send_ack = 1;
            break;
        case RESOURCE_FREE_RESERVED:
            assert(ns->is_init);
            reserved_free(m->req, m->tok, &ns->r);
            break;
        default:
            tw_error(TW_LOC, "resource event type not known");
            break;
    }
    if (send_ack){
        /* we use bitfield to determine whether previous op was a success,
         * allowing us to do rollback a bit easier */
        b->c0 = !ret;
        /* send return message */

        msg_header h;
        msg_set_header(m->h_callback.magic, m->h_callback.event_type, 
                lp->gid, &h);

        resource_callback c;
        c.ret = ret;
        c.tok = tok;

        /* before we send the message, sanity check the sizes */
        if (m->msg_size >= m->msg_header_offset+sizeof(h) &&
                m->msg_size >= m->msg_callback_offset+sizeof(c)){
            tw_event *e = codes_event_new(m->h_callback.src, 
                    codes_local_latency(lp), lp);
            void *msg = tw_event_data(e);
            memcpy(((char*)msg)+m->msg_header_offset, &h, sizeof(h));
            memcpy(((char*)msg)+m->msg_callback_offset, &c, sizeof(c));
            tw_event_send(e);
        }
        else{
            tw_error(TW_LOC, 
                    "message size not large enough to hold header/callback "
                    "structures");
        }
    }
}

void resource_rev_handler(
        resource_state * ns,
        tw_bf * b,
        resource_msg * m,
        tw_lp * lp){
    assert(m->h.magic == resource_magic);
    
    int send_ack = 0;
    switch (m->h.event_type){
        case RESOURCE_INIT:
            assert(0); /* not currently used */
            /* this really shouldn't happen, but who knows... */
            ns->is_init = 0;
            break;
        case RESOURCE_GET:
            send_ack = 1;
            if (b->c0){ resource_free(m->req, &ns->r); }
            break;
        case RESOURCE_FREE:
            /* "re-allocate" the resource (this MUST work under the current
             * implementation) */
            assert(0==resource_get(m->req, &ns->r));
            break;
        case RESOURCE_RESERVE:
            /* this reversal method is essentially a hack that relies on each
             * sequential reserve appending to the end of the list */
            send_ack = 1;
            if (b->c0){ ns->r.num_tokens--; }
            break;
        case RESOURCE_GET_RESERVED:
            send_ack = 1;
            if (b->c0){ reserved_free(m->req, m->tok, &ns->r); }
            break;
        case RESOURCE_FREE_RESERVED:
            /* "re-allocate" the resource (this MUST work under the current
             * implementation) */
            assert(0==reserved_get(m->req, m->tok, &ns->r));
            break;
        default:
            tw_error(TW_LOC, "resource event type not known");
    }
    if (send_ack){ codes_local_latency_reverse(lp); }
}

void resource_finalize(
        resource_state * ns,
        tw_lp * lp){
    /* Fill me in... */
}

/**** END IMPLEMENTATIONS ****/

/**** BEGIN USER-FACING FUNCTIONS ****/
void resource_lp_init(){
    uint32_t h1=0, h2=0;

    bj_hashlittle2("resource", strlen("resource"), &h1, &h2);
    resource_magic = h1+h2;

    lp_type_register("resource", &resource_lp);
}

void resource_lp_configure(){
    long int avail;
    int ret = configuration_get_value_longint(&config, "resource", 
            "available", &avail);
    if (ret){
        fprintf(stderr, "Could not find section:resource value:available for "
                        "resource LP\n");
        exit(1);
    }
    assert(avail > 0);
    avail_global = (uint64_t) avail;
}

static void resource_lp_issue_event(
        msg_header *header,
        uint64_t req,
        resource_token_t tok, /* only used in reserve_get/free */
        int msg_size,
        int msg_header_offset,
        int msg_callback_offset,
        enum resource_event type,
        tw_lp *sender){

    tw_lpid resource_lpid;

    /* map out the lpid of the resource */
    int mapping_grp_id, mapping_type_id, mapping_rep_id, mapping_offset;
    char lp_type_name[MAX_NAME_LENGTH], lp_group_name[MAX_NAME_LENGTH];
    codes_mapping_get_lp_info(sender->gid, lp_group_name, 
            &mapping_grp_id, &mapping_type_id, lp_type_name, 
            &mapping_rep_id, &mapping_offset);
    codes_mapping_get_lp_id(lp_group_name, "resource", mapping_rep_id, 
            mapping_offset, &resource_lpid); 

    tw_event *e = codes_event_new(resource_lpid, codes_local_latency(sender),
            sender);

    /* set message info */
    resource_msg *m = tw_event_data(e);
    msg_set_header(resource_magic, type, sender->gid, &m->h);
    m->req = req;
    m->tok = tok;

    /* set callback info */
    if (header != NULL){
        m->h_callback = *header;
    }
    m->msg_size = msg_size;
    m->msg_header_offset = msg_header_offset;
    m->msg_callback_offset = msg_callback_offset;

    tw_event_send(e);
}

void resource_lp_get(
        msg_header *header,
        uint64_t req, 
        int msg_size, 
        int msg_header_offset,
        int msg_callback_offset,
        tw_lp *sender){
    resource_lp_issue_event(header, req, TOKEN_DUMMY, msg_size, 
            msg_header_offset, msg_callback_offset, RESOURCE_GET, sender);
}

/* no callback for frees thus far */
void resource_lp_free(uint64_t req, tw_lp *sender){
    resource_lp_issue_event(NULL, req, TOKEN_DUMMY, -1,-1,-1,
            RESOURCE_FREE, sender);
}
void resource_lp_reserve(
        msg_header *header, 
        uint64_t req,
        int msg_size,
        int msg_header_offset,
        int msg_callback_offset,
        tw_lp *sender){
    resource_lp_issue_event(header, req, TOKEN_DUMMY, msg_size, 
            msg_header_offset, msg_callback_offset, RESOURCE_RESERVE, sender);
}
void resource_lp_get_reserved(
        msg_header *header,
        uint64_t req,
        resource_token_t tok,
        int msg_size, 
        int msg_header_offset,
        int msg_callback_offset,
        tw_lp *sender){
    resource_lp_issue_event(header, req, tok, msg_size, 
            msg_header_offset, msg_callback_offset, RESOURCE_GET_RESERVED,
            sender);
}
void resource_lp_free_reserved(
        uint64_t req, 
        resource_token_t tok,
        tw_lp *sender){
    resource_lp_issue_event(NULL, req, tok, -1,-1,-1, RESOURCE_FREE_RESERVED,
            sender);
}

/* rc functions - thankfully, they only use codes-local-latency, so no need 
 * to pass in any arguments */

static void resource_lp_issue_event_rc(tw_lp *sender){
    codes_local_latency_reverse(sender);
}

void resource_lp_get_rc(tw_lp *sender){
    resource_lp_issue_event_rc(sender);
}
void resource_lp_free_rc(tw_lp *sender){
    resource_lp_issue_event_rc(sender);
}
void resource_lp_reserve_rc(tw_lp *sender){
    resource_lp_issue_event_rc(sender);
}
void resource_lp_get_reserved_rc(tw_lp *sender){
    resource_lp_issue_event_rc(sender);
}
void resource_lp_free_reserved_rc(tw_lp *sender){
    resource_lp_issue_event_rc(sender);
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
