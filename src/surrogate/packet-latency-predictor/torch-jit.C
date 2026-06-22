#include <torch/csrc/jit/serialization/import.h>
#include <torch/csrc/autograd/generated/variable_factories.h>
#include <torch/csrc/api/include/torch/utils.h>
#include <ATen/Parallel.h>

#include <iostream>
#include <memory>
#include <vector>
#include <chrono>
#include <cmath>

#include <codes/surrogate/packet-latency-predictor/torch-jit.h>
#include <ross.h>

/* Backward-compatible global model used by the two original Torch-JIT modes. */
static torch::jit::Module packet_latency_model;

/* New optional models used only by torch_jit_mode="lp-aware-lp-type-models". */
static torch::jit::Module terminal_packet_latency_model;
static torch::jit::Module default_packet_latency_model;
static torch::jit::Module router_timing_model;

static bool torch_jit_lp_aware_mode = false;
static bool torch_jit_lp_type_model_mode = false;
static bool terminal_packet_latency_model_loaded = false;
static bool default_packet_latency_model_loaded = false;
static bool router_timing_model_loaded = false;
static bool torch_jit_debug_prints = false;

static uint64_t torch_jit_predict_calls = 0;
static uint64_t torch_jit_router_predict_calls = 0;
static double torch_jit_forward_total_sec = 0.0;
static double torch_jit_router_forward_total_sec = 0.0;
static double torch_jit_min_travel_delta = 1.0e300;
static double torch_jit_max_travel_delta = -1.0e300;
static double torch_jit_min_next_delay = 1.0e300;
static double torch_jit_max_next_delay = -1.0e300;
static double torch_jit_min_router_queueing_delay = 1.0e300;
static double torch_jit_max_router_queueing_delay = -1.0e300;

static constexpr int TORCH_JIT_LEGACY_FEATURE_COUNT = 4;
static constexpr int TORCH_JIT_LP_AWARE_FEATURE_COUNT = 12;
static constexpr int TORCH_JIT_ROUTER_TIMING_FEATURE_COUNT = 12;

void surrogate_torch_set_lp_aware_mode(bool enabled) {
    torch_jit_lp_aware_mode = enabled;
}

void surrogate_torch_set_debug_prints(bool enabled) {
    torch_jit_debug_prints = enabled;
}

static bool has_path(char const* path) {
    return path != nullptr && path[0] != '\0';
}

static void load_module_or_die(torch::jit::Module* module, char const* path, char const* label) {
    if (!has_path(path)) {
        tw_error(TW_LOC, "Missing Torch-JIT %s path", label);
    }

    try {
        *module = torch::jit::load(path);
    } catch (const c10::Error& e) {
        tw_error(TW_LOC, "Error loading Torch-JIT %s model from `%s`", label, path);
    }

    if (module->is_training()) {
        std::cerr
            << "The Torch-JIT " << label
            << " model was saved before running .eval(); inference will use training-mode behavior."
            << std::endl;
    }
}

static void validate_output_dims(at::Tensor const& output, int expected_last_dim,
                                 char const* label) {
    int const dims = output.ndimension();
    if (dims < 1) {
        tw_error(TW_LOC, "Torch-JIT %s model returned a scalar; expected [1,%d]", label,
                 expected_last_dim);
    }
    for (int i = 0; i < dims - 1; i++) {
        if (at::size(output, i) != 1) {
            tw_error(TW_LOC, "Torch-JIT %s model returned unexpected dim %d size %lld; expected 1",
                     label, i, (long long)at::size(output, i));
        }
    }
    if (at::size(output, dims - 1) != expected_last_dim) {
        tw_error(TW_LOC, "Torch-JIT %s model returned last dim %lld; expected %d", label,
                 (long long)at::size(output, dims - 1), expected_last_dim);
    }
}

static void validate_packet_latency_model(torch::jit::Module* model, int feature_count,
                                          at::ScalarType dtype, char const* label) {
    std::vector<torch::jit::IValue> inputs;
    torch::NoGradGuard no_grad;

    if (dtype == at::kFloat) {
        std::vector<float> data_input(feature_count, 0.0f);
        inputs.emplace_back(
            torch::from_blob(data_input.data(), {1, feature_count}, at::kFloat).clone());
    } else if (dtype == at::kLong) {
        std::vector<long int> data_input(feature_count, 0);
        inputs.emplace_back(
            torch::from_blob(data_input.data(), {1, feature_count}, at::kLong).clone());
    } else {
        tw_error(TW_LOC, "Unsupported Torch-JIT validation dtype for %s", label);
    }

    at::Tensor output = model->forward(inputs).toTensor();
    validate_output_dims(output, 2, label);
}

