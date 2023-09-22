#include <torch/csrc/jit/serialization/import.h>
#include <torch/csrc/autograd/generated/variable_factories.h>
#include <ATen/Parallel.h>

#include <iostream>
#include <memory>
#include <vector>

#include <codes/surrogate/packet-latency-predictor/torch-jit.h>

static torch::jit::Module packet_latency_model;


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
        std::cerr << "Error loading Torch-JIT model\n";
        return;
    }

    // Configuring to run on a single thread
    at::set_num_threads(1);

    // === Checking consistency of model with dummy input
    float data_input[] = {0.0, 0.0, 0.0, 0.0};
    size_t const n_input = sizeof(data_input) / sizeof(float);

    std::vector<torch::jit::IValue> inputs;
    inputs.emplace_back(torch::from_blob(data_input, {1, (int) n_input}, at::kFloat));

    // Predicting value
    at::Tensor output = packet_latency_model.forward(inputs).toTensor();
    assert_correct_dims(&output);
    // === End of check
    std::cout << "Torch-JIT model loaded successfully\n";
}


static struct packet_end surrogate_torch_predict(void *, tw_lp * lp, unsigned int src_terminal, struct packet_start const * packet_dest) {
    //auto t_start = std::chrono::high_resolution_clock::now();

    // Create a vector of inputs.
    float data_input[] = {
        src_terminal,
        packet_dest->dfdally_dest_terminal_id,
        packet_dest->packet_size,
        packet_dest->is_there_another_pckt_in_queue
    };
    size_t n_input = sizeof(data_input) / sizeof(float);

    std::vector<torch::jit::IValue> inputs;
    inputs.emplace_back(torch::from_blob(data_input, {1, (int) n_input}, at::kFloat));

    at::Tensor output = packet_latency_model.forward(inputs).toTensor();
    //assert_correct_dims(&output);

    auto *out_data = output.data_ptr<float>();
    return (struct packet_end) {
        .travel_end_time = packet_dest->travel_start_time + (out_data[0] > 0 ? out_data[0] : 10),
        .next_packet_delay = out_data[1] > 0 ? out_data[1] : 200,
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
    .init              = (init_pred_f) init_pred_dummy,
    .feed              = (feed_pred_f) feed_pred_dummy,
    .predict           = (predict_pred_f) surrogate_torch_predict,
    .predict_rc        = (predict_pred_rc_f) predict_latency_rc_dummy,
    .predictor_data_sz = 0
};
