#!/usr/bin/env python

#
# pyrequester : a requester sample implementation to ZeroMQ-based ML
# task dispatching server
#
# Written by Kazutomo Yoshii <kazutomo.yoshii@gmail.com>
#

import zmq
import json
import time
import numpy as np
import sys
import re

debug = False # XXX: make this argument

endpoint = "tcp://localhost:5555" # XXX: make this configurable

def zmqml_request(cmd, args=None, bindata=b"None"):
    """
    Sends a command to a specified endpoint using ZeroMQ and waits for a response.

    :param cmd: the command to be sent.
    :type cmd: str
    :param args: A list of arguments for the command where the first argument is the function name. Defaults to None.
    :type args: list, optional
    :return: A tuple containing the results extracted from the response and the elapsed time in seconds.
    :rtype: tuple
    :raises zmq.ZMQError: Raises an exception if there is an issue with the ZeroMQ communication.

    Example usage:
    >>> zmqml_request("execute", ["mlpacketdelay", "param1", "param2"])
    """

    context = zmq.Context()
    socket = context.socket(zmq.REQ)
    socket.connect(endpoint)

    # the first arg in args is the function name (e.g., mlpacketdelay)
    msg = {"cmd":cmd, "args":args}
    msgencoded = json.dumps(msg).encode('utf-8')

    delimiter = b'\x00'
    payload = msgencoded + delimiter + bindata
    socket.send(payload)

    response = socket.recv_json()
    status = response["status"]
    if debug:
        print("status:", status)

    socket.close()

    return response

#
#
def measure_latency():
    print("* measure_latency")
    tss = []
    n = 1000
    for i in range(0,n):
        st = time.time()
        zmqml_request("nothing") # blocking
        tss.append(time.time() - st)
    print('zmqcmd latency:', np.mean(tss), np.std(tss))

#
#
def test_blocking_sleep():
    print("* test_blocking_sleep")

    target = ["sleep", "1"] # this works like args to main() in C

    ret = zmqml_request("execute", target) # blocking
    print(f'status={ret["status"]} et={ret["et"]}')
    print("done")

#
#
def test_nonblocking_sleep():
    print("* test_nonblocking_sleep")

    target = ["sleep", "2"]

    ret = zmqml_request("launch", target)
    status = ret["status"]
    id = ret["id"]
    print(f'status={status} id={id}')

    cnt = 0
    while True:
        ret = zmqml_request("query", [id])
        status = ret["status"]
        print(f"status={status}")
        if status == "done":
            break
        time.sleep(.5)
        cnt = cnt + 1
    print(f"done cnt={cnt}")


#
#
def test_mlpacketdelay_training():
    print("* test_mlpacketdelay_training")

    target = ["mlpacketdelay_training", 
              "--method", "MLP", "--epoch", "1",
              "--input-file", "model/packets-delay.csv",
              "--model-path", "ml-model.pt"]
    
    ret = zmqml_request("launch", target)
    status = ret["status"]
    id = ret["id"]
    print(f'status={status} id={id}')

    cnt = 0
    while True:
        ret = zmqml_request("query", [id])
        status = ret["status"]
        print(f"status={status}")
        if status == "done":
            break
        time.sleep(.5)
        cnt = cnt + 1
    print(f"done cnt={cnt}")
    
#
#
def test_send_binary():
    print("* test_send_binary")

    data = b""
    with open('model/ml-model.pt', 'rb') as f:
        data = f.read()
    
    ret = zmqml_request("send", ["tmptestsend.dat"], data)
    status = ret["status"]
    print(f"status={status}")

    
if __name__ == "__main__":
    test_mlpacketdelay_training()
    
    test_send_binary()
    test_blocking_sleep()
    test_nonblocking_sleep()
    measure_latency()

    zmqml_request("exit")
    sys.exit(0)
