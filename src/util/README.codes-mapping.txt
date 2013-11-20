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

Step 5: Querying number of LPs in a repetition

int codes_mapping_get_lp_count(group_name, lp_type_name);

For example, to query the number of server LPs in example-test2.conf file, calling

num_servers = codes_mapping_get_lp_count("MODELNET_GRP", "server");
 
will return 2.

Step 6: Querying number of repetitions in a particular group, use

int codes_mapping_get_group_reps(group_name);

For example, to query the number of repetitions in example-test2.conf file, calling

num_repetitions = codes_mapping_get_group_reps("MODELNET_GRP");

will return 16.
