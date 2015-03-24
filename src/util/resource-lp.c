/*
 * Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
*/

#include "codes/resource-lp.h"
#include "codes/resource.h"
#include "codes/codes_mapping.h"
#include "codes/configuration.h"
#include "codes/jenkins-hash.h"
#include "codes/quicklist.h"
#include "codes/lp-io.h"
#include "ross.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>


/**** BEGIN SIMULATION DATA STRUCTURES ****/

static int resource_magic; /* use this as sanity check on events */

/* configuration globals (will be consumed by LP when they init) */
static uint64_t avail_unanno;
static uint64_t *avail_per_anno;
static const config_anno_map_t *anno_map;

typedef struct resource_state resource_state;
typedef struct resource_msg resource_msg;
typedef struct pending_op pending_op;

#define TOKEN_DUMMY ((resource_token_t)-1)

/* event types */
enum resource_event
{
    RESOURCE_GET = 100,
    RESOURCE_FREE,
    RESOURCE_DEQ,
    RESOURCE_RESERVE,
};

struct resource_state {
    resource r;
    /* pending operations - if OOM and we are using the 'blocking' method, 
     * then need to stash parameters.
     * Index 0 is the general pool, index 1.. are the reservation-specific
     * pools. We take advantage of resource_token_t's status as a simple 
     * array index to do the proper indexing */
    struct qlist_head pending[MAX_RESERVE+1];
};

/* following struct exists because we want to basically cache a message within
 * a message for rc (ewww) */
struct resource_msg_internal{
    msg_header h;
    /* request data */
    uint64_t req;
    resource_token_t tok; /* only for reserved calls */
    /* behavior when sending response to caller
     * 0 - send the callback immediately if resource unavailable. 
     * 1 - send the callback when memory is available (danger - deadlock
     * possible) */
    int block_on_unavail; 
    /* callback data */
    msg_header h_callback;
    int msg_size;
    int msg_header_offset;
    int msg_callback_offset;
    /* user-provided data */
    int msg_callback_misc_size;
    int msg_callback_misc_offset;
    char msg_callback_misc[RESOURCE_MAX_CALLBACK_PAYLOAD];
}; 

struct resource_msg {
    struct resource_msg_internal i, i_rc;
    // for RC (asides from the message itself): the previous minimum resource
    // value
    uint64_t min_avail_rc;
};

struct pending_op {
    struct resource_msg_internal m;
    struct qlist_head ql;
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
    (pre_run_f) NULL,
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
    // get my annotation
    const char * anno = codes_mapping_get_annotation_by_lpid(lp->gid);
    if (anno == NULL){
        resource_init(avail_unanno, &ns->r);
    }
    else{
        int idx = configuration_get_annotation_index(anno, anno_map);
        if (idx < 0){
            tw_error("resource LP %lu: unable to find annotation "
                    "%s in configuration\n", lp->gid, anno);
        }
        else{
            resource_init(avail_per_anno[idx], &ns->r);
        }
    }
    int i;
    for (i = 0; i < MAX_RESERVE+1; i++){
        INIT_QLIST_HEAD(&ns->pending[i]);
    }
}

static void resource_response(
        struct resource_msg_internal *m,
        tw_lp *lp,
        int ret,
        resource_token_t tok){
    /* send return message */
    msg_header h;
    msg_set_header(m->h_callback.magic, m->h_callback.event_type, 
            lp->gid, &h);

    resource_callback c;
    c.ret = ret;
    c.tok = tok;

    /* before we send the message, sanity check the sizes */
    if (m->msg_size >= m->msg_header_offset+sizeof(h) &&
            m->msg_size >= m->msg_callback_offset+sizeof(c) &&
            m->msg_size >= m->msg_callback_offset+m->msg_callback_misc_size){
        tw_event *e = codes_event_new(m->h_callback.src, 
                codes_local_latency(lp), lp);
        void *msg = tw_event_data(e);
        memcpy(((char*)msg)+m->msg_header_offset, &h, sizeof(h));
        memcpy(((char*)msg)+m->msg_callback_offset, &c, sizeof(c));
        if (m->msg_callback_misc_size > 0){
            memcpy(((char*)msg)+m->msg_callback_misc_offset, 
                        m->msg_callback_misc, m->msg_callback_misc_size);
        }
        tw_event_send(e);
    }
    else{
        tw_error(TW_LOC, 
                "message size not large enough to hold header/callback/misc"
                " structures\n"
                "msg size: %3d, header   off/size:  %d, %d\n"
                "               callback off/size:  %d, %d\n"
                "               callback misc size: %d",
                m->msg_size, m->msg_header_offset, (int)sizeof(h),
                m->msg_callback_offset, (int)sizeof(c),
                m->msg_callback_misc_size);
    }
}
static void resource_response_rc(tw_lp *lp){
    codes_local_latency_reverse(lp);
}

