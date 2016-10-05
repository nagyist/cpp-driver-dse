/*
  Copyright (c) 2016 DataStax, Inc.

  This software can be used solely with DataStax Enterprise. Please consult the
  license at http://www.datastax.com/terms/datastax-dse-driver-license-terms
*/
#include "scassandra_cluster.hpp"
#include "exception.hpp"
#include "test_utils.hpp"

#include "scoped_lock.hpp"
#include "socket.hpp"

#include <sstream>

#define SCASSANDRA_IP_PREFIX "127.0.0."
#define SCASSANDRA_BINARY_PORT 9042
#define SCASSANDRA_LOG_LEVEL "DEBUG"
#define SCASSANDRA_REST_PORT_OFFSET 50000
#define SCASSANDRA_SPAWN_COMMAND_LENGTH 9
#define SCASSANDRA_SPAWN_COMMAND_BUFFER 64

#define HTTP_EOL "\r\n"
#define OUTPUT_BUFFER_SIZE 10240
#define SCASSANDRA_NAP 100
#define SCASSANDRA_CONNECTION_RETRIES 600 // Up to 60 seconds for retry based on SCASSANDRA_NAP

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
  char** spawn_command;

  Process(unsigned int node)
    : is_running(false) {

    // Create the spawn command
    spawn_command = new char*[SCASSANDRA_SPAWN_COMMAND_LENGTH];
    for (unsigned int n = 0; n < (SCASSANDRA_SPAWN_COMMAND_LENGTH - 1); ++n) {
      spawn_command[n] = new char[SCASSANDRA_SPAWN_COMMAND_BUFFER]();
    }
    snprintf(spawn_command[0], SCASSANDRA_SPAWN_COMMAND_BUFFER - 1, "java");
    snprintf(spawn_command[1], SCASSANDRA_SPAWN_COMMAND_BUFFER - 1, "-jar");
    snprintf(spawn_command[2], SCASSANDRA_SPAWN_COMMAND_BUFFER - 1,
      "-Dscassandra.log.level=%s", SCASSANDRA_LOG_LEVEL);
    snprintf(spawn_command[3], SCASSANDRA_SPAWN_COMMAND_BUFFER - 1,
      "-Dscassandra.admin.listen-address=%s%u", SCASSANDRA_IP_PREFIX, node);
    snprintf(spawn_command[4], SCASSANDRA_SPAWN_COMMAND_BUFFER - 1,
      "-Dscassandra.admin.port=%u", (node + SCASSANDRA_REST_PORT_OFFSET));
    snprintf(spawn_command[5], SCASSANDRA_SPAWN_COMMAND_BUFFER - 1,
      "-Dscassandra.binary.listen-address=%s%u", SCASSANDRA_IP_PREFIX, node);
    snprintf(spawn_command[6], SCASSANDRA_SPAWN_COMMAND_BUFFER - 1,
      "-Dscassandra.binary.port=%u", SCASSANDRA_BINARY_PORT);
    snprintf(spawn_command[7], SCASSANDRA_SPAWN_COMMAND_BUFFER - 1,
      SCASSANDRA_SERVER_JAR);
    spawn_command[8] = NULL;
  }

  ~Process() {
    // Clean up the memory for the spawn command
    for (unsigned int n = 0; n < (SCASSANDRA_SPAWN_COMMAND_LENGTH - 1); ++n) {
      delete[] spawn_command[n];
    }
    delete[] spawn_command;
  }
};

// Initialize the mutex
uv_mutex_t test::SCassandraCluster::mutex_;

test::SCassandraCluster::SCassandraCluster() {
  // Initialize the mutex
  uv_mutex_init(&mutex_);
}

test::SCassandraCluster::~SCassandraCluster() {
  stop_cluster();
}

void test::SCassandraCluster::create_cluster(unsigned int nodes /*= 1*/) {
  // Create the process for each node
  for (unsigned int n = 1; n <= nodes; ++n) {
    processes_.insert(ProcessPair(n, new Process(n)));
  }
}

