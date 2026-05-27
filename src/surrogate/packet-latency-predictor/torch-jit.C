#include <torch/csrc/jit/serialization/import.h>
#include <torch/csrc/autograd/generated/variable_factories.h>
#include <torch/csrc/api/include/torch/utils.h>
#include <ATen/Parallel.h>

#include <iostream>
#include <memory>
#include <vector>
#include <cmath>

#include <codes/surrogate/packet-latency-predictor/torch-jit.h>
#include <ross.h>

static torch::jit::Module packet_latency_model;

static bool torch_jit_lp_aware_mode = false;

static constexpr int TORCH_JIT_LEGACY_FEATURE_COUNT = 4;
static constexpr int TORCH_JIT_LP_AWARE_FEATURE_COUNT = 12;

void surrogate_torch_set_lp_aware_mode(bool enabled) {
    torch_jit_lp_aware_mode = enabled;
}

static std::vector<float> build_lp_aware_packet_features(
        tw_lp *lp,
        unsigned int src_terminal,
        struct packet_start const *packet_dest)
{
    assert(packet_dest != nullptr);

    return {
        /* Existing four features first. */
        (float)src_terminal,
        (float)packet_dest->dfdally_dest_terminal_id,
        (float)packet_dest->packet_size,
        packet_dest->is_there_another_pckt_in_queue ? 1.0f : 0.0f,

        /* Explicit LP/topology/context features. */
        lp ? (float)lp->gid : -1.0f,
        (float)packet_dest->src_router_id,
        (float)packet_dest->src_group_id,
        (float)packet_dest->dst_router_id,
        (float)packet_dest->dst_group_id,
        (float)packet_dest->terminal_queue_length,
        (float)packet_dest->terminal_vc_occupancy,
        (float)packet_dest->processing_packet_delay
    };
}


inline void assert_correct_dims(at::Tensor * t) {
    int const dims = t->ndimension();

    for (int i = 0; i < dims-1; i++) {
        assert(at::size(*t, i) == 1);
    } 
    assert(at::size(*t, dims - 1) == 2);
}


void surrogate_torch_init(char const * dir) {
    std::cout << "Loading Torch-JIT model\n";
    try {
        // Deserialize the ScriptModule from a file
        packet_latency_model = torch::jit::load(dir);
    }
    catch (const c10::Error& e) {
        tw_error(TW_LOC, "Error loading Torch-JIT model");
    }

    // Configuring to run on a single thread
    at::set_num_threads(1);

    // === Checking consistency of model with dummy input
    if (packet_latency_model.is_training()) {
        std::cerr << "The Torch-JIT model was saved before running .eval(). "
            "The output from the model will be as if it was in training mode, "
            "meaning, it might be faulty."
            << std::endl;
    }

    std::vector<torch::jit::IValue> inputs;
    torch::NoGradGuard no_grad;

    if (torch_jit_lp_aware_mode) {
        std::vector<float> data_input(TORCH_JIT_LP_AWARE_FEATURE_COUNT, 0.0f);
        inputs.emplace_back(
            torch::from_blob(
                data_input.data(),
                {1, TORCH_JIT_LP_AWARE_FEATURE_COUNT},
                at::kFloat).clone());
    } else {
        long int data_input[] = {0, 0, 0, 0};
        inputs.emplace_back(
            torch::from_blob(
                data_input,
                {1, TORCH_JIT_LEGACY_FEATURE_COUNT},
                at::kLong).clone());
    }

    // Predicting value
    at::Tensor output = packet_latency_model.forward(inputs).toTensor();
    assert_correct_dims(&output);
    // === End of check
    std::cout << "Torch-JIT model loaded successfully\n";
}


static struct packet_end surrogate_torch_predict(void *, tw_lp * lp, unsigned int src_terminal, struct packet_start const * packet_dest) {
    //auto t_start = std::chrono::high_resolution_clock::now();

    // Create a vector of inputs.
    std::vector<torch::jit::IValue> inputs;

    if (torch_jit_lp_aware_mode) {
        std::vector<float> data_input =
            build_lp_aware_packet_features(lp, src_terminal, packet_dest);

        assert((int)data_input.size() == TORCH_JIT_LP_AWARE_FEATURE_COUNT);

        inputs.emplace_back(
            torch::from_blob(
                data_input.data(),
                {1, TORCH_JIT_LP_AWARE_FEATURE_COUNT},
                at::kFloat).clone());
    } else {
        long int data_input[] = {
            src_terminal,
            packet_dest->dfdally_dest_terminal_id,
            packet_dest->packet_size,
            packet_dest->is_there_another_pckt_in_queue
        };

        inputs.emplace_back(
            torch::from_blob(
                data_input,
                {1, TORCH_JIT_LEGACY_FEATURE_COUNT},
                at::kLong).clone());
    }

    at::Tensor output = packet_latency_model.forward(inputs).toTensor();
    //assert_correct_dims(&output);

    auto *out_data = output.data_ptr<float>();
        double const raw_travel_delta = (double)out_data[0];
    double const raw_next_delay = (double)out_data[1];

    double const min_travel_delta = 10.0;
    double const min_next_packet_delay = 10.0;

    double const predicted_travel_delta =
            std::isfinite(raw_travel_delta) && raw_travel_delta > min_travel_delta
            ? raw_travel_delta
            : min_travel_delta;

    double const predicted_next_packet_delay =
            std::isfinite(raw_next_delay) && raw_next_delay > min_next_packet_delay
            ? raw_next_delay
            : min_next_packet_delay;
return (struct packet_end) {
        .travel_end_time = packet_dest->travel_start_time + predicted_travel_delta,
        .next_packet_delay = predicted_next_packet_delay,
    };

    //auto t_end = std::chrono::high_resolution_clock::now();
    //double total = std::chrono::duration<double, std::milli>(t_end-t_start).count();
}


// Dummies to use when no actual data is fed
static void init_pred_dummy(void * data, tw_lp * lp, unsigned int src_terminal) {
    (void) data;
    (void) lp;
    (void) src_terminal;
}


static void feed_pred_dummy(struct latency_surrogate * data, tw_lp * lp, unsigned int src_terminal, struct packet_start const * start, struct packet_end const * end) {
    (void) data;
    (void) lp;
    (void) src_terminal;
    (void) start;
    (void) end;
}


static void predict_latency_rc_dummy(struct latency_surrogate * data, tw_lp * lp) {
    (void) data;
    (void) lp;
}


struct packet_latency_predictor torch_latency_predictor = {
    .init              = (init_pred_lat_f) init_pred_dummy,
    .feed              = (feed_pred_lat_f) feed_pred_dummy,
    .predict           = (predict_pred_lat_f) surrogate_torch_predict,
    .predict_rc        = (predict_pred_lat_rc_f) predict_latency_rc_dummy,
    .predictor_data_sz = 0
};