static void validate_router_timing_model(torch::jit::Module* model, char const* label) {
    std::vector<torch::jit::IValue> inputs;
    torch::NoGradGuard no_grad;
    std::vector<float> data_input(TORCH_JIT_ROUTER_TIMING_FEATURE_COUNT, 0.0f);
    inputs.emplace_back(
        torch::from_blob(data_input.data(), {1, TORCH_JIT_ROUTER_TIMING_FEATURE_COUNT}, at::kFloat)
            .clone());
    at::Tensor output = model->forward(inputs).toTensor();
    validate_output_dims(output, 1, label);
}

static std::vector<float> build_lp_aware_packet_features(tw_lp* lp, unsigned int src_terminal,
                                                         struct packet_start const* packet_dest) {
    assert(packet_dest != nullptr);

    return {(float)src_terminal,
            (float)packet_dest->dfdally_dest_terminal_id,
            (float)packet_dest->packet_size,
            packet_dest->is_there_another_pckt_in_queue ? 1.0f : 0.0f,
            lp ? (float)lp->gid : -1.0f,
            (float)packet_dest->src_router_id,
            (float)packet_dest->src_group_id,
            (float)packet_dest->dst_router_id,
            (float)packet_dest->dst_group_id,
            (float)packet_dest->terminal_queue_length,
            (float)packet_dest->terminal_vc_occupancy,
            (float)packet_dest->processing_packet_delay};
}

void surrogate_torch_init(char const* dir) {
    std::cout << "Loading Torch-JIT packet-latency model\n";

    torch_jit_lp_type_model_mode = false;
    terminal_packet_latency_model_loaded = false;
    default_packet_latency_model_loaded = false;
    router_timing_model_loaded = false;

    load_module_or_die(&packet_latency_model, dir, "packet-latency");

    at::set_num_threads(1);

    if (torch_jit_lp_aware_mode) {
        validate_packet_latency_model(&packet_latency_model, TORCH_JIT_LP_AWARE_FEATURE_COUNT,
                                      at::kFloat, "lp-aware packet-latency");
    } else {
        validate_packet_latency_model(&packet_latency_model, TORCH_JIT_LEGACY_FEATURE_COUNT,
                                      at::kLong, "legacy packet-latency");
    }

    std::cout << "Torch-JIT packet-latency model loaded successfully\n";
}

void surrogate_torch_init_lp_type_models(char const* terminal_model_path,
                                         char const* router_timing_model_path,
                                         char const* default_model_path) {
    std::cout << "Loading Torch-JIT LP-type-aware models\n";

    torch_jit_lp_aware_mode = true;
    torch_jit_lp_type_model_mode = true;
    terminal_packet_latency_model_loaded = false;
    default_packet_latency_model_loaded = false;
    router_timing_model_loaded = false;

    at::set_num_threads(1);

    if (has_path(terminal_model_path)) {
        load_module_or_die(&terminal_packet_latency_model, terminal_model_path,
                           "terminal packet-latency");
        validate_packet_latency_model(&terminal_packet_latency_model,
                                      TORCH_JIT_LP_AWARE_FEATURE_COUNT, at::kFloat,
                                      "terminal packet-latency");
        terminal_packet_latency_model_loaded = true;
    }

    if (has_path(default_model_path)) {
        load_module_or_die(&default_packet_latency_model, default_model_path,
                           "default packet-latency");
        validate_packet_latency_model(&default_packet_latency_model,
                                      TORCH_JIT_LP_AWARE_FEATURE_COUNT, at::kFloat,
                                      "default packet-latency");
        default_packet_latency_model_loaded = true;
    }

    if (!terminal_packet_latency_model_loaded && !default_packet_latency_model_loaded) {
        tw_error(TW_LOC, "torch_jit_mode=lp-aware-lp-type-models requires "
                         "torch_jit_terminal_model_path or torch_jit_default_model_path");
    }

    if (has_path(router_timing_model_path)) {
        load_module_or_die(&router_timing_model, router_timing_model_path, "router timing");
        validate_router_timing_model(&router_timing_model, "router timing");
        router_timing_model_loaded = true;
    } else {
        tw_warning(
            TW_LOC,
            "No torch_jit_router_timing_model_path configured; router timing inference disabled.");
    }

    std::cout << "Torch-JIT LP-type-aware models loaded successfully\n";
}

