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
from model.mleventtime import EventTimeModel
import csv
from pathlib import Path

import os
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
iteration_time_models = IterationTimeModelRegistry(
    history_len=int(os.environ.get("ZMQML_ITERATION_HISTORY_LEN", "4")),
    horizon=int(os.environ.get("ZMQML_ITERATION_HORIZON", "30")),
    ridge_alpha=float(os.environ.get("ZMQML_ITERATION_RIDGE_ALPHA", "1.0")),
    train_stride=int(os.environ.get("ZMQML_ITERATION_TRAIN_STRIDE", "3")),
)
event_time_model = EventTimeModel(
    min_rows=int(os.environ.get("ZMQML_EVENT_TIME_MIN_ROWS", "32")),
    max_epochs=int(os.environ.get("ZMQML_EVENT_TIME_EPOCHS", "80")),
    lr=float(os.environ.get("ZMQML_EVENT_TIME_LR", "0.001")),
    hidden_dim=int(os.environ.get("ZMQML_EVENT_TIME_HIDDEN_DIM", "64")),
)
iteration_model_path = os.environ.get("ZMQML_ITERATION_MODEL_PATH", "").strip()
event_time_model_path = os.environ.get("ZMQML_EVENT_TIME_MODEL_PATH", "").strip()
event_time_record_log_path = os.environ.get("ZMQML_EVENT_TIME_RECORD_LOG_PATH", "").strip()
record_log_path = os.environ.get("ZMQML_RECORD_LOG_PATH", "").strip()
record_format = os.environ.get("ZMQML_RECORD_FORMAT", "app_id,client,iteration,value").strip()
app_alloc_path = os.environ.get("ZMQML_APP_ALLOC_PATH", "").strip()

auto_train_on_records = os.environ.get(
    "ZMQML_AUTO_TRAIN_ON_RECORDS", "1"
).strip().lower() in ("1", "true", "yes", "on")
event_time_auto_train_on_records = os.environ.get(
    "ZMQML_EVENT_TIME_AUTO_TRAIN_ON_RECORDS", "0"
).strip().lower() in ("1", "true", "yes", "on")

iteration_model_version = 0
event_time_model_version = 0

EVENT_TIME_RECORD_HEADER = (
    "schema_version,sample_id,now,current_lp_gid,current_lp_type,"
    "current_event_type,target_lp_gid,target_lp_type,target_event_type,"
    "nominal_delay,target_delay,backend\n"
)


if iteration_model_path:
    iteration_time_models.load(iteration_model_path)
    iteration_model_version = 1
    print(
        f"[zmqmlserver] loaded iteration-time model: {iteration_model_path}",
        flush=True,
    )

if event_time_model_path:
    event_time_model.load(event_time_model_path)
    event_time_model_version = 1
    print(
        f"[zmqmlserver] loaded event-time model: {event_time_model_path}",
        flush=True,
    )


def load_client_app_map_from_alloc(path: str) -> dict[int, int]:
    """Load a Union-style allocation file as client_id -> app_id.

    The current Union mixed workload allocation file has one line per app:

        line 0: clients/nodes assigned to app 0
        line 1: clients/nodes assigned to app 1

    This lets us enrich records from:

        client,value

    to:

        app_id,client,iteration,value
    """
    client_to_app: dict[int, int] = {}

    if not path:
        return client_to_app

    try:
        with open(path, "r") as f:
            for app_id, line in enumerate(f):
                for token in line.split():
                    try:
                        client = int(token)
                    except ValueError:
                        continue

                    if client not in client_to_app:
                        client_to_app[client] = app_id
    except FileNotFoundError:
        print(
            f"[zmqmlserver] warning: ZMQML_APP_ALLOC_PATH not found: {path}",
            flush=True,
        )
    except Exception as exc:
        print(
            f"[zmqmlserver] warning: failed to read ZMQML_APP_ALLOC_PATH={path}: {exc}",
            flush=True,
        )

    return client_to_app


client_app_map = load_client_app_map_from_alloc(app_alloc_path)
client_iteration_counts: dict[int, int] = {}


def record_log_header() -> list[str]:
    if record_format == "app_id,client,iteration,value":
        return ["app_id", "client", "iteration", "value"]

    return ["client", "value"]