/* bitfield usage:
 * c0 - enqueued a message 
 * c1 - sent an ack 
 * c2 - successfully got the resource */
static void handle_resource_get(
        resource_state * ns,
        tw_bf * b,
        resource_msg * m,
        tw_lp * lp){
    int ret = 1;
    int send_ack = 1;
    // save the previous minimum for RC
    assert(!resource_get_min_avail(m->i.tok, &m->min_avail_rc, &ns->r));
    if (!qlist_empty(&ns->pending[m->i.tok]) || 
            (ret = resource_get(m->i.req, m->i.tok, &ns->r))){
        /* failed to receive data */
        assert(ret != 2);
        if (m->i.block_on_unavail){
            /* queue up operation, save til later */
            b->c0 = 1;
            pending_op *op = (pending_op*)malloc(sizeof(pending_op));
            op->m = m->i; /* no need to set rc msg here */
            qlist_add_tail(&op->ql, &ns->pending[m->i.tok]);
            send_ack = 0;
        }
    }
    if (send_ack){
        b->c1 = 1;
        resource_response(&m->i, lp, ret, TOKEN_DUMMY);
    }

    b->c2 = !ret;
}

/* bitfield usage:
 * c0 - enqueued a message 
 * c1 - sent an ack 
 * c2 - successfully got the resource */
static void handle_resource_get_rc(
        resource_state * ns,
        tw_bf * b,
        resource_msg * m,
        tw_lp * lp){
    if (b->c0){
        assert(!qlist_empty(&ns->pending[m->i.tok]));
        struct qlist_head *ql = qlist_pop_back(&ns->pending[m->i.tok]);
        free(qlist_entry(ql, pending_op, ql));
    }
    else if (b->c1){
        resource_response_rc(lp);
    }

    if (b->c2){
        assert(!resource_restore_min_avail(m->i.tok, m->min_avail_rc, &ns->r));
        assert(!resource_free(m->i.req, m->i.tok, &ns->r));
    }
}

static void handle_resource_free(
        resource_state * ns,
        tw_bf * b,
        resource_msg * m,
        tw_lp * lp){
    assert(!resource_free(m->i.req, m->i.tok, &ns->r));
    /* create an event to pop the next queue item */
    tw_event *e = codes_event_new(lp->gid, codes_local_latency(lp), lp);
    resource_msg *m_deq = (resource_msg*)tw_event_data(e);
    msg_set_header(resource_magic, RESOURCE_DEQ, lp->gid, &m_deq->i.h);
    m_deq->i.tok = m->i.tok; /* only tok is needed, all others grabbed from q */
    tw_event_send(e);
}
static void handle_resource_free_rc(
        resource_state * ns,
        tw_bf * b,
        resource_msg * m,
        tw_lp * lp){
    assert(!resource_get(m->i.req, m->i.tok, &ns->r));
    codes_local_latency_reverse(lp);
}

/* bitfield usage:
 * c0 - queue was empty to begin with
 * c1 - assuming !c0, alloc succeeded */ 