bool surrogate_torch_router_timing_model_enabled(void) {
    return torch_jit_lp_type_model_mode && router_timing_model_loaded;
}

static struct packet_end surrogate_torch_predict(void*, tw_lp* lp, unsigned int src_terminal,
                                                 struct packet_start const* packet_dest) {
    std::vector<torch::jit::IValue> inputs;
    torch::jit::Module* model = &packet_latency_model;

    if (torch_jit_lp_type_model_mode) {
        model = terminal_packet_latency_model_loaded ? &terminal_packet_latency_model
                                                     : &default_packet_latency_model;
    }

    if (torch_jit_lp_aware_mode || torch_jit_lp_type_model_mode) {
        std::vector<float> data_input =
            build_lp_aware_packet_features(lp, src_terminal, packet_dest);

        if (torch_jit_debug_prints && torch_jit_predict_calls < 20) {
            fprintf(stderr,
                    "[torch-jit feature debug] "
                    "src_terminal=%g dest_terminal=%g packet_size=%g another=%g "
                    "caller_lp_gid=%g src_router_id=%g src_group_id=%g "
                    "dst_router_id=%g dst_group_id=%g terminal_queue_length=%g "
                    "terminal_vc_occupancy=%g processing_packet_delay=%g\n",
                    (double)data_input[0], (double)data_input[1], (double)data_input[2],
                    (double)data_input[3], (double)data_input[4], (double)data_input[5],
                    (double)data_input[6], (double)data_input[7], (double)data_input[8],
                    (double)data_input[9], (double)data_input[10], (double)data_input[11]);
            fflush(stderr);
        }

        assert((int)data_input.size() == TORCH_JIT_LP_AWARE_FEATURE_COUNT);
        inputs.emplace_back(
            torch::from_blob(data_input.data(), {1, TORCH_JIT_LP_AWARE_FEATURE_COUNT}, at::kFloat)
                .clone());
    } else {
        long int data_input[] = {(long int)src_terminal,
                                 (long int)packet_dest->dfdally_dest_terminal_id,
                                 (long int)packet_dest->packet_size,
                                 packet_dest->is_there_another_pckt_in_queue ? 1L : 0L};
        inputs.emplace_back(
            torch::from_blob(data_input, {1, TORCH_JIT_LEGACY_FEATURE_COUNT}, at::kLong).clone());
    }

    torch::NoGradGuard no_grad;
    auto const torch_jit_t0 = std::chrono::high_resolution_clock::now();
    at::Tensor output = model->forward(inputs).toTensor();
    auto const torch_jit_t1 = std::chrono::high_resolution_clock::now();
    validate_output_dims(output, 2, "packet-latency inference");

    torch_jit_forward_total_sec +=
        std::chrono::duration<double>(torch_jit_t1 - torch_jit_t0).count();
    torch_jit_predict_calls++;

    output = output.to(at::kFloat).contiguous();
    auto* out_data = output.data_ptr<float>();
    double const raw_travel_delta = (double)out_data[0];
    double const raw_next_delay = (double)out_data[1];

    double const min_travel_delta = 10.0;
    double const min_next_packet_delay = 10.0;

    double const predicted_travel_delta =
        std::isfinite(raw_travel_delta) && raw_travel_delta > min_travel_delta ? raw_travel_delta
                                                                               : min_travel_delta;

    double const predicted_next_packet_delay =
        std::isfinite(raw_next_delay) && raw_next_delay > min_next_packet_delay
            ? raw_next_delay
            : min_next_packet_delay;

    if (raw_travel_delta < torch_jit_min_travel_delta)
        torch_jit_min_travel_delta = raw_travel_delta;
    if (raw_travel_delta > torch_jit_max_travel_delta)
        torch_jit_max_travel_delta = raw_travel_delta;
    if (raw_next_delay < torch_jit_min_next_delay)
        torch_jit_min_next_delay = raw_next_delay;
    if (raw_next_delay > torch_jit_max_next_delay)
        torch_jit_max_next_delay = raw_next_delay;

    if (torch_jit_debug_prints &&
        (torch_jit_predict_calls <= 20 || torch_jit_predict_calls % 10000 == 0)) {
        fprintf(stderr,
                "[torch-jit predict debug] calls=%llu avg_forward_us=%g "
                "raw_travel_delta=%g raw_next_delay=%g "
                "effective_travel_delta=%g effective_next_delay=%g "
                "minmax_travel=[%g,%g] minmax_next=[%g,%g]\n",
                (unsigned long long)torch_jit_predict_calls,
                1.0e6 * torch_jit_forward_total_sec / (double)torch_jit_predict_calls,
                raw_travel_delta, raw_next_delay, predicted_travel_delta,
                predicted_next_packet_delay, torch_jit_min_travel_delta, torch_jit_max_travel_delta,
                torch_jit_min_next_delay, torch_jit_max_next_delay);
        fflush(stderr);
    }

    return (struct packet_end){
        .travel_end_time = packet_dest->travel_start_time + predicted_travel_delta,
        .next_packet_delay = predicted_next_packet_delay,
    };
}

