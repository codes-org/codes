#ifndef __ZMQREQUESTER_H_DEFINED__
#define __ZMQREQUESTER_H_DEFINED__

#include <string>
#include <vector>

/**
 * @brief Sends a request over ZeroMQ with the specified command and arguments,
 *        receives a reply
 *
 * This function constructs a JSON message with the provided command
 * and arguments, sends it over a ZeroMQ REQ socket, waits for the
 * reply, parses the JSON response, and extracts the 'status', 'et'
 * (if present), and 'id' (if present) fields. It constructs a vector
 * of strings containing these fields for the return value. If the
 * 'status' field is not present in the response, it returns a vector
 * containing "failed".
 *
 * @param cmd zmqml request command: 'query', 'launch', execute', send', 'nothing', 'exit'
 * @param args the arguments for launch and execute
 * @param bindata binary data from send
 * @return vector<string> A vector containing the 'status' field and
 *         optionally 'et' and 'id'.  'status' is not present, returns
 *         a vector with "failed".
 *
 * @exception std::runtime_error Thrown if there are any issues with ZeroMQ communication.
 * @exception rapidjson::ParseErrorException Thrown if parsing the JSON response fails.
 * @note This function assumes that the 'endpoint' variable (used in
 *       socket.connect) is defined externally and is accessible
 *       within this function scope. Ensure 'endpoint' is properly
 *       configured before calling this function.
 * @note If 'debug' is true, the JSON message sent is printed to standard output.
 */
extern std::vector<std::string> zmqml_request(const std::string& cmd,
                                              const std::vector<std::string>& args = std::vector<std::string>(),
                                              const std::string& bindata = "None"
                                              );
#endif
