#!/bin/bash

network_size=1056

for alloc in 128 256 512 1024 2048 4096 6144 8192 10240 12288 14336 16512 # job allocation size 
  do
    # generate config file with job allocation 
#    if [! -f config_alloc_$alloc.conf];
#    then 
#        echo "Creating config file "
        printf "rand\n$network_size\n$alloc" >> config_alloc_$alloc.conf
        # now generate allocation file 
        python listgen.py config_alloc_$alloc.conf
#    fi
#    if [! -f workload_$alloc.conf];
#    then 
       echo "Creating workload file "
    # generate the workload file 
    printf "$alloc collective" >> workload_$alloc.conf
#    fi
    
    #for routing in ['minimal','nonminimal','adaptive']
    for routing in "minimal" "nonminimal" "adaptive"
  do
      for data_size in 1024 1048576 # 1KB, 1MB
        do
            for algo in 0 1 2 3 # TREE, LLF, GLF, FOREST
            do 
                    # now do 5 iterations of simulation 
                    for iter in {1..5}
                     do
                    echo "./src/network-workloads/model-net-mpi-replay --sync=1
                    --bcast_size=$data_size --algo_type=$algo
                    --alloc_file=allocation.conf --workload_type=cortex-workload
                    --workload_conf_file=workload_$alloc.conf
                    ../src/network-workloads/cortex-conf/modelnet-mpi-test-dragonfly-$network_size-$routing.conf
                    >> dragonfly-$network_size-$bcast_size-$routing-$algo.out"
                    done
          done
    done
  done 
done 

#$network_size = 5616
#
#for routing in ['minimal','nonminimal','adaptive']
# for data_size in [1024,1048576] # 1KB, 1MB
#  for algo in [0,1,2,3] # TREE, LLF, GLF, FOREST
#   for alloc in [128,256,512,1024,2*1024,3*1024,4*1024]
#    for bg_traffic in [false,true]
#     run_exp($network_size,routing,data_size,alloc,algo,bg_traffic)
#    end
#   end
#  end
# end
#end
