#ifndef MODELNET_QOS_H
#define MODELNET_QOS_H

#include <stddef.h>

#define NO_PACKETS_TO_SEND -1

size_t get_next_sl(size_t numSLs, int * qos_table, size_t * qos_table_index, size_t * qos_table_counter, int (*has_packets)(void*,size_t), void * packet_data);

#endif