double
surrogate_torch_predict_router_queueing_delay(struct router_timing_prediction_start const* start,
                                              double fallback_queueing_delay) {
    if (!surrogate_torch_router_timing_model_enabled() || start == nullptr) {
        return fallback_queueing_delay;
    }

    std::vector<float> data_input = {start->router_id,
                                     start->group_id,
                                     start->output_port,
                                     start->output_chan,
                                     start->to_terminal,
                                     start->is_global,
                                     start->packet_size,
                                     start->chunk_size,
                                     start->output_vc_occupancy,
                                     start->output_queued_count,
                                     start->next_output_available_delta,
                                     start->nominal_router_delay};
    assert((int)data_input.size() == TORCH_JIT_ROUTER_TIMING_FEATURE_COUNT);

    std::vector<torch::jit::IValue> inputs;
    inputs.emplace_back(
        torch::from_blob(data_input.data(), {1, TORCH_JIT_ROUTER_TIMING_FEATURE_COUNT}, at::kFloat)
            .clone());

    torch::NoGradGuard no_grad;
    auto const torch_jit_t0 = std::chrono::high_resolution_clock::now();
    at::Tensor output = router_timing_model.forward(inputs).toTensor();
    auto const torch_jit_t1 = std::chrono::high_resolution_clock::now();
    validate_output_dims(output, 1, "router timing inference");

    torch_jit_router_forward_total_sec +=
        std::chrono::duration<double>(torch_jit_t1 - torch_jit_t0).count();
    torch_jit_router_predict_calls++;

    output = output.to(at::kFloat).contiguous();
    double const raw_queueing_delay = (double)output.data_ptr<float>()[0];
    double const predicted_queueing_delay =
        std::isfinite(raw_queueing_delay) && raw_queueing_delay >= 0.0 ? raw_queueing_delay
                                                                       : fallback_queueing_delay;

    if (raw_queueing_delay < torch_jit_min_router_queueing_delay)
        torch_jit_min_router_queueing_delay = raw_queueing_delay;
    if (raw_queueing_delay > torch_jit_max_router_queueing_delay)
        torch_jit_max_router_queueing_delay = raw_queueing_delay;

    if (torch_jit_debug_prints &&
        (torch_jit_router_predict_calls <= 20 || torch_jit_router_predict_calls % 10000 == 0)) {
        fprintf(stderr,
                "[torch-jit router timing debug] calls=%llu avg_forward_us=%g "
                "raw_queueing_delay=%g effective_queueing_delay=%g minmax_queueing=[%g,%g]\n",
                (unsigned long long)torch_jit_router_predict_calls,
                1.0e6 * torch_jit_router_forward_total_sec / (double)torch_jit_router_predict_calls,
                raw_queueing_delay, predicted_queueing_delay, torch_jit_min_router_queueing_delay,
                torch_jit_max_router_queueing_delay);
        fflush(stderr);
    }

    return predicted_queueing_delay;
}

// Dummies to use when no actual data is fed
static void init_pred_dummy(void* data, tw_lp* lp, unsigned int src_terminal) {
    (void)data;
    (void)lp;
    (void)src_terminal;
}

static void feed_pred_dummy(struct latency_surrogate* data, tw_lp* lp, unsigned int src_terminal,
                            struct packet_start const* start, struct packet_end const* end) {
    (void)data;
    (void)lp;
    (void)src_terminal;
    (void)start;
    (void)end;
}

static void predict_latency_rc_dummy(struct latency_surrogate* data, tw_lp* lp) {
    (void)data;
    (void)lp;
}

struct packet_latency_predictor torch_latency_predictor = {
    .init = (init_pred_lat_f)init_pred_dummy,
    .feed = (feed_pred_lat_f)feed_pred_dummy,
    .predict = (predict_pred_lat_f)surrogate_torch_predict,
    .predict_rc = (predict_pred_lat_rc_f)predict_latency_rc_dummy,
    .predictor_data_sz = 0};
