#!/bin/bash
FILE=/tmp/df_AMG_n27_dumpi/dumpi-2014.03.03.14.55.00-0000.bin

if [ ! -f $FILE ]; then
       wget https://raw.githubusercontent.com/codes-org/codes-files/master/tests/df_AMG_n27_dumpi.tar.gz
       tar -xvf df_AMG_n27_dumpi.tar.gz -C /tmp
fi