def format_record_log_row(client: int, value: float) -> list[object]:
    client = int(client)
    value = float(value)

    if record_format == "app_id,client,iteration,value":
        iteration = client_iteration_counts.get(client, 0)
        client_iteration_counts[client] = iteration + 1

        app_id = client_app_map.get(client, -1)

        return [app_id, client, iteration, value]

    return [client, value]


def append_record_log(client: int, values: list[float]) -> None:
    if not record_log_path or not values:
        return

    out_path = Path(record_log_path)
    first_write = not out_path.exists()

    with out_path.open("a", newline="") as f:
        writer = csv.writer(f)
        if first_write:
            writer.writerow(record_log_header())
        for value in values:
            writer.writerow(format_record_log_row(client, value))

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
    event_time_model.set_debug(director_debug_prints)

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

    # Keep the raw records available for offline/pretraining workflows.
    # By default this preserves the old behavior and trains immediately.
    # Set ZMQML_AUTO_TRAIN_ON_RECORDS=0 for pure-PDES collection or
    # frozen pretrained inference runs.
    if parsed_records:
        append_record_log(client, parsed_records)
        model = iteration_time_models.get(client)

        # Enrich the ML model with app_id metadata when available.
        # The C++ protocol still sends client + timing values, while the Python
        # server infers app_id from ZMQML_APP_ALLOC_PATH.
        app_id = client_app_map.get(client, -1) if "client_app_map" in globals() else -1

        if hasattr(model, "set_app_id"):
            model.set_app_id(app_id)

        model.add_records(parsed_records)

        if hasattr(iteration_time_models, "set_client_app_id"):
            iteration_time_models.set_client_app_id(client, app_id)

        if auto_train_on_records:
            model.train_or_update()

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

    if parsed_context and director_debug_prints:
        print(
            f"[iteration-time inference] ignoring context in frozen/read-only inference "
            f"client={client} context={parsed_context}",
            flush=True,
        )

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






def event_time_payload_has_header(payload: str) -> bool:
    for line in payload.splitlines():
        line = line.strip()
        if not line:
            continue
        return line.startswith("schema_version,")
    return False


def event_time_payload_with_header(payload: str) -> str:
    payload = payload.strip()
    if not payload:
        return ""

    if event_time_payload_has_header(payload):
        return payload + ("\n" if not payload.endswith("\n") else "")

    return EVENT_TIME_RECORD_HEADER + payload + ("\n" if not payload.endswith("\n") else "")


def append_event_time_record_log(payload: str) -> None:
    if not event_time_record_log_path or not payload.strip():
        return

    out_path = Path(event_time_record_log_path)
    if out_path.parent:
        out_path.parent.mkdir(parents=True, exist_ok=True)

    file_has_content = out_path.exists() and out_path.stat().st_size > 0
    payload_has_header = event_time_payload_has_header(payload)

    lines = payload.strip().splitlines()

    # Avoid repeated headers in the server-owned CSV.
    if payload_has_header and file_has_content and lines:
        lines = lines[1:]

    with out_path.open("a") as f:
        if not file_has_content and not payload_has_header:
            f.write(EVENT_TIME_RECORD_HEADER)

        for line in lines:
            line = line.strip()
            if not line:
                continue
            f.write(line)
            f.write("\n")


def receive_event_time_records(args, bindata):
    st = time.time()

    raw_payload = bindata.decode("utf-8", errors="replace").strip()
    payload = event_time_payload_with_header(raw_payload)
    loaded_rows = event_time_model.add_records_text(payload) if payload else 0

    if loaded_rows > 0:
        append_event_time_record_log(raw_payload)
        if event_time_auto_train_on_records:
            event_time_model.train_or_update()

    director_debug(
        f"[event-time records] loaded_rows={loaded_rows} "
        f"total_rows={len(event_time_model.rows)} "
        f"trained={int(event_time_model.trained)}"
    )

    return ("done", time.time() - st, loaded_rows)