static void handle_resource_deq(
        resource_state * ns,
        tw_bf * b,
        resource_msg * m,
        tw_lp * lp){
    if (qlist_empty(&ns->pending[m->i.tok])){
        /* nothing to do */
        b->c0 = 1;
        return;
    }

    struct qlist_head *front = ns->pending[m->i.tok].next;
    pending_op *p = qlist_entry(front, pending_op, ql);
    assert(!resource_get_min_avail(m->i.tok, &m->min_avail_rc, &ns->r));
    int ret = resource_get(p->m.req, p->m.tok, &ns->r);
    assert(ret != 2);
    if (!ret){
        b->c1 = 1;
        /* success, dequeue (saving as rc) and send to client */
        qlist_del(front);
        m->i_rc = p->m;
        resource_response(&p->m, lp, ret, TOKEN_DUMMY);
        free(p);
        /* additionally attempt to dequeue next one down */
        tw_event *e = codes_event_new(lp->gid, codes_local_latency(lp), lp);
        resource_msg *m_deq = (resource_msg*)tw_event_data(e);
        msg_set_header(resource_magic, RESOURCE_DEQ, lp->gid, &m_deq->i.h);
        /* only tok is needed, all others grabbed from q */
        m_deq->i.tok = m->i.tok; 
        tw_event_send(e);
    }
    /* else do nothing */
}

/* bitfield usage:
 * c0 - dequeue+alloc success */ 
static void handle_resource_deq_rc(
        resource_state * ns,
        tw_bf * b,
        resource_msg * m,
        tw_lp * lp){
    if (b->c0){
        return;
    }

    if (b->c1){
        /* add operation back to the front of the queue */
        pending_op *op = (pending_op*)malloc(sizeof(pending_op));
        op->m = m->i_rc;
        qlist_add(&op->ql, &ns->pending[m->i.tok]);
        resource_response_rc(lp);
        assert(!resource_restore_min_avail(m->i.tok, m->min_avail_rc, &ns->r)); 
        assert(!resource_free(op->m.req, op->m.tok, &ns->r));
        /* reverse "deq next" op */
        codes_local_latency_reverse(lp);
    }
}

static void handle_resource_reserve(
        resource_state * ns,
        tw_bf * b,
        resource_msg * m,
        tw_lp * lp){
    resource_token_t tok;
    int ret = resource_reserve(m->i.req, &tok, &ns->r);
    assert(!ret);
    resource_response(&m->i, lp, ret, tok);
}
static void handle_resource_reserve_rc(
        resource_state * ns,
        tw_bf * b,
        resource_msg * m,
        tw_lp * lp){
    /* this reversal method is essentially a hack that relies on each
     * sequential reserve appending to the end of the list 
     * - we expect reserves to happen strictly at the beginning of the
     *   simulation */
    ns->r.num_tokens--;
    resource_response_rc(lp);
}

void resource_event_handler(
        resource_state * ns,
        tw_bf * b,
        resource_msg * m,
        tw_lp * lp){
    assert(m->i.h.magic == resource_magic);
    switch(m->i.h.event_type){
        case RESOURCE_GET:
            handle_resource_get(ns,b,m,lp);
            break;
        case RESOURCE_FREE:
            handle_resource_free(ns,b,m,lp);
            break;
        case RESOURCE_DEQ:
            handle_resource_deq(ns,b,m,lp);
            break;
        case RESOURCE_RESERVE:
            handle_resource_reserve(ns,b,m,lp);
            break;
        default:
            assert(0);
    }
}
void resource_rev_handler(
        resource_state * ns,
        tw_bf * b,
        resource_msg * m,
        tw_lp * lp){
    assert(m->i.h.magic == resource_magic);
    switch(m->i.h.event_type){
        case RESOURCE_GET:
            handle_resource_get_rc(ns,b,m,lp);
            break;
        case RESOURCE_FREE:
            handle_resource_free_rc(ns,b,m,lp);
            break;
        case RESOURCE_DEQ:
            handle_resource_deq_rc(ns,b,m,lp);
            break;
        case RESOURCE_RESERVE:
            handle_resource_reserve_rc(ns,b,m,lp);
            break;
        default:
            assert(0);
    }
}

void resource_finalize(
        resource_state * ns,
        tw_lp * lp){
    struct qlist_head *ent;
    for (int i = 0; i < MAX_RESERVE+1; i++){
        qlist_for_each(ent, &ns->pending[i]){
            fprintf(stderr, "WARNING: resource LP %lu has a pending allocation\n",
                    lp->gid);
        }
    }

    char *out_buf = (char*)malloc(1<<12);
    int written;
    // see if I'm the "first" resource (currently doing it globally)
    if (codes_mapping_get_lp_relative_id(lp->gid, 0, 0) == 0){
        written = sprintf(out_buf, 
                "# format: <LP> <max used general> <max used token...>\n");
        lp_io_write(lp->gid, RESOURCE_LP_NM, written, out_buf);
    }
    written = sprintf(out_buf, "%lu", lp->gid);

    // compute peak resource usage
    // TODO: wrap this up in the resource interface
    for (int i = 0; i < ns->r.num_tokens+1; i++){
        written += sprintf(out_buf+written, " %lu", ns->r.max[i]-ns->r.min_avail[i]);
    }
    written += sprintf(out_buf+written, "\n");
    lp_io_write(lp->gid, RESOURCE_LP_NM, written, out_buf);
}

