#include "zmqmlrequester.h"

#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <cmath>
#include <numeric>
#include <regex>

#include <zmq.hpp>
#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"

using namespace std;
using namespace rapidjson;

static string endpoint = "tcp://localhost:5555";
static int debug = 0;

/**
 * See zmqmlrequester.h
 */
vector<string> zmqml_request(const string& cmd,
                             const vector<string>& args,
                             const string& bindata) {
    zmq::context_t context(1);
    zmq::socket_t socket(context, ZMQ_REQ);
    socket.connect(endpoint);

    Document msg;
    msg.SetObject();
    auto& allocator = msg.GetAllocator();

    Value cmdValue;
    cmdValue.SetString(cmd.c_str(), cmd.length(), msg.GetAllocator());
    msg.AddMember("cmd", cmdValue, msg.GetAllocator());

    if (args == std::vector<std::string>()) {
        Value argsArray(kArrayType);
        argsArray.PushBack(Value("dummy", allocator), allocator);
        msg.AddMember("args", argsArray, allocator);
    } else {
        Value argsArray(kArrayType);
        for(const auto& arg: args) {
            argsArray.PushBack(Value(arg.c_str(), allocator), allocator);
        }
        msg.AddMember("args", argsArray, allocator);
    }

    StringBuffer buffer;
    Writer<StringBuffer> writer(buffer);
    msg.Accept(writer);

	if (debug) cout << buffer.GetString() << endl;

    string bufferstr = buffer.GetString();
    const char delimiter = '\0';
    string jsonbinmsg = bufferstr + delimiter + bindata;

    zmq::message_t reqmsg(jsonbinmsg.begin(), jsonbinmsg.end());
    socket.send(reqmsg, zmq::send_flags::none);

    zmq::message_t reply;
    socket.recv(reply);

    string tmp(static_cast<char*>(reply.data()), reply.size());
    Document response;
    response.Parse(tmp.c_str());

    vector<string> ret;

    if (response.HasMember("status")) {
        ret.push_back(response["status"].GetString());

        if (response.HasMember("et")) {
            ret.push_back(response["et"].GetString());
        }

        if (response.HasMember("id")) {
            ret.push_back(response["id"].GetString());
        }
    } else {
        ret.push_back("failed");
    }

    return ret;
}




#if 0
/**
 * @brief Finds all occurrences of a regex pattern within a given
 * input string and returns them.
 *
 * This function searches for all matches of the `pattern` within the
 * `input` string, extracting the first captured group from each
 * match. Each match found by applying the regular expression is added
 * to a vector of strings, which is then returned.
 *
 * @param pattern The regular expression pattern to search for within
 * the input string. The pattern should include at least one capturing group.
 * @param input The string to search within for the pattern.
 * @return A `std::vector<std::string>` containing all the matches
 *         found. Each element in the vector is the first captured
 *         group from a match of the pattern in the input.
 */
static std::vector<std::string> findall(const std::string& pattern, const std::string& input) {
    std::vector<std::string> matches;
    std::regex re(pattern);
    auto words_begin = std::sregex_iterator(input.begin(), input.end(), re);
    auto words_end = std::sregex_iterator();

    for (auto it = words_begin; it != words_end; ++it) {
        std::smatch match = *it;
        matches.push_back(match.str(1)); // Extract the first captured group
    }
    return matches;
}
#endif