def load_event_time_records_csv_command(args):
    st = time.time()
    real_args = _real_command_args(args)

    if not real_args:
        return {
            "status": "failed",
            "et": str(time.time() - st),
            "error": "missing CSV path or directory",
        }

    path = Path(real_args[0])
    if not path.exists():
        return {
            "status": "failed",
            "et": str(time.time() - st),
            "error": f"event-time records path does not exist: {path}",
        }

    loaded_rows = event_time_model.load_csv(path)

    return {
        "status": "done",
        "et": str(time.time() - st),
        "path": str(path),
        "loaded_rows": str(loaded_rows),
        "total_rows": str(len(event_time_model.rows)),
    }


def train_event_time_model_command(args):
    global event_time_model_version

    st = time.time()
    trained = event_time_model.train_or_update()

    if trained:
        event_time_model_version += 1

    ret = {
        "status": "done" if trained else "failed",
        "et": str(time.time() - st),
        "model_version": str(event_time_model_version),
    }
    ret.update(event_time_model.status())

    if not trained:
        ret["error"] = "event-time model was not trained; load enough generic event-time rows first"

    print(
        f"[event-time model-train-command] trained={int(trained)} "
        f"rows={len(event_time_model.rows)} "
        f"training_examples={event_time_model.training_examples} "
        f"model_version={event_time_model_version}",
        flush=True,
    )

    return ret


def save_event_time_model_command(args):
    st = time.time()
    real_args = _real_command_args(args)

    if not real_args:
        return {
            "status": "failed",
            "et": str(time.time() - st),
            "error": "missing output model path",
        }

    model_path = Path(real_args[0])
    if model_path.parent:
        model_path.parent.mkdir(parents=True, exist_ok=True)

    event_time_model.save(model_path)

    return {
        "status": "done",
        "et": str(time.time() - st),
        "path": str(model_path),
        "model_version": str(event_time_model_version),
    }


def load_event_time_model_command(args):
    global event_time_model_version

    st = time.time()
    real_args = _real_command_args(args)

    if not real_args:
        return {
            "status": "failed",
            "et": str(time.time() - st),
            "error": "missing input model path",
        }

    model_path = Path(real_args[0])
    if not model_path.exists():
        return {
            "status": "failed",
            "et": str(time.time() - st),
            "error": f"model path does not exist: {model_path}",
        }

    event_time_model.load(model_path)
    event_time_model_version += 1

    return {
        "status": "done",
        "et": str(time.time() - st),
        "path": str(model_path),
        "model_version": str(event_time_model_version),
    }


def event_time_model_status_command(args):
    st = time.time()
    ret = {
        "status": "done",
        "et": str(time.time() - st),
        "model_version": str(event_time_model_version),
    }
    ret.update(event_time_model.status())
    return ret


def launch_event_time_inferencing(args, bindata):
    st = time.time()

    real_args = _real_command_args(args)
    requested_count = 1
    if real_args:
        try:
            requested_count = int(real_args[-1])
        except Exception:
            requested_count = 1

    payload = bindata.decode("utf-8", errors="replace").strip()
    predictions = event_time_model.predict_from_text(
        payload,
        requested_count=max(1, requested_count),
    )
    predictions_str = " ".join(str(float(x)) for x in predictions)

    director_debug(
        f"[event-time inference] requested_count={requested_count} "
        f"payload_bytes={len(payload)} predictions={predictions_str}"
    )

    return ("done", time.time() - st, predictions_str)

def _real_command_args(args):
    if args is None:
        return []

    out = [str(a) for a in args]

    # CODES often sends ["N", arg1, arg2, ...]. Manual tools may send
    # [arg1, arg2, ...]. Accept both.
    if out:
        try:
            n = int(out[0])
            if n == len(out) - 1:
                return out[1:]
        except ValueError:
            pass

    return out


