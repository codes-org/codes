cfields = ( ("C_NUM_SERVERS", [1<<i for i in range(1,3)]),
            ("C_NUM_REQS",    [1,2]),
            ("C_PAYLOAD_SZ",  [1024*i for i in range(1,3)]) )

# example derived paramter - make the packet size 1/4 of the payload size
cfields_derived_labels = ( "C_PACKET_SZ", )
def cfields_derived(replace_map):
    replace_map["C_PACKET_SZ"] = replace_map["C_PAYLOAD_SZ"] / 4

# example exception - don't generate configs with these sets of params
excepts = ( {"C_NUM_SERVERS" : 4, "C_PAYLOAD_SZ" : 1024}, )
