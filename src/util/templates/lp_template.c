/*
 * Copyright (C) 2014 University of Chicago.
 * See COPYRIGHT notice in top-level directory.
 *
 */

/* This is meant to be a template file to use when developing new LPs. 
 * Roughly follows the format of the existing LPs in the CODES repos */

#include "lp_template.h"
#include "codes/codes_mapping.h"
#include "codes/lp-type-lookup.h"
#include "codes/jenkins-hash.h"
#include "codes/codes.h"

/**** BEGIN SIMULATION DATA STRUCTURES ****/

static int template_magic; /* use this as sanity check on events */

typedef struct template_state template_state;
typedef struct template_msg template_msg;

/* event types */
enum template_event
{
    TEMPLATE_A,
    TEMPLATE_B,
};

struct template_state {
};

struct template_msg {
    enum template_event event_type;
    int magic;
};

/**** END SIMULATION DATA STRUCTURES ****/

/**** BEGIN LP, EVENT PROCESSING FUNCTION DECLS ****/

/* ROSS LP processing functions */  
static void template_lp_init(
    template_state * ns,
    tw_lp * lp);
static void template_event_handler(
    template_state * ns,
    tw_bf * b,
    template_msg * m,
    tw_lp * lp);
static void template_rev_handler(
    template_state * ns,
    tw_bf * b,
    template_msg * m,
    tw_lp * lp);
static void template_finalize(
    template_state * ns,
    tw_lp * lp);

/* event type handlers */
static void handle_template_a(
    template_state * ns,
    template_msg * m,
    tw_lp * lp);
static void handle_template_b(
    template_state * ns,
    template_msg * m,
    tw_lp * lp);
static void handle_template_a_rev(
    template_state * ns,
    template_msg * m,
    tw_lp * lp);
static void handle_template_b_rev(
    template_state * ns,
    template_msg * m,
    tw_lp * lp);

/* ROSS function pointer table for this LP */
tw_lptype template_lp = {
    (init_f) template_lp_init,
    (pre_run_f) NULL,
    (event_f) template_event_handler,
    (revent_f) template_rev_handler,
    (final_f)  template_finalize, 
    (map_f) codes_mapping,
    sizeof(template_state),
};

/**** END LP, EVENT PROCESSING FUNCTION DECLS ****/

/**** BEGIN IMPLEMENTATIONS ****/

void template_init(){
    uint32_t h1=0, h2=0;

    bj_hashlittle2("template", strlen("template"), &h1, &h2);
    template_magic = h1+h2;

    lp_type_register("template", &template_lp);
}

void template_lp_init(
        template_state * ns,
        tw_lp * lp){
    /* Fill me in... */
}

void template_event_handler(
        template_state * ns,
        tw_bf * b,
        template_msg * m,
        tw_lp * lp){
    assert(m->magic == template_magic);
    
    switch (m->event_type){
        case TEMPLATE_A:
            handle_template_a(ns, m, lp);
            break;
        case TEMPLATE_B:
            handle_template_b(ns, m, lp);
            break;
        /* ... */
        default:
            assert(!"template event type not known");
            break;
    }
}

void template_rev_handler(
        template_state * ns,
        tw_bf * b,
        template_msg * m,
        tw_lp * lp){
    assert(m->magic == template_magic);
    
    switch (m->event_type){
        case TEMPLATE_A:
            handle_template_a_rev(ns, m, lp);
            break;
        case TEMPLATE_B:
            handle_template_b_rev(ns, m, lp);
            break;
        /* ... */
        default:
            assert(!"template event type not known");
            break;
    }
}

void template_finalize(
        template_state * ns,
        tw_lp * lp){
    /* Fill me in... */
}

/* event type handlers */
void handle_template_a(
    template_state * ns,
    template_msg * m,
    tw_lp * lp){

}
void handle_template_b(
    template_state * ns,
    template_msg * m,
    tw_lp * lp){

}
void handle_template_a_rev(
    template_state * ns,
    template_msg * m,
    tw_lp * lp){

}
void handle_template_b_rev(
    template_state * ns,
    template_msg * m,
    tw_lp * lp){

}

/**** END IMPLEMENTATIONS ****/

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ft=c ts=8 sts=4 sw=4 expandtab
 */