def train_iteration_time_model_command(args):
    global iteration_model_version

    st = time.time()
    real_args = _real_command_args(args)

    target = real_args[0] if real_args else "all"

    if target not in ("", "all", "*"):
        return {
            "status": "failed",
            "et": str(time.time() - st),
            "error": (
                "rich iteration-time model is global; "
                "train with target=all"
            ),
        }

    client_ids = sorted(iteration_time_models.models.keys())
    total_clients = len(client_ids)
    total_records = sum(
        len(iteration_time_models.get(client_id).records)
        for client_id in client_ids
    )

    trained = iteration_time_models.train_or_update()

    if trained:
        iteration_model_version += 1

    trained_clients = total_clients if trained else 0
    skipped_clients = 0 if trained else total_clients
    status = "done" if trained else "failed"

    ret = {
        "status": status,
        "et": str(time.time() - st),
        "target": "all",
        "total_clients": str(total_clients),
        "trained_clients": str(trained_clients),
        "skipped_clients": str(skipped_clients),
        "total_records": str(total_records),
        "training_examples": str(getattr(iteration_time_models, "training_examples", "")),
        "model_version": str(iteration_model_version),
    }

    if status != "done":
        ret["error"] = (
            "global model was not trained; check that records contain at least "
            "history_len + horizon values per client"
        )

    print(
        "[iteration-time model-train-command] "
        f"target=all total_clients={total_clients} "
        f"trained={int(trained)} total_records={total_records} "
        f"training_examples={getattr(iteration_time_models, 'training_examples', '')} "
        f"model_version={iteration_model_version}",
        flush=True,
    )

    return ret

def save_iteration_time_model_command(args):
    st = time.time()
    real_args = _real_command_args(args)

    if not real_args:
        return {
            "status": "failed",
            "et": str(time.time() - st),
            "error": "missing output model path",
        }

    model_path = real_args[0]
    out_path = Path(model_path)
    if out_path.parent:
        out_path.parent.mkdir(parents=True, exist_ok=True)

    iteration_time_models.save(str(out_path))

    print(
        f"[iteration-time model-save-command] path={out_path} "
        f"model_version={iteration_model_version}",
        flush=True,
    )

    return {
        "status": "done",
        "et": str(time.time() - st),
        "path": str(out_path),
        "model_version": str(iteration_model_version),
    }



def load_iteration_records_csv_command(args):
    st = time.time()
    real_args = _real_command_args(args)

    if not real_args:
        return {
            "status": "failed",
            "et": str(time.time() - st),
            "error": "missing CSV path",
        }

    csv_path = Path(real_args[0])
    if not csv_path.exists():
        return {
            "status": "failed",
            "et": str(time.time() - st),
            "error": f"CSV path does not exist: {csv_path}",
        }

    loaded_rows = 0
    loaded_clients = set()

    with csv_path.open(newline="") as f:
        reader = csv.DictReader(f)
        fieldnames = set(reader.fieldnames or [])

        if {"client", "value"}.issubset(fieldnames):
            for row in reader:
                try:
                    client = int(row["client"])
                    value = float(row["value"])
                except Exception:
                    continue

                if not np.isfinite(value) or value <= 0.0:
                    continue

                app_id = -1
                if "app_id" in row and row["app_id"] not in ("", None):
                    try:
                        app_id = int(row["app_id"])
                    except Exception:
                        app_id = -1

                if client not in training_records:
                    training_records[client] = []

                training_records[client].append(value)

                model = iteration_time_models.get(client)

                if hasattr(model, "set_app_id"):
                    model.set_app_id(app_id)

                model.add_records([value])

                if hasattr(iteration_time_models, "set_client_app_id"):
                    iteration_time_models.set_client_app_id(client, app_id)

                loaded_rows += 1
                loaded_clients.add(client)

        else:
            return {
                "status": "failed",
                "et": str(time.time() - st),
                "error": f"CSV missing required columns client,value: {sorted(fieldnames)}",
            }

    print(
        "[iteration-time records-load-command] "
        f"path={csv_path} loaded_rows={loaded_rows} "
        f"loaded_clients={len(loaded_clients)}",
        flush=True,
    )

    return {
        "status": "done",
        "et": str(time.time() - st),
        "path": str(csv_path),
        "loaded_rows": str(loaded_rows),
        "loaded_clients": str(len(loaded_clients)),
    }


def load_iteration_time_model_command(args):
    global iteration_model_version

    st = time.time()
    real_args = _real_command_args(args)

    if not real_args:
        return {
            "status": "failed",
            "et": str(time.time() - st),
            "error": "missing input model path",
        }

    model_path = real_args[0]
    in_path = Path(model_path)

    if not in_path.exists():
        return {
            "status": "failed",
            "et": str(time.time() - st),
            "error": f"model path does not exist: {in_path}",
        }

    iteration_time_models.load(str(in_path))
    iteration_model_version += 1

    print(
        f"[iteration-time model-load-command] path={in_path} "
        f"model_version={iteration_model_version}",
        flush=True,
    )

    return {
        "status": "done",
        "et": str(time.time() - st),
        "path": str(in_path),
        "model_version": str(iteration_model_version),
    }


