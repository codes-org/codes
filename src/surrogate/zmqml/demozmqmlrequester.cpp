#include "zmqmlrequester.h"

#include <iostream>
#include <string>
#include <vector>
#include <cmath>
#include <chrono>
#include <thread>

using namespace std;

static void test_blockingcall() {
    vector<string> args = {"sleep", "1"};
    vector<string> result = zmqml_request("execute", args);

    cout << "status:" << result[0] << endl;
}


#if 0
static void test_nonblockingcall() {
    vector<string> args = {"sleep", "3"};
    vector<string> ret = zmqml_request("launch", args);
    
    string status = ret[0];
    int id = ret[1];
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
#endif

static void measure_latency() {
    cout << "measure latency" << endl;
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

int main () {

    test_blockingcall();

    //test_nonblockingcall();

    measure_latency();

    zmqml_request("exit");
    return 0;
}
