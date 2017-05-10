## README for using ROSS instrumentation with CODES

For details about the ROSS instrumentation, see the [ROSS Instrumentation blog post](http://carothersc.github.io/ROSS/feature/instrumentation.html) 
on the ROSS webpage.
 

There are currently 3 types of instrumentation: GVT-based, real time, and event tracing.  See the ROSS documentation for more info on
the specific options or use `--help` with your model. To collect data about the simulation engine, no changes are needed to model code
for any of the instrumentation modes.  Some additions to the model code is needed in order to turn on any model-level data collection.
See the "Model-level data sampling" section on [ROSS Instrumentation blog post](http://carothersc.github.io/ROSS/feature/instrumentation.html).
Here we describe CODES specific details.

### Register Instrumentation Callback Functions

The examples here are based on the dragonfly router and terminal LPs (`src/networks/model-net/dragonfly.c`).

As described in the ROSS Vis documentation, we need to create a `st_model_types` struct with the pointer and size information.
```C
st_model_types dragonfly_model_types[] = {
    {(rbev_trace_f) dragonfly_event_collect,
    sizeof(int),
    (ev_trace_f) dragonfly_event_collect,
    sizeof(int),
    (model_stat_f) dragonfly_model_stat_collect,
    sizeof(tw_lpid) + sizeof(long) * 2 + sizeof(double) + sizeof(tw_stime) * 2},
    {(rbev_trace_f) dragonfly_event_collect,
    sizeof(int),
    (ev_trace_f) dragonfly_event_collect,
    sizeof(int),
    (model_stat_f) dfly_router_model_stat_collect,
    0}, // updated in router_setup()
    {0}
}
```
`dragonfly_model_types[0]` is the function pointers for the terminal LP and `dragonfly_model_types[1]` is for the router LP.
For the first two function pointers for each LP, we use the same `dragonfly_event_collec()` because right now we just collect the event type, so
it's the same for both of these LPs.  You can change these if you want to use different functions for different LP types or if you want a different
function for the full event tracing than that used for the rollback event trace (`rbev_trace_f` is for the event tracing of rollback triggering events only,
while `ev_trace_f` is for the full event tracing).
The number following each function pointer is the size of the data that will be saved when the function is called.
The third pointer is for the data to be sampled at the GVT or real time sampling points.
In this case the LPs have different function pointers since we want to collect different types of data for the two LP types.
For the terminal, I set the appropriate size of the data to be collected, but for the router, the size of the data is dependent on the radix for the
dragonfly configuration being used, which isn't known until runtime.

*Note*: You can only reuse the function for event tracing for LPs that use the same type of message struct.  
For example, the dragonfly terminal and router LPs both use the `terminal_message` struct, so they can
use the same functions for event tracing.  However the model net base LP uses the `model_net_wrap_msg` struct, so it gets its own event collection function and 
`st_trace_type` struct, in order to read the event type correctly from the model. 

In the ROSS instrumentation documentation, there are two methods provided for letting ROSS know about these `st_model_types` structs.  
In CODES, this step is a little different, as `codes_mapping_setup()` calls `tw_lp_settype()`.  
Instead, you add a function to return this struct for each of your LP types:
```C
static const st_model_types *dragonfly_get_model_types(void)
{
    return(&dragonfly_model_types[0]);
}
static const st_model_types *dfly_router_get_model_types(void)
{
    return(&dragonfly_model_types[1]);
}
```

Now you need to add register functions for CODES:
```C
static void dragonfly_register_model_types(st_model_types *base_type)
{
    st_model_type_register(LP_CONFIG_NM_TERM, base_type);
}

static void router_register_model_types(st_model_types *base_type)
{
    st_model_type_register(LP_CONFIG_NM_ROUT, base_type);
}
```
`st_model_type_register(const char* name, const st_trace_type* type)` is part of the CODES API and lets CODES know the pointers for LP initialization.

At this point, there are two different steps to follow depending on whether the model is one of the model-net models or not.

##### Model-net Models
In the `model_net_method` struct, two fields have been added: `mn_model_stat_register` and `mn_get_model_stat_types`.  
You need to set these to the functions described above.  For example:

```C
struct model_net_method dragonfly_method =
{
    .mn_configure = dragonfly_configure,
    // ... all the usual model net stuff
    .mn_model_stat_register = dragonfly_register_model_types,
    .mn_get_model_stat_types = dragonfly_get_model_types,
};

struct model_net_method dragonfly_router_method =
{
    .mn_configure = NULL,
    // ... all the usual model net stuff
    .mn_model_stat_register = router_register_model_types,
    .mn_get_model_stat_types = dfly_router_get_model_types,
};
```

##### All other CODES models

Using the synthetic workload LP for dragonfly as an example (`src/network-workloads/model-net-synthetic.c`).
In the main function, you call the register function *before* calling `codes_mapping_setup()`.
```C
st_model_types svr_model_types[] = {
    {(rbev_trace_f) svr_event_collect,
    sizeof(int),
    (ev_trace_f) svr_event_collect,
    sizeof(int),
    (model_stat_f) svr_model_stat_collect,
    0},  // at the moment, we're not actually collecting any data about this LP
    {0}
}

static void svr_register_model_types()
{
    st_model_type_register("server", &svr_model_types[0]);
}

int main(int argc, char **argv)
{
    // ... some set up removed for brevity
    
    model_net_register();
    svr_add_lp_type();
    
    if (g_st_ev_trace || g_st_model_stats)
        svr_register_model_types();
        
    codes_mapping_setup();
    
    //...
}
```

`g_st_ev_trace` is a ROSS flag for determining if event tracing is turned on and `g_st_model_stats` determines if the GVT-based or real time instrumentation
modes are collecting model-level data as well.  


### CODES LPs that currently have event type collection implemented:
If you're using any of the following CODES models, you don't have to add anything, unless you want to change the data that's being collected.
- nw-lp (model-net-mpi-replay.c)
- original dragonfly router and terminal LPs (dragonfly.c)
- dfly server LP (model-net-synthetic.c)
- model-net-base-lp (model-net-lp.c)
- fat tree server LP (model-net-synthetic-fattree.c)
 
The fat-tree terminal and switch LPs (fattree.c) are only partially implemented at the moment.  It needs two `model_net_method` structs to fully implement, 
but currently both terminal and switch LPs use the same `fattree_method` struct.