std::string test::SCassandraCluster::get_ip_prefix() const {
  return SCASSANDRA_IP_PREFIX;
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
  for (ProcessMap::iterator iterator = processes_.begin();
    iterator != processes_.end(); ++iterator) {
    // Start the node but do not wait for the node to be up
    start_node(iterator->first, false);
  }

  // Determine if the cluster is ready
  for (ProcessMap::iterator iterator = processes_.begin();
    iterator != processes_.end(); ++iterator) {
    if (!is_node_up(iterator->first)) {
      return false;
    }
  }
  return true;
}

bool test::SCassandraCluster::stop_cluster() {
  for (ProcessMap::iterator iterator = processes_.begin();
    // Stop the node but do not wait for the node to be down
    iterator != processes_.end(); ++iterator) {
    stop_node(iterator->first, false);
  }

  // Determine if the cluster is down
  for (ProcessMap::iterator iterator = processes_.begin();
    iterator != processes_.end(); ++iterator) {
    if (!is_node_down(iterator->first)) {
      return false;
    }
  }
  return true;
}

bool test::SCassandraCluster::start_node(unsigned int node,
  bool wait_for_up /*true*/) {
  ProcessMap::const_iterator iterator = processes_.find(node);

  // Make sure node is valid
  if (iterator == processes_.end()) {
    return false;
  }

  // Ensure the process is not already running
  SharedPtr<Process> process = iterator->second;
  if (!process->is_running) {
    // Create SCassandra process (threaded)
    uv_thread_create(&process->thread, handle_thread_create, process.get());
  }

  // Wait for the node to become available
  if (wait_for_up) {
    return is_node_up(node);
  }
  return true;
}

bool test::SCassandraCluster::stop_node(unsigned int node,
  bool wait_for_down /*true*/) {
  ProcessMap::const_iterator iterator = processes_.find(node);

  // Make sure node is valid
  if (iterator == processes_.end()) {
    return false;
  }

  // Ensure the process is running
  SharedPtr<Process> process = iterator->second;
  if (process->is_running) {
    // TODO: Add exceptions if the process can't be killed and thread joined
    // TODO: Utilize the socket class to ensure the node is down
    int error_code = uv_process_kill(&process->process, SIGTERM);
    if (error_code != 0) {
      return false;
    }
    error_code = uv_thread_join(&process->thread);
    return error_code == 0;
  }

  // Wait for the node to become unavailable
  if (wait_for_down) {
    return is_node_down(node);
  }
  return true;
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

  // Create the options for the process
  options.args = process->spawn_command;
  options.exit_cb = handle_exit;
  options.file = process->spawn_command[0];

  // Start the process
#if UV_VERSION_MAJOR == 0
  int error_code = uv_spawn(loop, &process->process, options);
  uv_err_t error = uv_last_error(loop);
  error_code = error.code;
#else
  int error_code = uv_spawn(&loop, &process->process, &options);
#endif
  if (error_code == 0) {
    LOG("Launched " << process->spawn_command[0] << " with ID "
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
  // Update the HTTP request with the host (address) information
  std::stringstream address;
  address << SCASSANDRA_IP_PREFIX << node << ":"
    << (node + SCASSANDRA_REST_PORT_OFFSET);
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
    mg_printf(nc, "%s", http_message.c_str());
  }

  // Handle/Poll the HTTP request
  while (http_request->exit_status == 0) {
    mg_mgr_poll(&mgr, 1000);
  }
  mg_mgr_free(&mgr);
}

bool test::SCassandraCluster::is_node_available(unsigned int node) {
  // Determine the IP address and port from the node requested
  std::stringstream ip_address;
  ip_address << SCASSANDRA_IP_PREFIX << node;

  return is_node_available(ip_address.str(),
    (node + SCASSANDRA_REST_PORT_OFFSET));
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