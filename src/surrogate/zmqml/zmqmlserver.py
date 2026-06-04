#!/usr/bin/env python3

#
# zmqmlserver : ZeroMQ-based ML task dispatching server
#
# Written by Kazutomo Yoshii <kazutomo.yoshii@gmail.com>
#

import zmq
import json
import threading
import sys
import time
from itertools import count # generate unit id
# from dataclasses import dataclass
import numpy as np

# TODO: abstract a mechanism to call training
from runmlpacketdelay import run_mlpacketdelay_training
from model.mliterationtime import IterationTimeModelRegistry

#import os
#model_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), "model"))
#sys.path.insert(0, model_dir)

endpoint = "tcp://*:5555"

debug = False
debug2 = False
director_debug_prints = False
#
#
#
launch_id = count(start=1) # unique for launched thread
launched_threads = {} # id:obj. keep track of active threads. remove the thread once it finished

training_records = {} # client_id:[]
iteration_time_models = IterationTimeModelRegistry(history_len=2, horizon=3)

class LaunchCMD:
    def __init__(self):
        # thread event
        self.done_ev   = threading.Event() # successfully done

    def launch(self, func, func_args):
        self.thread = threading.Thread(target=func, args=(self.done_ev, func_args))
        try:
            self.thread.start()
            self.st = time.time()
            self.id = next(launch_id)
            launched_threads[self.id] = self
        except RuntimeError as e:
            print(f"Failed to launch: {e}")
            self.id = -1

        return self.id

    def query(self):
        res = self.done_ev.is_set()
        status = "running"
        if res:
            self.thread.join()
            del launched_threads[self.id]
            if debug:
                print("thread joined")
            status = "done"

        return (status, time.time() - self.st)


def set_director_debug_prints(args):
    global director_debug_prints

    director_debug_prints = False

    # CODES sends args in the same convention as other commands:
    #   args[0] = number of real args
    #   args[1] = debug flag value
    #
    # So for set-debug, "1;0;" means one real arg whose value is 0.
    if len(args) >= 2:
        raw = str(args[1]).strip().lower()
    elif len(args) == 1:
        # Allow manual Python-client tests that send just ["0"] or ["1"].
        raw = str(args[0]).strip().lower()
    else:
        raw = "0"

    director_debug_prints = raw in ("1", "true", "yes", "on", "enabled")
    iteration_time_models.set_debug(director_debug_prints)

    if director_debug_prints:
        print(f"[zmqmlserver] director_debug_prints=1", flush=True)

    return ("done", 0.0)


def director_debug(msg):
    if director_debug_prints:
        print(msg, flush=True)

#
# launchable functions by LaunchCMD here
#
def launch_sleep(done_event, args):
    if debug:
        print("sleep started")
    time.sleep(int(args[0]))
    if debug:
        print("sleep done")
    done_event.set()

def launch_mlpacketdelay_training(done_event, args):
    if debug:
        print("mlpacketdelay_training started")

    run_mlpacketdelay_training(args)

    if debug:
        print("mlpacketdelay_training done")
    done_event.set()

    
list_nonblockingcalls = {
    "sleep": launch_sleep,
    "mlpacketdelay_training": launch_mlpacketdelay_training,
}

#
#
#
def nonblockingcall(args):
    func = args[0]   # the 1st arg is the target func
    func_args = args[1:]

    status = "failed"

    threadid = -1
    if func in list_nonblockingcalls:
        launchcmd = LaunchCMD()

        threadid = launchcmd.launch(
            list_nonblockingcalls[func],  # func
            func_args                # args
        )
        if threadid > 0:
            launched_threads[threadid] = launchcmd
            status = "done"

    return (status, threadid)

#
# define blocking-call functions here
#
def func_sleep(args):
    time.sleep(int(args[0]))
    return True

#
# register blocking call functions to list_blockingcalls
#
list_blockingcalls = {
    "sleep" : func_sleep
}

def blockingcall(args):
    func = args[0]   # the 1st arg is the target func
    func_args = args[1:]

    status = "failed"
    st = time.time()
    if func in list_blockingcalls:
        if func_sleep(func_args):
            status = "done"

    elapsed_time = time.time() - st
    return (status, elapsed_time)


#
# receive bindata
#
def receivedata(args, bindata):
    destfn = args[0]
    status = "failed"
    st = time.time()
    with open(destfn, "wb") as f:
        f.write(bindata)
        status = "done"

    elapsed_time = time.time() - st
    return (status, elapsed_time)