def iteration_time_model_status_command(args):
    st = time.time()
    real_args = _real_command_args(args)

    target = real_args[0] if real_args else "all"

    if target in ("", "all", "*"):
        client_ids = sorted(iteration_time_models.models.keys())
    else:
        try:
            client_ids = [int(target)]
        except ValueError:
            return {
                "status": "failed",
                "et": str(time.time() - st),
                "error": f"invalid client target: {target}",
            }

    total_clients = len(client_ids)
    trained_clients = 0
    total_records = 0
    per_client = []

    for client_id in client_ids:
        model = iteration_time_models.get(client_id)
        records = len(model.records)
        trained = bool(model.trained)
        total_records += records
        trained_clients += int(trained)
        per_client.append(
            f"{client_id}:records={records},trained={int(trained)}"
        )

    return {
        "status": "done",
        "et": str(time.time() - st),
        "target": str(target),
        "total_clients": str(total_clients),
        "trained_clients": str(trained_clients),
        "total_records": str(total_records),
        "model_version": str(iteration_model_version),
        "clients": ";".join(per_client),
    }


# Backwards-compatible wrapper for the old command name.
def launch_surrogate_inferencing(args, bindata):
    return launch_iteration_time_inferencing(args, bindata)


#

#
# main listener loop
# XXX: add mechanisms for multiple requesters
#

def director_request_command(msg, bindata):
    """Handle all Director surrogate requests through one API.

    Wire format:

        {
            "cmd": "director-request",
            "surrogate_family": "iteration-time" | "event-time",
            "surrogate_backend": "...",
            "operation": "send-records" | "inference" | "train-model" |
                         "save-model" | "load-model" | "load-records-csv" |
                         "model-status",
            "args": [...]
        }
    """
    family = str(msg.get("surrogate_family", "iteration-time")).strip()
    operation = str(msg.get("operation", "")).strip()
    backend = str(msg.get("surrogate_backend", "")).strip()
    args = _real_command_args(msg.get("args", []))

    operation_aliases = {
        "status": "model-status",
        "train": "train-model",
        "save": "save-model",
        "load": "load-model",
        "load-records": "load-records-csv",
        "do-inference": "inference",
    }
    operation = operation_aliases.get(operation, operation)

    if family in ("", "iteration", "iteration-time"):
        if operation == "send-records":
            status, et = receiverecords(args, bindata)
            return {"status": status, "et": str(et)}
        if operation == "inference":
            status, et, predictions = launch_iteration_time_inferencing(args, bindata)
            return {"status": status, "et": str(et), "predictions": predictions}
        if operation == "train-model":
            return train_iteration_time_model_command(args)
        if operation == "save-model":
            return save_iteration_time_model_command(args)
        if operation == "load-model":
            return load_iteration_time_model_command(args)
        if operation == "load-records-csv":
            return load_iteration_records_csv_command(args)
        if operation == "model-status":
            return iteration_time_model_status_command(args)

    if family == "event-time":
        if operation == "send-records":
            status, et, loaded_rows = receive_event_time_records(args, bindata)
            return {
                "status": status,
                "et": str(et),
                "loaded_rows": str(loaded_rows),
            }
        if operation == "inference":
            status, et, predictions = launch_event_time_inferencing(args, bindata)
            return {
                "status": status,
                "et": str(et),
                "predictions": predictions,
                "prediction": predictions.split()[0] if predictions else "",
            }
        if operation == "train-model":
            return train_event_time_model_command(args)
        if operation == "save-model":
            return save_event_time_model_command(args)
        if operation == "load-model":
            return load_event_time_model_command(args)
        if operation == "load-records-csv":
            return load_event_time_records_csv_command(args)
        if operation == "model-status":
            return event_time_model_status_command(args)

    return {
        "status": "failed",
        "error": (
            "unknown director request: "
            f"surrogate_family={family} surrogate_backend={backend} "
            f"operation={operation}"
        ),
    }

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

        elif cmd == "director-request":
            retmsg = director_request_command(msg, bindata)

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
