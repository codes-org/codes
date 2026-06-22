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
import io
import pickle
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

training_records = {} # client_id -> []

ITERATION_MODEL_KWARGS = {
    "history_len": int(os.environ.get("ZMQML_ITERATION_HISTORY_LEN", "4")),
    "horizon": int(os.environ.get("ZMQML_ITERATION_HORIZON", "30")),
    "ridge_alpha": float(os.environ.get("ZMQML_ITERATION_RIDGE_ALPHA", "1.0")),
    "train_stride": int(os.environ.get("ZMQML_ITERATION_TRAIN_STRIDE", "3")),
}

EVENT_TIME_MODEL_KWARGS = {
    "min_rows": int(os.environ.get("ZMQML_EVENT_TIME_MIN_ROWS", "32")),
    "max_epochs": int(os.environ.get("ZMQML_EVENT_TIME_EPOCHS", "80")),
    "lr": float(os.environ.get("ZMQML_EVENT_TIME_LR", "0.001")),
    "hidden_dim": int(os.environ.get("ZMQML_EVENT_TIME_HIDDEN_DIM", "64")),
}

DEFAULT_TERMINAL_MODEL_SCOPE = "terminal"
DEFAULT_TERMINAL_MODEL_KEY = "global"

# The current dragonfly-dally event-time rows use numeric LP-type fields.
# By default, current_lp_type=0 is treated as a terminal LP; all other LP
# types get switch-local models. Override if the enum changes.
EVENT_TIME_TERMINAL_LP_TYPES = {
    token.strip()
    for token in os.environ.get("ZMQML_EVENT_TIME_TERMINAL_LP_TYPES", "0").split(",")
    if token.strip()
}


def normalize_model_identity(model_scope: str | None = None, model_key: str | None = None) -> tuple[str, str, str]:
    scope = str(model_scope or "").strip()
    key = str(model_key or "").strip()

    if not scope:
        scope = DEFAULT_TERMINAL_MODEL_SCOPE

    if scope in ("terminal", "term", "client", "global"):
        scope = DEFAULT_TERMINAL_MODEL_SCOPE
        key = DEFAULT_TERMINAL_MODEL_KEY
    elif scope in ("router", "switch", "switch-lp", "router-lp"):
        scope = "switch"
        if not key:
            key = "unknown"
    else:
        if not key:
            key = DEFAULT_TERMINAL_MODEL_KEY

    model_id = f"{scope}:{key}"
    return scope, key, model_id


def model_identity_from_real_args(
    real_args: list[str],
    *,
    offset: int,
    default_scope: str = DEFAULT_TERMINAL_MODEL_SCOPE,
    default_key: str = DEFAULT_TERMINAL_MODEL_KEY,
) -> tuple[str, str, str]:
    if len(real_args) >= offset + 2:
        return normalize_model_identity(real_args[offset], real_args[offset + 1])
    return normalize_model_identity(default_scope, default_key)



class ScopedIterationTimeModelRegistry:
    """Compatibility facade that keeps iteration-time unscoped.

    Event-time uses scoped models, but iteration-time should remain the original
    client/app-level model. The surrounding unified Director request code may
    still pass model_scope/model_key arguments; this facade deliberately ignores
    them and routes all iteration-time records/inference to one plain
    IterationTimeModelRegistry.
    """

    def __init__(self, kwargs: dict):
        self.kwargs = dict(kwargs)
        self.registry = IterationTimeModelRegistry(**self.kwargs)
        self.debug = False

    def set_debug(self, enabled: bool) -> None:
        self.debug = bool(enabled)
        self.registry.set_debug(self.debug)

    def get_registry(self, model_scope: str | None = None, model_key: str | None = None) -> IterationTimeModelRegistry:
        return self.registry

    def get(self, client_id: int, model_scope: str | None = None, model_key: str | None = None):
        return self.registry.get(client_id)

    def set_client_app_id(self, client_id: int, app_id: int, model_scope: str | None = None, model_key: str | None = None) -> None:
        self.registry.set_client_app_id(client_id, app_id)

    def predict(self, client_id: int, requested_horizon: int | None = None, model_scope: str | None = None, model_key: str | None = None):
        return self.registry.predict(client_id, requested_horizon)

    def train_or_update(self, model_scope: str | None = None, model_key: str | None = None) -> bool:
        return self.registry.train_or_update()

    def save(self, path: str) -> None:
        self.registry.save(path)

    def load(self, path: str) -> None:
        self.registry.load(path)
        self.registry.set_debug(self.debug)

    def status(self, model_scope: str | None = None, model_key: str | None = None) -> dict:
        client_ids = sorted(self.registry.models.keys())
        total_clients = len(client_ids)
        trained_clients = 0
        total_records = 0
        client_summaries = []

        for client_id in client_ids:
            model = self.registry.get(client_id)
            records = len(model.records)
            trained = bool(model.trained)
            total_records += records
            trained_clients += int(trained)
            client_summaries.append(
                f"{client_id}:records={records},trained={int(trained)}"
            )

        return {
            "total_clients": str(total_clients),
            "trained_clients": str(trained_clients),
            "total_records": str(total_records),
            "clients": ";".join(client_summaries),
        }