#
# receive training records
#
def receiverecords(args, bindata):
    status = "failed"
    st = time.time()

    num_args = int(args[0]) # 1st arg is num of args
    client = int(args[1]) # 2nd arg is client id
    num_records = int(args[2]) # 3rd arg is num records

    records_str = str(bindata.decode('utf-8'))
    records_str = records_str.strip()

    parsed_records = []
    for s in records_str.split():
        try:
            value = float(s)
        except ValueError:
            continue

        if np.isfinite(value) and value > 0.0:
            parsed_records.append(value)

    if client not in training_records:
        training_records[client] = []

    training_records[client].extend(parsed_records)

    # Keep an online per-client iteration-time predictor updated from the
    # same iteration-delta records already sent by CODES.
    if parsed_records:
        iteration_time_models.add_records(client, parsed_records)

    director_debug(
        f"[iteration-time records] client={client} "
        f"num_records_arg={num_records} accepted={len(parsed_records)} "
        f"total_records={len(training_records.get(client, []))} "
        f"values={parsed_records}"
    )

    if (debug2) and (client == 51):
        print(f"Training records[51] :{training_records[client]}")

    status = "done"
    elapsed_time = time.time() - st
    return (status, elapsed_time)


#
# do inference to get predictions
#
def launch_iteration_time_inferencing(args, bindata):
    status = "failed"
    st = time.time()

    num_args = int(args[0]) # 1st arg is num of args
    client = int(args[1]) # 2nd arg is client id
    num_steps = int(args[2]) # 3rd arg is num steps to predict

    # Optional recent-context payload. The normal path uses records previously
    # received through send-records, but accepting context keeps the API flexible.
    records_str = str(bindata.decode('utf-8'))
    records_str = records_str.strip()

    parsed_context = []
    for s in records_str.split():
        try:
            value = float(s)
        except ValueError:
            continue

        if np.isfinite(value) and value > 0.0:
            parsed_context.append(value)

    if parsed_context:
        iteration_time_models.add_records(client, parsed_context)

    inferences = iteration_time_models.predict(client, num_steps)
    inferences_str = ' '.join([str(float(f)) for f in inferences])

    model = iteration_time_models.get(client)
    director_debug(
        f"[iteration-time inference] client={client} "
        f"num_steps={num_steps} context={parsed_context} "
        f"records={len(model.records)} trained={int(model.trained)} "
        f"predictions={inferences_str}"
    )

    status = "done"
    elapsed_time = time.time() - st
    return (status, elapsed_time, inferences_str)


# Backwards-compatible wrapper for the old command name.
def launch_surrogate_inferencing(args, bindata):
    return launch_iteration_time_inferencing(args, bindata)


#

#
# main listener loop
# XXX: add mechanisms for multiple requesters
#
def zmq_cmd_listener():
    context = zmq.Context()
    socket = context.socket(zmq.REP)
    socket.bind(endpoint)

    while True:
        tmp = socket.recv()
        delimiter = b'\x00'
        msgraw, bindata = tmp.split(delimiter, 1)
        msg = json.loads(msgraw.decode('utf-8'))
        cmd = msg["cmd"]
        args = msg.get("args",[])

        if debug:
            print(f"Received cmd:{cmd} args:{args}")

        retmsg = {"status":"none"} # empty status

        if cmd == "nothing": # this cmd does nothing. to measure the latency
            retmsg = {"status":"done"}
        elif cmd == "execute":
            (status, et) = blockingcall(args)
            retmsg = {"status":status, "et":str(et)}
        elif cmd == "launch":
            (status, id) = nonblockingcall(args)
            retmsg = {"status":status, "id":str(id)}
        elif cmd == "query":
            targetid = int(args[0])
            (status, et) = launched_threads[targetid].query()
            retmsg = {"status":status, "et":str(et)}
        elif cmd == "send":
            destfn = args[0]
            (status, et) = receivedata(args, bindata)
            retmsg = {"status":status, "et":str(et)}
        elif cmd == "set-debug":
            (status, et) = set_director_debug_prints(args)
            retmsg = {"status":status, "et":str(et)}

        elif cmd == "send-records":
            (status, et) = receiverecords(args, bindata)
            retmsg = {"status":status, "et":str(et)}
        elif cmd == "iteration-time-inference":
            (status, et, predictions) = launch_iteration_time_inferencing(args, bindata)
            retmsg = {"status":status, "et":str(et), "predictions": predictions}

        elif cmd == "do-inference":
            # Backwards-compatible alias for the previous Director command.
            (status, et, predictions) = launch_surrogate_inferencing(args, bindata)
            retmsg = {"status":status, "et":str(et), "predictions": predictions}

        # send response back to the requester
        socket.send_json(retmsg)

        if cmd == "exit":
            # XXX: add codes to kill active threads
            break

#
#
#
if __name__ == "__main__":
    if debug:
        print("start zmq_cmd_listener")

    zmq_cmd_listener()

    if debug:
        print("done")
