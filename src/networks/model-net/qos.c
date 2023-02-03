#include "qos.h"

static void to_next_sl(int numSLs, size_t * qos_table_index, size_t * qos_table_counter) {
    ++(*qos_table_index);
    if (*qos_table_index >= numSLs) {
        *qos_table_index = 0;
    }
    *qos_table_counter = 0;
}

size_t get_next_sl(size_t numSLs, int * qos_table, size_t * qos_table_index, size_t * qos_table_counter, int (*has_packets)(void*,size_t), void * packet_data) {
    for (size_t i = 0; i < numSLs; ++i) {
        size_t sl = *qos_table_index;
        if (has_packets(packet_data, sl)) {
            // Advance the QoS table index and counter
            ++(*qos_table_counter);

            if (*qos_table_counter >= qos_table[sl]) {
                to_next_sl(numSLs, qos_table_index, qos_table_counter);
            }

            // Perform the send
            return sl;
        }
        else {
            // The queue is empty, so advance to the next queue
            to_next_sl(numSLs, qos_table_index, qos_table_counter);
        }
    }
    return NO_PACKETS_TO_SEND;
}