class ScopedEventTimeModelRegistry:
    def __init__(self, kwargs: dict):
        self.kwargs = dict(kwargs)
        self.models: dict[str, EventTimeModel] = {}
        self.debug = False
        self.model_versions: dict[str, int] = {}

    def set_debug(self, enabled: bool) -> None:
        self.debug = bool(enabled)
        for model in self.models.values():
            model.set_debug(self.debug)

    def get(self, model_scope: str | None = None, model_key: str | None = None) -> EventTimeModel:
        _, _, model_id = normalize_model_identity(model_scope, model_key)
        if model_id not in self.models:
            model = EventTimeModel(**self.kwargs)
            model.set_debug(self.debug)
            self.models[model_id] = model
            self.model_versions.setdefault(model_id, 0)
        return self.models[model_id]

    def model_id(self, model_scope: str | None = None, model_key: str | None = None) -> str:
        return normalize_model_identity(model_scope, model_key)[2]

    def train_or_update(self, model_scope: str | None = None, model_key: str | None = None) -> bool:
        if model_scope in (None, "", "all", "*"):
            trained_any = False
            for model_id, model in self.models.items():
                trained = model.train_or_update()
                if trained:
                    self.model_versions[model_id] = self.model_versions.get(model_id, 0) + 1
                trained_any = trained or trained_any
            return trained_any

        model_id = self.model_id(model_scope, model_key)
        model = self.get(model_scope, model_key)
        trained = model.train_or_update()
        if trained:
            self.model_versions[model_id] = self.model_versions.get(model_id, 0) + 1
        return trained

    @staticmethod
    def _safe_filename(model_id: str) -> str:
        return model_id.replace("/", "_").replace(":", "__")

    def save(self, path: str | Path, model_scope: str | None = None, model_key: str | None = None) -> None:
        path = Path(path)

        if model_scope not in (None, "", "all", "*"):
            model = self.get(model_scope, model_key)
            if path.parent:
                path.parent.mkdir(parents=True, exist_ok=True)
            model.save(path)
            return

        path.mkdir(parents=True, exist_ok=True)
        manifest = {
            "format": "scoped-event-time-directory-v1",
            "models": {},
        }

        for model_id, model in sorted(self.models.items()):
            filename = self._safe_filename(model_id) + ".pt"
            model.save(path / filename)
            manifest["models"][model_id] = filename

        (path / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True))

    def load(self, path: str | Path, model_scope: str | None = None, model_key: str | None = None) -> None:
        path = Path(path)

        if path.is_dir():
            manifest_path = path / "manifest.json"
            if not manifest_path.exists():
                raise FileNotFoundError(f"missing scoped event-time manifest: {manifest_path}")

            manifest = json.loads(manifest_path.read_text())
            self.models.clear()
            self.model_versions.clear()
            for model_id, filename in manifest.get("models", {}).items():
                scope, key = model_id.split(":", 1)
                model = self.get(scope, key)
                model.load(path / filename)
                model.set_debug(self.debug)
                self.model_versions[model_id] = self.model_versions.get(model_id, 0) + 1
            return

        # Backward-compatible load of an old single event-time model file.
        model = self.get(model_scope, model_key)
        model.load(path)
        model.set_debug(self.debug)
        model_id = self.model_id(model_scope, model_key)
        self.model_versions[model_id] = self.model_versions.get(model_id, 0) + 1

    def status(self, model_scope: str | None = None, model_key: str | None = None) -> dict[str, str]:
        if model_scope in (None, "", "all", "*"):
            model_ids = sorted(self.models)
        else:
            model_ids = [self.model_id(model_scope, model_key)]

        total_rows = 0
        trained_models = 0
        entries = []

        for model_id in model_ids:
            model = self.models.get(model_id)
            if model is None:
                continue
            total_rows += len(model.rows)
            trained_models += int(bool(model.trained))
            entries.append(
                f"{model_id}:rows={len(model.rows)},trained={int(model.trained)},examples={model.training_examples}"
            )

        return {
            "model_count": str(len(model_ids)),
            "trained_models": str(trained_models),
            "total_rows": str(total_rows),
            "models": ";".join(entries),
        }


