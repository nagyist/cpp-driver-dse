/*
  Copyright (c) 2016 DataStax, Inc.

  This software can be used solely with DataStax Enterprise. Please consult the
  license at http://www.datastax.com/terms/datastax-dse-driver-license-terms
*/
#include "scassandra_cluster.hpp"
#include "options.hpp"
#include "test_utils.hpp"

#include "scoped_lock.hpp"
#include "socket.hpp"
#include "values/uuid.hpp"
#include "objects/uuid_gen.hpp"

#include <algorithm>
#include <sstream>

#define SCASSANDRA_IP_PREFIX "127.0."
#define SCASSANDRA_BINARY_PORT 9042
#define SCASSANDRA_LOG_LEVEL "DEBUG"
#define SCASSANDRA_REST_PORT_NODE_OFFSET 50000
#define SCASSANDRA_SPAWN_COMMAND_LENGTH 9
#define SCASSANDRA_SPAWN_COMMAND_BUFFER 64

#define HTTP_EOL "\r\n"
#define OUTPUT_BUFFER_SIZE 10240
#define SCASSANDRA_NAP 100
#define SCASSANDRA_CONNECTION_RETRIES 600 // Up to 60 seconds for retry based on SCASSANDRA_NAP
#define SCASSANDRA_PROCESS_RETRIS 100 // Up to 10 seconds for retry based on SCASSANDRA_NAP
#define MAX_TOKEN static_cast<uint64_t>(INT64_MAX) - 1
#define DATA_CENTER_PREFIX "dc"

/**
 * HTTP request method
 */
enum HttpRequestMethod {
  /**
   * HTTP DELETE request method
   */
  HTTP_REQUEST_METHOD_DELETE = 0,
  /**
   * HTTP GET request method
   */
  HTTP_REQUEST_METHOD_GET,
  /**
   * HTTP POST request method
   */
  HTTP_REQUEST_METHOD_POST
} HttpRequestMethod_;

/**
 * HTTP request
 */
struct HttpRequest {
  /**
   * Host address (ip:port)
   */
  std::string address;
  /**
   * Exit status (or error) for the HTTP request
   */
  int exit_status;
  /**
   * JSON message to put in the POST request contents
   */
  std::string content;
  /**
   * The endpoint (URI) for the request
   */
  std::string endpoint;
  /**
   * HTTP request type to use for the request
   */
  HttpRequestMethod method;
  /**
   * Response for HTTP request
   */
  std::string response;
} HttpRequest_;

/**
 * Process information for the SCassandra spawned instance
 */
struct Process {
  /**
   * Data center for SCassandra process
   */
  unsigned int data_center;
  /**
   * Node for SCassandra process
   */
  unsigned int node;
  /**
   * Host ID for the SCassandra process (node information)
   */
  std::string host_id;
  /**
   * IPv4 address SCassandra process is listening on
   */
  std::string listen_address;
  /**
   * SCassandra admin port for interacting through REST API
   */
  unsigned short admin_port;
  /**
   * Flag to check if the SCassandra process is running (spawned)
   */
  bool is_running;
  /**
   * Process information for spawned SCassandra
   */
  uv_process_t process;
  /**
   * Thread to utilize for SCassandra process
   */
  uv_thread_t thread;
  /**
   * Generated command to execute for starting SCassandra process
   */
  std::vector<std::string> spawn_command;

