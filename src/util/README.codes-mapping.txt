Codes Mapping API Overview
==========================

- The codes-mapping.h header file contains the function definitions for the codes LP mapping.

Step 1: Specifying the LP types in the config file:

- Here is an example of a config file that specifies the codes LP mapping
-------------------------------example-test.conf-----------------------------
LPGROUPS
{
   MODELNET_GRP
   {
      repetitions="16";
      server="1";
      example_net="1";
   }
}
------------------------------------------------------------------------------
In this config file, there are multiple LP types defined in a single LP group (As we will see in a later
example, there can be multiple LP groups in a config file too). There is 1 server LP and 1 example_net
LP type in a group and this combination is repeated 16 time (repetitions="16"). ROSS will assign the 
LPs to the PEs (PEs is an abstraction for MPI rank in ROSS) by placing 1 server LP then 1 example_net
LP a total of 16 times. This configuration is useful if there is some form of communication involved
between the server and example_net LP types, in which case ROSS will place them on the same PE and 
communication between server and example_net LPs will not involve remote messages. 
The number of server and example_net LPs can be more than 1. Lets assume if we have two example_net
LPs for each server then the config file will have the following format:

-------------------------------example-test2.conf-----------------------------
LPGROUPS
{
   MODELNET_GRP
   {
      repetitions="16";
      server="1";
      example_net="2";
   }
}
------------------------------------------------------------------------------

Step 2: Loading the config file in the model:

After the initialization function calls of ROSS (tw_init), the configuration file can be loaded in the
example program using:

configuration_load(example-test.conf, MPI_COMM_WORLD, &config);

Step 3: Each LP type must register itself  with the lp_type_register before setting up the codes-mapping. 
Following is an example function that registers 'server' lp type.

static void svr_add_lp_type()
{
  lp_type_register("server", svr_get_lp_type());
}

Step 4: Call to codes_mapping_setup that sets up the LPs of each registered LP type as specified in the config
file.

codes_mapping_setup();

Step 5: Querying number of LPs in a group/repetition.

int codes_mapping_get_lp_count(group_name, 1, lp_type_name, NULL, 0);

The second argument indicates whether to include the number of repetitions into
the count or not (the NULL, 0 arguments will be discussed later). For example,
to query the number of server LPs in example-test2.conf file, calling

num_servers = codes_mapping_get_lp_count("MODELNET_GRP", 1, "server", NULL, 0);
 
will return 2.

Step 6: Querying number of repetitions in a particular group, use

int codes_mapping_get_group_reps(group_name);

For example, to query the number of repetitions in example-test2.conf file, calling

num_repetitions = codes_mapping_get_group_reps("MODELNET_GRP");

will return 16.

=== LP to PE mapping for parallel simulations ===

In the case of parallel simulations using MPI, the LP mapping explained in Step
1 still holds. However, these LPs must also be mapped to PEs, which can be an
arbitrary mapping in ROSS. We simply assign the first N LPs to the first PE, the
second N to the second PE, and so forth, where N is the floor of the LP count
and the PE count. If the number of LPs is not divisible by the number of PEs,
then the first N+1 LPs are mapped to the first PE and so on, until the remainder
has been taken care of.

=== Namespaces supported by codes mapping API ===
The configuration scheme suppports "annotation"-based specifications. For
example:

-------------------------------------------------------------------------------
LPGROUPS
{
   GROUP_1
   {
      repetitions="16";
      server@foo="1";
      example_net@foo="1";
   }

   SWITCH
   {
      repetitions="1";
      example_net@foo="1";
      example_net@bar="1";
   }

   GROUP_2
   {
      repetitions="16";
      server@bar="1";
      example_net@bar="1";
   }
}

PARAMS
{
   net_bandwidth@foo="100";
   net_bandwidth@bar="50";
}
-------------------------------------------------------------------------------

In this example, the example_net LP will be configured corresponding to their
annotations "foo" and "bar". Not only does this allow specialization of
otherwise identical models, this also presents the opportunity to provide
namespace functionality to the mapping API.

Current namespace support includes (see codes_mapping.h for more details)
- Global (e.g., LPs). Additional specializations for doing lookups corresponding
  to a specific annotation, to support use cases such as the two example_net's
  in the SWITCH group.
- LP-specific namespace (0..N) where N is the number of LPs of a specific type
  (codes_mapping_get_lp_relative_id and codes_mapping_get_lpid_from_relative)
  - relative IDs can be with respect to a particular group, a particular
    annotation, both, or neither
