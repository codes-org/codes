cfields = ( ("C_NUM_SERVERS", [1<<i for i in range(1,3)]),
            ("C_NUM_REQS",    [1,2]),
            ("C_PAYLOAD_SZ",  [1024*i for i in range(1,3)]) )

excepts = ( {"C_NUM_SERVERS" : 4, "C_PAYLOAD_SZ" : 1024}, )