  /**
   * Initialize the process instance; define the application spawn command
   *
   * @param data_center Data center the SCassandra instance will be part of
   * @param node Node in the data center
   */
  Process(unsigned int data_center, unsigned int node)
    : data_center(data_center)
    , node(node)
    , is_running(false) {
    // Create the listen address and admin port for the node
    std::stringstream listen_address_s;
    listen_address_s << SCASSANDRA_IP_PREFIX << (data_center - 1) << "." << node;
    listen_address = listen_address_s.str();
    admin_port = node + SCASSANDRA_REST_PORT_NODE_OFFSET;
    test::driver::Uuid uuid = test::driver::UuidGen().generate_random_uuid();
    host_id = uuid.str();

    // Create the spawn command
    std::stringstream argument;
    spawn_command.push_back("java");
    spawn_command.push_back("-jar");
    argument << "-Dscassandra.log.level=" << SCASSANDRA_LOG_LEVEL;
    spawn_command.push_back(argument.str());
    argument.str("");
    argument << "-Dscassandra.admin.listen-address=" << listen_address;
    spawn_command.push_back(argument.str());
    argument.str("");
    argument << "-Dscassandra.admin.port=" << admin_port;
    spawn_command.push_back(argument.str());
    argument.str("");
    argument << "-Dscassandra.binary.listen-address=" << listen_address;
    spawn_command.push_back(argument.str());
    argument.str("");
    argument << "-Dscassandra.binary.port=" << SCASSANDRA_BINARY_PORT;
    spawn_command.push_back(argument.str());
    argument.str("");
    spawn_command.push_back(SCASSANDRA_SERVER_JAR);
  }
};

// Initialize the mutex and default data center nodes
uv_mutex_t test::SCassandraCluster::mutex_;
const unsigned int DEFAULT_NODE[] = { 1 };
const std::vector<unsigned int> test::SCassandraCluster::DEFAULT_DATA_CENTER_NODES(
  DEFAULT_NODE, DEFAULT_NODE + sizeof(DEFAULT_NODE) / sizeof(DEFAULT_NODE[0]));

test::SCassandraCluster::SCassandraCluster() {
  // Initialize the mutex
  uv_mutex_init(&mutex_);

  // Determine if SCassandra file exists
  if (!test::Utils::file_exists(SCASSANDRA_SERVER_JAR)) {
    std::stringstream message;
    message << "Unable to find SCassandra JAR file ["
      << SCASSANDRA_SERVER_JAR << "]";
    throw Exception(message.str());
  }

  // Determine the release version (for priming nodes)
  if (Options::is_dse()) {
    CCM::DseVersion dse_version(Options::server_version());
    CCM::CassVersion cass_version = dse_version.get_cass_version();
    if (cass_version == "0.0.0") {
      throw Exception("Unable to determine Cassandra version from DSE version");
    }
    release_version_ = test::Utils::replace_all(cass_version.to_string(), "-",
      ".");
  }

  // Generate the schema version (for priming nodes)
  test::driver::Uuid uuid = test::driver::UuidGen().generate_random_uuid();
  schema_version_ = uuid.str();
}

test::SCassandraCluster::~SCassandraCluster() {
  peers_.clear();
  stop_cluster();
}

std::string test::SCassandraCluster::cluster_contact_points(bool is_all /*= true*/) {
  std::stringstream contact_points;
  for (ProcessMap::iterator iterator = processes_.begin();
    iterator != processes_.end(); ++iterator) {
    // Determine if all the nodes are being returned or just the available nodes
    Process process = iterator->second;
    if (is_all || is_node_up(iterator->first)) {
      if (!contact_points.str().empty()) {
        contact_points << ",";
      }
      contact_points << get_ip_prefix(process.data_center) << process.node;
    }
  }
  return contact_points.str();
}

void test::SCassandraCluster::create_cluster(
  std::vector<unsigned int> data_center_nodes /*= DEFAULT_DATA_CENTER_NODES*/) {
  create_processes(data_center_nodes);

  // Create the peers mapping for easy lookup
  for (ProcessMap::iterator outer = processes_.begin();
    outer != processes_.end(); ++outer) {
    unsigned int node = outer->first;
    std::vector<Process*> processes;
    for (ProcessMap::iterator inner = processes_.begin();
      inner != processes_.end(); ++inner) {
      if (inner->first != node) {
        if (inner->second.data_center == outer->second.data_center) {
          processes.push_back(&inner->second);
        }
      }
    }
    peers_.insert(PeersPair(node, processes));
  }
}

void test::SCassandraCluster::create_cluster(unsigned int data_center_one_nodes,
  unsigned int data_center_two_nodes /*= 0*/) {
  std::vector<unsigned int> data_center_nodes;
  if (data_center_one_nodes > 0) {
    data_center_nodes.push_back(data_center_one_nodes);
  }
  if (data_center_two_nodes > 0) {
    data_center_nodes.push_back(data_center_two_nodes);
  }
  create_cluster(data_center_nodes);
}

