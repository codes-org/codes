#!/bin/bash
FILE=/tmp/df_AMG_n27_dumpi/dumpi-2014.03.03.14.55.00-0000.bin

if [ ! -f $FILE ]; then
       wget https://portal.nersc.gov/project/CAL/doe-miniapps-mpi-traces/AMG/df_AMG_n27_dumpi.tar.gz
       tar -xvf df_AMG_n27_dumpi.tar.gz -C /tmp
fi