/**** END IMPLEMENTATIONS ****/

/**** BEGIN USER-FACING FUNCTIONS ****/
void resource_lp_init(){
    uint32_t h1=0, h2=0;

    bj_hashlittle2(RESOURCE_LP_NM, strlen(RESOURCE_LP_NM), &h1, &h2);
    resource_magic = h1+h2;

    lp_type_register(RESOURCE_LP_NM, &resource_lp);
}

void resource_lp_configure(){

    anno_map = codes_mapping_get_lp_anno_map(RESOURCE_LP_NM);
    avail_per_anno = (anno_map->num_annos > 0) ?
        (uint64_t*)malloc(anno_map->num_annos * sizeof(*avail_per_anno)) :
            NULL;
    // get the unannotated version
    long int avail;
    int ret;
    if (anno_map->has_unanno_lp > 0){
        ret = configuration_get_value_longint(&config, RESOURCE_LP_NM,
            "available", NULL, &avail);
        if (ret){
            fprintf(stderr,
                    "Could not find section:resource value:available for "
                    "resource LP\n");
            exit(1);
        }
        assert(avail > 0);
        avail_unanno = (uint64_t)avail;
    }
    for (uint64_t i = 0; i < anno_map->num_annos; i++){
        ret = configuration_get_value_longint(&config, RESOURCE_LP_NM,
            "available", anno_map->annotations[i], &avail);
        if (ret){
            fprintf(stderr,
                    "Could not find section:resource value:available@%s for "
                    "resource LP\n", anno_map->annotations[i]);
            exit(1);
        }
        assert(avail > 0);
        avail_per_anno[i] = (uint64_t)avail;
    }
}

static void resource_lp_issue_event_base(
        msg_header *header,
        uint64_t req,
        resource_token_t tok, /* only used in reserve_get/free */
        int block_on_unavail,
        int msg_size,
        int msg_header_offset,
        int msg_callback_offset,
        int msg_callback_misc_size,
        int msg_callback_misc_offset,
        void *msg_callback_misc_data,
        enum resource_event type,
        tw_lp *sender,
        const char * annotation,
        int ignore_annotations){

    tw_lpid resource_lpid;

    /* map out the lpid of the resource */
    int mapping_rep_id, mapping_offset, dummy;
    char lp_group_name[MAX_NAME_LENGTH];
    int resource_count;
    // TODO: currently ignoring annotations... perhaps give annotation as a
    // parameter?
    codes_mapping_get_lp_info(sender->gid, lp_group_name, &dummy,
            NULL, &dummy, NULL,
            &mapping_rep_id, &mapping_offset);
    resource_count = codes_mapping_get_lp_count(lp_group_name, 1,
            RESOURCE_LP_NM, annotation, ignore_annotations);
    codes_mapping_get_lp_id(lp_group_name, RESOURCE_LP_NM, annotation,
            ignore_annotations, mapping_rep_id, mapping_offset % resource_count,
            &resource_lpid);

    tw_event *e = codes_event_new(resource_lpid, codes_local_latency(sender),
            sender);

    /* set message info */
    resource_msg *m = (resource_msg*)tw_event_data(e);
    msg_set_header(resource_magic, type, sender->gid, &m->i.h);
    m->i.req = req;
    m->i.tok = tok;
    m->i.block_on_unavail = block_on_unavail;

    /* set callback info */
    if (header != NULL){
        m->i.h_callback = *header;
    }
    m->i.msg_size = msg_size;
    m->i.msg_header_offset = msg_header_offset;
    m->i.msg_callback_offset = msg_callback_offset;

    if (msg_callback_misc_size > 0){
        assert(msg_callback_misc_size <= RESOURCE_MAX_CALLBACK_PAYLOAD);
        m->i.msg_callback_misc_size = msg_callback_misc_size;
        m->i.msg_callback_misc_offset = msg_callback_misc_offset;
        memcpy(m->i.msg_callback_misc, msg_callback_misc_data,
                msg_callback_misc_size);
    }
    else{
        m->i.msg_callback_misc_size = 0;
        m->i.msg_callback_misc_offset = 0;
    }

    tw_event_send(e);
}