std::string test::SCassandraCluster::get_ip_prefix(
  unsigned int data_center /*= 1*/) const {
  if (data_center == 0) {
    throw Exception("Invalid Data Center: Must be greater than zero");
  }

  std::stringstream ip_prefix;
  ip_prefix << SCASSANDRA_IP_PREFIX << (data_center - 1) << ".";
  return ip_prefix.str();
}

bool test::SCassandraCluster::is_node_down(unsigned int node) {
  // Attempt to connect to the binary port on the node
  unsigned int number_of_retries = 0;
  while (number_of_retries++ < SCASSANDRA_CONNECTION_RETRIES) {
    if (!is_node_available(node)) {
      return true;
    } else {
      LOG("Connected to Node " << node
        << " in Cluster: Rechecking node down status ["
        << number_of_retries << "]");
      test::Utils::msleep(SCASSANDRA_NAP);
    }
  }

  // Connection can still be established on node
  return false;
}

bool test::SCassandraCluster::is_node_up(unsigned int node) {
  // Attempt to connect to the binary port on the node
  unsigned int number_of_retries = 0;
  while (number_of_retries++ < SCASSANDRA_CONNECTION_RETRIES) {
    if (is_node_available(node)) {
      return true;
    } else {
      LOG("Unable to Connect to Node " << node
        << " in Cluster: Rechecking node up status ["
        << number_of_retries << "]");
      test::Utils::msleep(SCASSANDRA_NAP);
    }
  }

  // Connection cannot be established on node
  return false;
}

bool test::SCassandraCluster::start_cluster() {
  // Start each SCassandra node/process
  for (ProcessMap::iterator iterator = processes_.begin();
    iterator != processes_.end(); ++iterator) {
    // Start the node but do not wait for the node to be up
    start_node(iterator->first, false);
  }

  // Wait for each SCassandra node/process to be running
  for (ProcessMap::iterator iterator = processes_.begin();
    iterator != processes_.end(); ++iterator) {
    while (!iterator->second.is_running) {
      test::Utils::msleep(SCASSANDRA_NAP);
    }
  }

  // Determine if the cluster is ready
  for (ProcessMap::iterator iterator = processes_.begin();
    iterator != processes_.end(); ++iterator) {
    try {
      if (!is_node_up(iterator->first)) {
        return false;
      }
    } catch (Exception e) {
      LOG_ERROR(e.what());
      return false;
    }
  }
  return true;
}

bool test::SCassandraCluster::stop_cluster() {
  // Stop each SCassandra node/process
  for (ProcessMap::iterator iterator = processes_.begin();
    // Stop the node but do not wait for the node to be down
    iterator != processes_.end(); ++iterator) {
    stop_node(iterator->first, false);
  }

  // Wait for each SCassandra node/process to be finished
  for (ProcessMap::iterator iterator = processes_.begin();
    iterator != processes_.end(); ++iterator) {
    while (iterator->second.is_running) {
      test::Utils::msleep(SCASSANDRA_NAP);
    }
  }

  // Determine if the cluster is down
  for (ProcessMap::iterator iterator = processes_.begin();
    iterator != processes_.end(); ++iterator) {
    try {
      if (!is_node_down(iterator->first)) {
        return false;
      }
    } catch (Exception e) {
      LOG_ERROR(e.what());
      return false;
    }
  }
  return true;
}

bool test::SCassandraCluster::start_node(unsigned int node,
  bool wait_for_up /*= true*/) {
  ProcessMap::iterator iterator = processes_.find(node);

  // Make sure node is valid
  if (iterator == processes_.end()) {
    return false;
  }

  // Ensure the process is not already running
  Process* process = &iterator->second;
  if (!process->is_running) {
    // Create SCassandra process (threaded)
    uv_thread_create(&process->thread, handle_thread_create, process);
  }

  // Wait for the node to become available
  if (wait_for_up) {
    return is_node_up(node);
  }
  return true;
}