iteration_time_models = IterationTimeModelRegistry(**ITERATION_MODEL_KWARGS)
event_time_models = ScopedEventTimeModelRegistry(EVENT_TIME_MODEL_KWARGS)

# Compatibility alias for old helper code that still references event_time_model.
event_time_model = event_time_models.get()
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
    event_time_models.load(event_time_model_path)
    event_time_model = event_time_models.get()
    event_time_model_version = 1
    print(
        f"[zmqmlserver] loaded event-time model(s): {event_time_model_path}",
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
    event_time_models.set_debug(director_debug_prints)

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

    real_args = _real_command_args(args)
    if len(real_args) < 2:
        return ("failed", time.time() - st)

    client = int(real_args[0])
    num_records = int(real_args[1])

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

    if parsed_records:
        append_record_log(client, parsed_records)
        model = iteration_time_models.get(client)

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


def launch_iteration_time_inferencing(args, bindata):
    status = "failed"
    st = time.time()

    real_args = _real_command_args(args)
    if len(real_args) < 2:
        return ("failed", time.time() - st, "")

    client = int(real_args[0])
    num_steps = int(real_args[1])

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


def event_time_model_identity_from_row(row: dict) -> tuple[str, str, str]:
    raw_lp_type = str(row.get("current_lp_type", "")).strip()
    raw_gid = str(row.get("current_lp_gid", "")).strip()

    if raw_lp_type in EVENT_TIME_TERMINAL_LP_TYPES:
        return normalize_model_identity("terminal", "global")

    return normalize_model_identity("switch", raw_gid or "unknown")


def iter_event_time_rows_from_payload(raw_payload: str):
    payload = event_time_payload_with_header(raw_payload)
    if not payload.strip():
        return

    reader = csv.DictReader(io.StringIO(payload))
    if reader.fieldnames:
        reader.fieldnames = [str(name).strip().lstrip("#").strip() for name in reader.fieldnames]

    for row in reader:
        clean = {str(k).strip().lstrip("#").strip(): v for k, v in row.items()}
        yield clean


def receive_event_time_records(args, bindata):
    st = time.time()

    raw_payload = bindata.decode("utf-8", errors="replace").strip()
    loaded_rows = 0

    # Important performance rule:
    # Do NOT call EventTimeModel.add_records_text(...) once per row.
    # Event-time batches can contain 65K+ rows, and per-row parsing/routing makes
    # pure-PDES collection much slower than the old single-global model path.
    #
    # Instead, parse once, group rows by scoped model id, then call
    # add_records_text(...) once per scoped model per C++ batch.
    grouped_rows: dict[str, list[dict]] = {}
    grouped_identity: dict[str, tuple[str, str]] = {}

    for row in iter_event_time_rows_from_payload(raw_payload) or []:
        model_scope, model_key, model_id = event_time_model_identity_from_row(row)
        grouped_rows.setdefault(model_id, []).append(row)
        grouped_identity[model_id] = (model_scope, model_key)

    per_model_loaded: dict[str, int] = {}

    for model_id, rows in grouped_rows.items():
        model_scope, model_key = grouped_identity[model_id]
        model = event_time_models.get(model_scope, model_key)

        if not rows:
            continue

        # Build one CSV payload for this model. Preserve the field order from
        # the first row and include any later extra keys defensively.
        fieldnames = list(rows[0].keys())
        seen = set(fieldnames)
        for row in rows[1:]:
            for key in row.keys():
                if key not in seen:
                    fieldnames.append(key)
                    seen.add(key)

        buf = io.StringIO()
        writer = csv.DictWriter(buf, fieldnames=fieldnames, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)

        accepted = model.add_records_text(buf.getvalue())
        loaded_rows += accepted
        per_model_loaded[model_id] = accepted

        if accepted > 0 and event_time_auto_train_on_records:
            model.train_or_update()

    if loaded_rows > 0:
        append_event_time_record_log(raw_payload)

    if director_debug_prints:
        # Keep this compact. Printing the full per_model dict for 100+ switch
        # models is noisy and can itself become expensive.
        nonempty_models = sum(1 for v in per_model_loaded.values() if v > 0)
        min_rows = min(per_model_loaded.values()) if per_model_loaded else 0
        max_rows = max(per_model_loaded.values()) if per_model_loaded else 0
        sample = sorted(per_model_loaded.items())[:8]
        print(
            f"[event-time records] loaded_rows={loaded_rows} "
            f"models={nonempty_models} min_rows_per_model={min_rows} "
            f"max_rows_per_model={max_rows} sample={sample}",
            flush=True,
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

    loaded_rows = 0
    files = sorted(path.rglob("*")) if path.is_dir() else [path]

    for child in files:
        if not child.is_file() or child.suffix.lower() not in (".csv", ".txt", ".log"):
            continue
        status, _et, child_rows = receive_event_time_records(["0"], child.read_text().encode("utf-8"))
        if status == "done":
            loaded_rows += int(child_rows)

    ret = {
        "status": "done",
        "et": str(time.time() - st),
        "path": str(path),
        "loaded_rows": str(loaded_rows),
    }
    ret.update(event_time_models.status())
    return ret


def train_event_time_model_command(args):
    global event_time_model_version

    st = time.time()
    real_args = _real_command_args(args)

    target = real_args[0] if real_args else "all"
    if target in ("", "all", "*"):
        trained = event_time_models.train_or_update("all", "")
        model_scope = "all"
        model_key = ""
        model_id = "all"
    elif len(real_args) >= 2:
        model_scope, model_key, model_id = normalize_model_identity(real_args[0], real_args[1])
        trained = event_time_models.train_or_update(model_scope, model_key)
    else:
        model_scope, model_key, model_id = normalize_model_identity("switch", target)
        trained = event_time_models.train_or_update(model_scope, model_key)

    if trained:
        event_time_model_version += 1

    ret = {
        "status": "done" if trained else "failed",
        "et": str(time.time() - st),
        "target": model_id,
        "model_version": str(event_time_model_version),
    }
    ret.update(event_time_models.status(model_scope, model_key))

    if not trained:
        ret["error"] = "event-time model was not trained; load enough scoped event-time rows first"

    print(
        f"[event-time model-train-command] target={model_id} trained={int(trained)} "
        f"model_version={event_time_model_version} status={event_time_models.status(model_scope, model_key)}",
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
    target = real_args[1] if len(real_args) >= 2 else "all"

    if target in ("", "all", "*"):
        event_time_models.save(model_path, "all", "")
        model_id = "all"
    elif len(real_args) >= 3:
        model_scope, model_key, model_id = normalize_model_identity(real_args[1], real_args[2])
        event_time_models.save(model_path, model_scope, model_key)
    else:
        model_scope, model_key, model_id = normalize_model_identity("switch", target)
        event_time_models.save(model_path, model_scope, model_key)

    return {
        "status": "done",
        "et": str(time.time() - st),
        "path": str(model_path),
        "target": model_id,
        "model_version": str(event_time_model_version),
    }


def load_event_time_model_command(args):
    global event_time_model_version, event_time_model

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

    target = real_args[1] if len(real_args) >= 2 else "all"

    if target in ("", "all", "*"):
        event_time_models.load(model_path)
        model_id = "all"
    elif len(real_args) >= 3:
        model_scope, model_key, model_id = normalize_model_identity(real_args[1], real_args[2])
        event_time_models.load(model_path, model_scope, model_key)
    else:
        model_scope, model_key, model_id = normalize_model_identity("switch", target)
        event_time_models.load(model_path, model_scope, model_key)

    event_time_model = event_time_models.get()
    event_time_model_version += 1

    return {
        "status": "done",
        "et": str(time.time() - st),
        "path": str(model_path),
        "target": model_id,
        "model_version": str(event_time_model_version),
    }


def event_time_model_status_command(args):
    st = time.time()
    real_args = _real_command_args(args)

    if not real_args or real_args[0] in ("", "all", "*"):
        model_scope = "all"
        model_key = ""
        model_id = "all"
    elif len(real_args) >= 2:
        model_scope, model_key, model_id = normalize_model_identity(real_args[0], real_args[1])
    else:
        model_scope, model_key, model_id = normalize_model_identity("switch", real_args[0])

    ret = {
        "status": "done",
        "et": str(time.time() - st),
        "target": model_id,
        "model_version": str(event_time_model_version),
    }
    ret.update(event_time_models.status(model_scope, model_key))
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
    rows = list(iter_event_time_rows_from_payload(payload) or [])

    if rows:
        model_scope, model_key, model_id = event_time_model_identity_from_row(rows[0])
    else:
        model_scope, model_key, model_id = normalize_model_identity()

    model = event_time_models.get(model_scope, model_key)
    predictions = model.predict_from_text(
        payload,
        requested_count=max(1, requested_count),
    )
    predictions_str = " ".join(str(float(x)) for x in predictions)

    director_debug(
        f"[event-time inference] model={model_id} requested_count={requested_count} "
        f"payload_bytes={len(payload)} trained={int(model.trained)} "
        f"rows={len(model.rows)} predictions={predictions_str}"
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
    # Do not normalize here.  Each command handler normalizes exactly once.
    #
    # Normalizing here and again in receiverecords()/inference breaks client 1:
    #   ["2", "1", "9"] -> ["1", "9"] -> ["9"]
    # so the server drops client 1 records and later returns empty predictions.
    args = msg.get("args", [])

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
