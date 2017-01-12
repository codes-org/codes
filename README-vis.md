## README for using ROSS instrumentation in CODES

For details about the ROSS instrumentation, see the [ROSS Instrumentation blog post](http://carothersc.github.io/ROSS/feature/instrumentation.html) on the ROSS webpage.
The instrumentation will be merged into the master branch of the ROSS repo very soon.  

There are currently 3 types of instrumentation: GVT-based, real time, and event tracing.  See the ROSS documentation for more info on
the specific options or use `--help` with your model.  The GVT-based and real time sampling do not require any changes to your model code.
The event tracing will run without any changes, but some additions to the model code is needed in order to get specific model event types.  
This document describes how to do it.

### Register LP event tracing function

The examples here are based on the server LP for the synthetic workload generation for dragonfly (`src/network-workloads/model-net-synthetic.c`).

As described in the ROSS Vis documentation, we need to first add our function that will save the event type (and any other desired data) to the
buffer location provided by ROSS.
```C
void svr_event_collect(svr_msg *m, tw_lp *lp, char *buffer)
{
    int type = (int) m->svr_event_type;
    memcpy(buffer, &type, sizeof(type));
}
```

Then we need to create a `st_trace_type` struct with the pointer and size information.
```C
st_trace_type svr_trace_types[] = {
    {(rbev_trace_f) svr_event_collect,
    sizeof(int),
    (ev_trace_f) svr_event_collect,
    sizeof(int)},
    {0}
}
```

And a function to return this struct
```C
static const st_trace_type *svr_get_trace_types(void)
{
    return(&svr_trace_types[0]);
}
```

As a reminder, there are two types of event tracing the full event trace (`ev_trace_f`) or only events that trigger rollbacks (`rbev_trace_f`).  
It's set up so that you can have different functions for both types of event tracing, or you can use the same function for both.
Immediately after each function pointer is a `size_t` type that takes the amount of data that the function will be placing in the buffer,
so ROSS can appropriately handle things.

If you have multiple LPs, you can do a `st_trace_type` for each LP, or you can reuse.  *Note*: You can only reuse `st_trace_type` and the event type collection
function for LPs that use the same type of message struct.  For example, the dragonfly terminal and router LPs both use the `terminal_message` struct, so they can
use the same functions for event tracing.  However the model net base LP uses the `model_net_wrap_msg` struct, so it gets its own event collection function and 
`st_trace_type` struct, in order to read the event type correctly from the model.  

`codes_mapping_init()` was changed to register the function pointers when it is setting up the LP types.  So for CODES models, you need to add a register function:
```C
void svr_register_trace()
{
    trace_type_register("server", svr_get_trace_types());
}
```

`trace_type_register(const char* name, const st_trace_type* type)` is part of the API and lets CODES know the pointers for LP initialization.

Now in the main function, you call the register function *before* calling `codes_mapping_setup()`.
```C
if (g_st_ev_trace)
    svr_register_trace();
```

`g_st_ev_trace` is a ROSS flag for determining if event tracing is turned on.  

That's all you need to add for each LP.  

### Model Net LPs
In addition to the dragonfly synthetic server LP, I've already added in the necessary changes for both the model net base LP type and dragonfly (both router and terminal LPs),
so no other changes need to be made to those LPs. (Unless you want to collect some additional data.) 
For any other network LPs that are based on the model net base LP type, there are a few additional details to know.
There are two fields added to the `model_net_method` struct for pointers to the trace registration functions for each LP.

```C
void (*mn_trace_register)(st_trace_type *base_type);
const st_trace_type* (*mn_get_trace_type)();
```

For example, right now, both the dragonfly router and terminal LPs use the same `st_trace_type dragonfly_trace_types` struct and the following function to return its pointer:
```C
static const st_trace_type *dragonfly_get_trace_types(void)
{
    return(&dragonfly_trace_types[0]);
}
```

They have different register functions:
```C
static void dragonfly_register_trace(st_trace_type *base_type)
{
    trace_type_register(LP_CONFIG_NM_TERM, base_type);
}

static void router_register_trace(st_trace_type *base_type)
{
    trace_type_register(LP_CONFIG_NM_ROUT, base_type);
}
```

And then the following additions to their `model_net_method` structs:
```C
struct model_net_method dragonfly_method =
{
    // the fields already in the struct
    ...
    // event tracing additions
    .mn_trace_register = dragonfly_register_trace,
    .mn_get_trace_type = dragonfly_get_trace_types,
};

struct model_net_method dragonfly_router_method =
{
    // the fields already in the struct
    ...
    // event tracing additions
    .mn_trace_register = router_register_trace,
    .mn_get_trace_type = dragonfly_get_trace_types,
};
```

Any other LPs built off of the model net LP, can be changed in the same way.


### CODES LPs that have event type collection implemented:
- nw-lp (model-net-mpi-replay.c)
- original dragonfly router and terminal LPs (dragonfly.c)
- dfly server LP (model-net-synthetic.c)
- fat tree terminal and switch LPs (fattree.c)
- model-net-base-lp (model-net-lp.c)