bool test::SCassandraCluster::stop_node(unsigned int node,
  bool wait_for_down /*= true*/) {
  ProcessMap::iterator iterator = processes_.find(node);

  // Make sure node is valid
  if (iterator == processes_.end()) {
    return false;
  }

  // Ensure the process is running
  Process process = iterator->second;
  if (process.is_running) {
    // TODO: Add exceptions if the process can't be killed
    int error_code = uv_process_kill(&process.process, SIGTERM);
    if (error_code != 0) {
      return false;
    }
  }

  // Wait for the node to become unavailable
  if (wait_for_down) {
    return is_node_down(node);
  }
  return true;
}

std::vector<unsigned int> test::SCassandraCluster::nodes(
  bool is_available /*= true*/) {
  std::vector<unsigned int> nodes;
  for (ProcessMap::iterator iterator = processes_.begin();
    iterator != processes_.end(); ++iterator) {
    if (!is_available || iterator->second.is_running) {
      nodes.push_back(iterator->first);
    }
  }
  return nodes;
}

void test::SCassandraCluster::prime_system_tables() {
  for (ProcessMap::iterator iterator = processes_.begin();
    iterator != processes_.end(); ++iterator) {
    if (iterator->second.is_running) {
      prime_system_tables(iterator->first);
    }
  }
}

void test::SCassandraCluster::prime_query(PrimingRequest request) {
  for (ProcessMap::iterator iterator = processes_.begin();
    iterator != processes_.end(); ++iterator) {
    prime_query(iterator->first, request);
  }
}

void test::SCassandraCluster::prime_query(unsigned int node,
  PrimingRequest request) {
  rest_server_post(node, request.json(), "prime-query-single");
}

SharedPtr<rapidjson::Document> test::SCassandraCluster::primed_queries(
  unsigned int node) {
  std::string json_reponse = rest_server_get(node, "prime-query-single");
  SharedPtr<rapidjson::Document> document = new rapidjson::Document();
  document->Parse(json_reponse.c_str());
  return document;
}

void test::SCassandraCluster::remove_primed_queries() {
  for (ProcessMap::iterator iterator = processes_.begin();
    iterator != processes_.end(); ++iterator) {
    remove_primed_queries(iterator->first);
  }
}

void test::SCassandraCluster::remove_primed_queries(unsigned int node) {
  rest_server_delete(node, "prime-query-single");
}

