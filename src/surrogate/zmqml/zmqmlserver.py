#!/usr/bin/env python

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

# TODO: abstract a mechanism to call training
from runmlpacketdelay import run_training

#import os
#model_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), "model"))
#sys.path.insert(0, model_dir)

endpoint = "tcp://*:5555"

debug = False

#
#
#
launch_id = count(start=1) # unique for launched thread
launched_threads = {} # id:obj. keep track of active threads. remove the thread once it finished

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

#
# launchable functions by LaunchCMD here
#
def launch_sleep(done_event, args):
    if debug:
        print("Dummy started")
    time.sleep(int(args[0]))
    if debug:
        print("Dummy done")
    done_event.set()

list_nonblockingcalls = {
    "sleep": launch_sleep
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
            retmsg = {"status":status, "et":et}
        elif cmd == "launch":
            (status, id) = nonblockingcall(args)
            retmsg = {"status":status, "id":id}
        elif cmd == "query":
            targetid = args[0]
            (status, et) = launched_threads[targetid].query()
            retmsg = {"status":status, "et":et}
        elif cmd == "send":
            destfn = args[0]
            (status, et) = receivedata(args, bindata)
            retmsg = {"status":status, "et":et}

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