static void resource_lp_issue_event_annotated(
        msg_header *header,
        uint64_t req,
        resource_token_t tok, /* only used in reserve_get/free */
        int block_on_unavail,
        int msg_size,
        int msg_header_offset,
        int msg_callback_offset,
        int msg_callback_misc_size,
        int msg_callback_misc_offset,
        void *msg_callback_misc_data,
        enum resource_event type,
        tw_lp *sender,
        const char * annotation,
        int ignore_annotations){
    resource_lp_issue_event_base(header, req, tok, block_on_unavail, msg_size,
            msg_header_offset, msg_callback_offset, msg_callback_misc_size,
            msg_callback_misc_offset, msg_callback_misc_data, type, sender,
            annotation, ignore_annotations);
}
static void resource_lp_issue_event(
        msg_header *header,
        uint64_t req,
        resource_token_t tok, /* only used in reserve_get/free */
        int block_on_unavail,
        int msg_size,
        int msg_header_offset,
        int msg_callback_offset,
        int msg_callback_misc_size,
        int msg_callback_misc_offset,
        void *msg_callback_misc_data,
        enum resource_event type,
        tw_lp *sender) {
    resource_lp_issue_event_base(header, req, tok, block_on_unavail, msg_size,
            msg_header_offset, msg_callback_offset, msg_callback_misc_size,
            msg_callback_misc_offset, msg_callback_misc_data, type, sender,
            NULL, 1);
}

void resource_lp_get(
        msg_header *header,
        uint64_t req, 
        int block_on_unavail,
        int msg_size, 
        int msg_header_offset,
        int msg_callback_offset,
        int msg_callback_misc_size,
        int msg_callback_misc_offset,
        void *msg_callback_misc_data,
        tw_lp *sender){
    resource_lp_issue_event(header, req, 0, block_on_unavail,
            msg_size, msg_header_offset, msg_callback_offset,
            msg_callback_misc_size, msg_callback_misc_offset,
            msg_callback_misc_data, RESOURCE_GET, sender);
}

/* no callback for frees thus far */
void resource_lp_free(uint64_t req, tw_lp *sender){
    resource_lp_issue_event(NULL, req, 0, -1, -1,-1,-1, 0, 0, NULL,
            RESOURCE_FREE, sender);
}
void resource_lp_reserve(
        msg_header *header, 
        uint64_t req,
        int block_on_unavail,
        int msg_size,
        int msg_header_offset,
        int msg_callback_offset,
        int msg_callback_misc_size,
        int msg_callback_misc_offset,
        void *msg_callback_misc_data,
        tw_lp *sender){
    resource_lp_issue_event(header, req, 0, block_on_unavail, msg_size,
            msg_header_offset, msg_callback_offset, msg_callback_misc_size,
            msg_callback_misc_offset, msg_callback_misc_data, RESOURCE_RESERVE,
            sender);
}
void resource_lp_get_reserved(
        msg_header *header,
        uint64_t req,
        resource_token_t tok,
        int block_on_unavail,
        int msg_size, 
        int msg_header_offset,
        int msg_callback_offset,
        int msg_callback_misc_size,
        int msg_callback_misc_offset,
        void *msg_callback_misc_data,
        tw_lp *sender){
    resource_lp_issue_event(header, req, tok, block_on_unavail, msg_size,
            msg_header_offset, msg_callback_offset, msg_callback_misc_size,
            msg_callback_misc_offset, msg_callback_misc_data, RESOURCE_GET,
            sender);
}
void resource_lp_free_reserved(
        uint64_t req, 
        resource_token_t tok,
        tw_lp *sender){
    resource_lp_issue_event(NULL, req, tok, -1,-1,-1,-1, 0,0,NULL,
            RESOURCE_FREE, sender);
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