#if UV_VERSION_MAJOR == 0
void test::SCassandraCluster::handle_exit(uv_process_t* process, int error_code,
  int term_signal) {
#else
void test::SCassandraCluster::handle_exit(uv_process_t* process,
  int64_t error_code, int term_signal) {
#endif
  cass::ScopedMutex lock(&mutex_);
  LOG("Process " << process->pid << " Terminated: " << error_code);
  uv_close(reinterpret_cast<uv_handle_t*>(process), NULL);
}

#if UV_VERSION_MAJOR == 0
uv_buf_t test::SCassandraCluster::handle_allocation(uv_handle_t* handle,
  size_t suggested_size) {
  cass::ScopedMutex lock(&mutex_);
  return uv_buf_init(new char[suggested_size], suggested_size);
#else
void test::SCassandraCluster::handle_allocation(uv_handle_t* handle,
  size_t suggested_size, uv_buf_t* buffer) {
  cass::ScopedMutex lock(&mutex_);
  buffer->base = new char[OUTPUT_BUFFER_SIZE];
  buffer->len = OUTPUT_BUFFER_SIZE;
#endif
}

void test::SCassandraCluster::handle_thread_create(void* arg) {
  Process* process = static_cast<Process*>(arg);

  // Initialize the loop and process arguments
#if UV_VERSION_MAJOR == 0
  uv_loop_t* loop = uv_loop_new();
#else
  uv_loop_t loop;
  uv_loop_init(&loop);
#endif
  uv_process_options_t options;
  memset(&options, 0, sizeof(uv_process_options_t));

  // Create the options for reading information from the spawn pipes
  uv_pipe_t standard_output;
  uv_pipe_t error_output;
#if UV_VERSION_MAJOR == 0
  uv_pipe_init(loop, &standard_output, 0);
  uv_pipe_init(loop, &error_output, 0);
#else
  uv_pipe_init(&loop, &standard_output, 0);
  uv_pipe_init(&loop, &error_output, 0);
#endif
  uv_stdio_container_t stdio[3];
  options.stdio_count = 3;
  options.stdio = stdio;
  options.stdio[0].flags = UV_IGNORE;
  options.stdio[1].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
  options.stdio[1].data.stream = (uv_stream_t*) &standard_output;
  options.stdio[2].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
  options.stdio[2].data.stream = (uv_stream_t*) &error_output;

  // Generate the spawn command for use with uv_spawn
  const char* spawn_command[SCASSANDRA_SPAWN_COMMAND_LENGTH];
  size_t index = 0;
  for (std::vector<std::string>::iterator iterator = process->spawn_command.begin();
  iterator != process->spawn_command.end(); ++iterator) {
    spawn_command[index++] = iterator->data();
  }
  spawn_command[index] = NULL;

  // Create the options for the process
  options.args = const_cast<char**>(spawn_command);
  options.exit_cb = handle_exit;
  options.file = spawn_command[0];

  // Start the process
#if UV_VERSION_MAJOR == 0
  int error_code = uv_spawn(loop, &process->process, options);
  uv_err_t error = uv_last_error(loop);
  error_code = error.code;
#else
  int error_code = uv_spawn(&loop, &process->process, &options);
#endif
  if (error_code == 0) {
    LOG("Launched " << spawn_command[0] << " with ID "
      << process->process.pid);

    // Start the output thread loops
    uv_read_start(reinterpret_cast<uv_stream_t*>(&standard_output),
      handle_allocation, handle_read);
    uv_read_start(reinterpret_cast<uv_stream_t*>(&error_output),
      handle_allocation, handle_read);

    // Start the process loop
    process->is_running = true;
#if UV_VERSION_MAJOR == 0
    uv_run(loop, UV_RUN_DEFAULT);
    uv_loop_delete(loop);
#else
    uv_run(&loop, UV_RUN_DEFAULT);
    uv_loop_close(&loop);
#endif
    process->is_running = false;
  } else {
#if UV_VERSION_MAJOR == 0
    LOG_ERROR(uv_strerror(error));
#else
    LOG_ERROR(uv_strerror(error_code));
#endif
  }
}

#if UV_VERSION_MAJOR == 0
void test::SCassandraCluster::handle_read(uv_stream_t* stream,
  ssize_t buffer_length, uv_buf_t buffer) {
#else
void test::SCassandraCluster::handle_read(uv_stream_t* stream,
  ssize_t buffer_length, const uv_buf_t* buffer) {
#endif
  cass::ScopedMutex lock(&mutex_);
  if (buffer_length > 0) {
    // Process the buffer and log it
#if UV_VERSION_MAJOR == 0
    std::string message(buffer.base, buffer_length);
#else
    std::string message(buffer->base, buffer_length);
#endif
    LOG(Utils::trim(message));
  } else if (buffer_length < 0) {
#if UV_VERSION_MAJOR == 0
    uv_err_t error = uv_last_error(stream->loop);
    if (error.code == UV_EOF) // uv_close if UV_EOF
#endif
    uv_close(reinterpret_cast<uv_handle_t*>(stream), NULL);
  }

  // Clean up the memory allocated
#if UV_VERSION_MAJOR == 0
  delete[] buffer.base;
#else
  delete[] buffer->base;
#endif
}

void test::SCassandraCluster::handle_connected(struct mg_connection* nc, int ev,
  void* ev_data) {
  http_message *hm = static_cast<http_message*>(ev_data);
  HttpRequest* http_request = static_cast<HttpRequest*>(nc->user_data);

  // Handle the event responses
  switch (ev) {
    case MG_EV_CONNECT:
      // Ensure the connection was established
      int status;
      status = *static_cast<int*>(ev_data);
      if (status != 0) {
        std::cerr << "Error Establishing Connection: " << strerror(status)
          << std::endl;
        http_request->exit_status = -status;
      }
      break;
    case MG_EV_HTTP_REPLY:
      // Gather the server response and close the connection
      http_request->response = std::string(hm->body.p, hm->body.len);
      nc->flags |= MG_F_SEND_AND_CLOSE;
      http_request->exit_status = 1;
      break;
    default:
      break;
  }
}

void test::SCassandraCluster::create_processes(std::vector<unsigned int> nodes) {
  // Create the process for each data center and the nodes within
  unsigned int data_center = 1;
  unsigned int node = 1;
  for (std::vector<unsigned int>::iterator iterator = nodes.begin();
    iterator != nodes.end(); ++iterator) {
    for (unsigned int n = 1; n <= *iterator; ++n) {
      processes_.insert(ProcessPair(node++, Process(data_center, n)));
    }
    ++data_center;
  }
}

std::string test::SCassandraCluster::generate_http_message(
  HttpRequest* http_request) const {
  // Determine the method of the the request
  std::stringstream http_message;
  if (http_request->method == HTTP_REQUEST_METHOD_DELETE) {
    http_message << "DELETE";
  } else if (http_request->method == HTTP_REQUEST_METHOD_GET) {
    http_message << "GET";
  } else if (http_request->method == HTTP_REQUEST_METHOD_POST) {
    http_message << "POST";
  }
  http_message << " /" << http_request->endpoint << " HTTP/1.1" << HTTP_EOL;

  // Generate the headers
  bool is_post = http_request->method == HTTP_REQUEST_METHOD_POST;
  http_message
    << "Host: " << http_request->address << HTTP_EOL
    << (is_post ? "Content-Type: application/json" HTTP_EOL : "")
    << "Content-Length: "<< ((is_post) ? http_request->content.size() : 0) << HTTP_EOL
    << "Connection: close" << HTTP_EOL << HTTP_EOL
    << (is_post ? http_request->content : "");

  // Return the HTTP message
  return http_message.str();
}

void test::SCassandraCluster::rest_server_delete(unsigned int node,
  const std::string& endpoint) {
  HttpRequest http_request;
  http_request.method = HTTP_REQUEST_METHOD_DELETE;
  http_request.endpoint = endpoint;
  send_http_request(node, &http_request);
}

std::string test::SCassandraCluster::rest_server_get(unsigned int node,
  const std::string& endpoint) const {
  HttpRequest http_request;
  http_request.method = HTTP_REQUEST_METHOD_GET;
  http_request.endpoint = endpoint;
  send_http_request(node, &http_request);
  return http_request.response;
}

void test::SCassandraCluster::rest_server_post(unsigned int node,
  const std::string& content, const std::string& endpoint) {
  HttpRequest http_request;
  http_request.method = HTTP_REQUEST_METHOD_POST;
  http_request.endpoint = endpoint;
  http_request.content = content;
  send_http_request(node, &http_request);
}

void test::SCassandraCluster::send_http_request(unsigned int node,
  HttpRequest* http_request)
  const {
  // Determine if the node is valid and is the process is running
  ProcessMap::const_iterator iterator = processes_.find(node);
  if (iterator == processes_.end()) {
    std::stringstream message;
    message << "Unable to Find Node: Node " << node << " is not a valid node";
    throw test::Exception(message.str());
  }

  // Update the HTTP request with the host (address) information
  std::string ip_address = iterator->second.listen_address;
  unsigned short port = iterator->second.admin_port;
  std::stringstream address;
  address << ip_address << ":" << port;
  http_request->address = address.str();
  http_request->exit_status = 0;

  // Create a connection to the SCassandra REST server
  struct mg_mgr mgr;
  mg_mgr_init(&mgr, NULL);
  struct mg_connect_opts opts;
  memset(&opts, 0, sizeof(opts));
  struct mg_connection* nc = mg_connect_opt(&mgr, http_request->address.c_str(),
    handle_connected, opts);

  // Write the HTTP request to the established connection
  if (!nc) {
    std::cerr << "Unable to Establish Connection: Unknown error occurred"
      << std::endl;
  } else {
    nc->user_data = http_request;
    mg_set_protocol_http_websocket(nc);
    std::string http_message = generate_http_message(http_request);
    LOG_DEBUG(http_request->response);
    mg_printf(nc, "%s", http_message.c_str());
  }

  // Handle/Poll the HTTP request
  while (http_request->exit_status == 0) {
    mg_mgr_poll(&mgr, 1000);
  }
  mg_mgr_free(&mgr);
}

bool test::SCassandraCluster::is_node_available(unsigned int node) {
  // Determine if the node is valid and is the process is running
  ProcessMap::iterator iterator = processes_.find(node);
  if (iterator == processes_.end()) {
    std::stringstream message;
    message << "Unable to Check Availability of Node: Node " << node
      << " is not a valid node";
    throw test::Exception(message.str());
  }

  // Determine if the node is available
  std::string ip_address = iterator->second.listen_address;
  unsigned short port = iterator->second.admin_port;
  return is_node_available(ip_address, port);
}

bool test::SCassandraCluster::is_node_available(const std::string& ip_address,
  unsigned short port) {
  Socket socket;
  try {
    socket.establish_connection(ip_address, port);
    return true;
  } catch (...) {
    ; // No-op
  }

  // Unable to establish connection to node
  return false;
}

std::vector<std::string> test::SCassandraCluster::generate_token_ranges(
  unsigned int data_center, unsigned int nodes) {
  // Create and sort the token ranges
  int offset = (data_center - 1) * 100;
  std::vector<int64_t> token_ranges;
  for(unsigned int n = 0; n < nodes; ++n) {
    int64_t token_range = static_cast<uint64_t>(((UINT64_MAX / nodes) * n)) -
      MAX_TOKEN;
    token_ranges.push_back(token_range + offset);
  }
  std::sort(token_ranges.begin(), token_ranges.end());

  // Convert the token ranges to a vector of strings
  std::vector<std::string> token_ranges_s;
  for(std::vector<int64_t>::iterator iterator = token_ranges.begin();
    iterator != token_ranges.end(); ++iterator) {
    std::stringstream token_range_stream;
    token_range_stream << *iterator;
    token_ranges_s.push_back(token_range_stream.str());
  }
  return token_ranges_s;
}

void test::SCassandraCluster::prime_system_tables(unsigned int node) {
  // Determine if the node is valid and is the process is running
  ProcessMap::iterator process_iterator = processes_.find(node);
  if (process_iterator == processes_.end()) {
    std::stringstream message;
    message << "Unable to Prime System Tables: Node " << node
      << " is not a valid node";
    throw test::Exception(message.str());
  }
  unsigned int data_center = process_iterator->second.data_center;
  unsigned int current_node = process_iterator->second.node;
  std::string host_id = process_iterator->second.host_id;
  std::string node_address = process_iterator->second.listen_address;

  // Determine if the peers are valid (this should never error)
  PeersMap::iterator peers_iterator = peers_.find(node);
  if (peers_iterator == peers_.end()) {
    std::stringstream message;
    message << "Unable to Prime System Tables: Node " << node
      << " did not define peers (this should never happen)";
    throw test::Exception(message.str());
  }
  std::vector<Process*> peers = peers_iterator->second;
  unsigned int nodes = peers.size() + 1;

  // Prime the system.local and system.peers table
  std::stringstream data_center_name;
    data_center_name << DATA_CENTER_PREFIX << data_center;
  std::vector<std::string> token_ranges = generate_token_ranges(data_center,
    nodes);
  std::string token = token_ranges.at(current_node - 1);

  // Prime the system.local table
  PrimingRow row = PrimingRow::builder()
    .add_column("bootstrapped", CASS_VALUE_TYPE_VARCHAR, "COMPLETED")
    .add_column("broadcast_address", CASS_VALUE_TYPE_INET, node_address)
    .add_column("data_center", CASS_VALUE_TYPE_VARCHAR,
      data_center_name.str())
    .add_column("host_id", CASS_VALUE_TYPE_UUID, host_id)
    .add_column("listen_address", CASS_VALUE_TYPE_INET, node_address)
    .add_column("partitioner", CASS_VALUE_TYPE_VARCHAR,
      "org.apache.cassandra.dht.Murmur3Partitioner")
    .add_column("rack", CASS_VALUE_TYPE_VARCHAR, "rack1") //TODO: Allow for multiple racks
    .add_column("release_version", CASS_VALUE_TYPE_VARCHAR, release_version_)
    .add_column("rpc_address", CASS_VALUE_TYPE_INET, node_address)
    .add_column("schema_version", CASS_VALUE_TYPE_UUID, schema_version_)
    .add_column("tokens", "set<text>", "[" + token + "]");
  if (Options::is_dse()) {
    row.add_column("dse_version", CASS_VALUE_TYPE_VARCHAR,
        Options::server_version().to_string())
      .add_column("graph", CASS_VALUE_TYPE_BOOLEAN, "false")
      .add_column("workload", CASS_VALUE_TYPE_VARCHAR, "Cassandra");
  }
  PrimingRequest system_local = PrimingRequest::builder()
    .with_query_pattern("SELECT .* FROM system.local WHERE key='local'")
    .with_consistency(CASS_CONSISTENCY_ANY)
    .with_consistency(CASS_CONSISTENCY_ONE)
    .with_consistency(CASS_CONSISTENCY_TWO)
    .with_consistency(CASS_CONSISTENCY_THREE)
    .with_consistency(CASS_CONSISTENCY_QUORUM)
    .with_consistency(CASS_CONSISTENCY_ALL)
    .with_consistency(CASS_CONSISTENCY_LOCAL_QUORUM)
    .with_consistency(CASS_CONSISTENCY_EACH_QUORUM)
    .with_consistency(CASS_CONSISTENCY_SERIAL)
    .with_consistency(CASS_CONSISTENCY_LOCAL_SERIAL)
    .with_consistency(CASS_CONSISTENCY_LOCAL_ONE)
    .with_rows(PrimingRows::builder().add_row(row));
  prime_query(current_node, system_local);

  // Generate the rows for the system.peers table
  PrimingRows peers_rows = PrimingRows::builder();
  for (std::vector<Process*>::iterator iterator = peers.begin();
    iterator != peers.end(); ++iterator) {
    Process* process = *iterator;
    current_node = process->node;
    host_id = process->host_id;
    node_address = process->listen_address;
    token = token_ranges.at(current_node - 1);

    PrimingRow row = PrimingRow::builder()
      .add_column("data_center", CASS_VALUE_TYPE_VARCHAR, data_center_name.str())
      .add_column("host_id", CASS_VALUE_TYPE_UUID, host_id)
      .add_column("peer", CASS_VALUE_TYPE_INET, node_address)
      .add_column("rack", CASS_VALUE_TYPE_VARCHAR, "rack1") //TODO: Allow for multiple racks
      .add_column("release_version", CASS_VALUE_TYPE_VARCHAR, release_version_)
      .add_column("rpc_address", CASS_VALUE_TYPE_INET, node_address)
      .add_column("schema_version", CASS_VALUE_TYPE_UUID, schema_version_)
      .add_column("tokens", "set<text>", "[" + token + "]");
    if (Options::is_dse()) {
      row.add_column("dse_version", CASS_VALUE_TYPE_VARCHAR,
          Options::server_version().to_string())
        .add_column("graph", CASS_VALUE_TYPE_BOOLEAN, "false")
        .add_column("workload", CASS_VALUE_TYPE_VARCHAR, "Cassandra");
    }
    peers_rows.add_row(row);
  }

  // Prime the system.peers table
  PrimingRequest system_peers = PrimingRequest::builder()
    .with_query_pattern("SELECT .* FROM system.peers")
    .with_consistency(CASS_CONSISTENCY_ANY)
    .with_consistency(CASS_CONSISTENCY_ONE)
    .with_consistency(CASS_CONSISTENCY_TWO)
    .with_consistency(CASS_CONSISTENCY_THREE)
    .with_consistency(CASS_CONSISTENCY_QUORUM)
    .with_consistency(CASS_CONSISTENCY_ALL)
    .with_consistency(CASS_CONSISTENCY_LOCAL_QUORUM)
    .with_consistency(CASS_CONSISTENCY_EACH_QUORUM)
    .with_consistency(CASS_CONSISTENCY_SERIAL)
    .with_consistency(CASS_CONSISTENCY_LOCAL_SERIAL)
    .with_consistency(CASS_CONSISTENCY_LOCAL_ONE)
    .with_rows(peers_rows);
  prime_query(node, system_peers);
}