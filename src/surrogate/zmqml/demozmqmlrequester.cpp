#include "zmqmlrequester.h"

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <cmath>
#include <chrono>
#include <thread>

using namespace std;

static void test_blockingcall() {
    cout << "* test_blockingcall" << endl;
    
    vector<string> args = {"sleep", "1"};
    vector<string> result = zmqml_request("execute", args);

    cout << "status:" << result[0] << endl;
}

static void test_nonblockingcall() {
    cout << "* test_nonblockingcall" << endl;

    vector<string> args = {"sleep", "3"};
    vector<string> ret = zmqml_request("launch", args);
    
    string status = ret[0];
    string id = ret[1];
    cout << "status=" << status << " id=" << id << endl;

    int cnt = 0;
    while (true) {
        ret = zmqml_request("query", {id});
        status = ret[0];
        cout << "status=" << status << endl;
        if (status == "done") {
            break;
        }
        this_thread::sleep_for(chrono::milliseconds(500));
        cnt++;
    }
    cout << "done cnt=" << cnt << endl;
}

static void test_send_binary() {
    cout << "* test_send_binary" << endl;
    
    string data;
    ifstream file("model/ml-model.pt", ios::binary);

    if (file) {
        file.seekg(0, ios::end);
        data.resize(file.tellg());
        file.seekg(0, ios::beg);
        file.read(&data[0], data.size());
        file.close();
    } else {
        cerr << "Failed to open the file." << endl;
        return;
    }

    vector<string> ret = zmqml_request("send",
                                       {"tmptestsend.dat"}, // dest filename
                                       data);
    string status = ret[0];
    cout << "status=" << status << endl;
}

static void measure_latency() {
    cout << "* measure_latency" << endl;

    vector<double> tss;

    int n = 1000;
    for (int i = 0; i < n; ++i) {
        auto start_time = chrono::steady_clock::now();
        vector<string> result = zmqml_request("nothing");
        auto end_time = chrono::steady_clock::now();
        auto duration = chrono::duration<double>(end_time - start_time).count();
        tss.push_back(duration);
    }
    double sum = 0;
    for (double ts : tss) sum += ts;
    double mean = sum / tss.size();
    double sum_sq_diff = 0;
    for (double ts : tss) sum_sq_diff += (ts - mean) * (ts - mean);
    double std_dev = sqrt(sum_sq_diff / tss.size());
    cout << "zmqcmd latency: mean = " << mean << ", std deviation = " << std_dev << endl;
}


void test_mlpacketdelay_training() {
    std::cout << "* test_mlpacketdelay_training" << std::endl;

    vector<string> args = {"mlpacketdelay_training", 
                           "--method", "MLP", "--epoch", "1",
                            "--input-file", "model/packets-delay.csv",
                            "--model-path", "ml-model.pt"};

    vector<string> ret = zmqml_request("launch", args);
    
    string status = ret[0];
    string id = ret[1];
    cout << "status=" << status << " id=" << id << endl;

    int cnt = 0;
    while (true) {
        ret = zmqml_request("query", {id});
        status = ret[0];
        cout << "status=" << status << endl;
        if (status == "done") {
            break;
        }
        this_thread::sleep_for(chrono::milliseconds(500));
        cnt++;
    }
    cout << "done cnt=" << cnt << endl;
}



int main () {
    if(0) {
        test_send_binary();
        test_blockingcall();
        test_nonblockingcall();
        measure_latency();
    }

    test_mlpacketdelay_training();
    
    zmqml_request("exit");
    return 0;
}
