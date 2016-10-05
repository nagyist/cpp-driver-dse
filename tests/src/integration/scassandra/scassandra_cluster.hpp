/*
  Copyright (c) 2016 DataStax, Inc.

  This software can be used solely with DataStax Enterprise. Please consult the
  license at http://www.datastax.com/terms/datastax-dse-driver-license-terms
*/

#ifndef __TEST_SCASSANDRA_CLUSTER_HPP__
#define __TEST_SCASSANDRA_CLUSTER_HPP__

// Make sure SCassandra server is available during compile time
#ifdef DSE_USE_STANDALONE_SCASSANDRA_SERVER
#include "priming_requests.hpp"
#include "shared_ptr.hpp"

#include <mongoose.h>
#include<rapidjson/document.h>
#include <uv.h>

#include <cstdio>
#include <map>
#include <string>
#include <string.h>

// Forward declare for enums and structs in source
struct HttpRequest;
struct Process;

namespace test {

/**
 * SCassandra cluster for easily creating SCassandra instances/nodes
 */
class SCassandraCluster {
public:
  /**
   * Initialize the SCassandra cluster
   */
  SCassandraCluster();
  /**
   * Terminate all SCassandra clusters and perform any additional cleanup
   * operations
   */
  ~SCassandraCluster();
  /**
   * Create a single data center SCassandra cluster
   *
   * @param nodes Number of nodes in the SCassandr cluster (default: 1)
   */
  void create_cluster(unsigned int nodes = 1);
  /**
   * Get the IPv4 address prefix being utilized for the SCassandr cluster
   *
   * @return IPv4 address prefix used
   */
  std::string get_ip_prefix() const;
  /**
   * Start the SCassandra cluster
   *
   * @return True is cluster is up; false otherwise
   */
  bool start_cluster();
  /**
   * Stop the SCassandra cluster (terminate the SCassandra process)
   *
   * @return True is cluster is down; false otherwise
   */
  bool stop_cluster();
  /**
   * Start a node on the SCassandra cluster
   *
   * NOTE: If node is not valid false is returned
   *
   * @param node Node to start
   * @param wait_for_up True if waiting for node to be up; false otherwise
   * @return True if node was started (and is up if wait_for_up is true); false
   *         otherwise
   */
  bool start_node(unsigned int node, bool wait_for_up = true);
  /**
   * Stop a node on the SCassandra cluster
   *
   * NOTE: If node is not valid false is returned
   *
   * @param node Node to stop
   * @param wait_for_down True if waiting for node to be down; false otherwise
   * @return True if node was stopped (and is dpwn if wait_for_down is true);
   *         false otherwise
   */
  bool stop_node(unsigned int node, bool wait_for_down = true);
  /**
   * Check to see if a node is no longer accepting connections
   *
   * NOTE: This method may check the status of the node multiple times
   *
   * @param node Node to check 'DOWN' status
   * @return True if node is no longer accepting connections; false otherwise
   */
  bool is_node_down(unsigned int node);
  /**
   * Check to see if a node is ready to accept connections
   *
   * NOTE: This method may check the status of the node multiple times
   *
   * @param node Node to check 'UP' status
   * @return True if node is ready to accept connections; false otherwise
   */
  bool is_node_up(unsigned int node);

  /**
   * Prime the queries on SCassandra cluster using the REST API
   *
   * @param request Request to prime
   */
  void prime_query(PrimingRequest request);
  /**
   * Prime the queries on SCassandra using the REST API
   *
   * @param node Node to prime the query on
   * @param request Request to prime
   */
  void prime_query(unsigned int node, PrimingRequest request);
  /**
   * Get the primed queries on a given node is the SCassandra cluster
   *
   * @param node Node to get primed queries
   * @return JSON object representing all the primed queries on the node
   */
  SharedPtr<rapidjson::Document> primed_queries(unsigned int node);
  /**
   * Remove all the primed queries in the SCassandra cluster
   */
  void remove_primed_queries();
  /**
   * Remove the primed queries on a given node in the SCassandra cluster
   *
   * @param node Node to remove primed queries from
   */
  void remove_primed_queries(unsigned int node);

private:
  typedef std::map<unsigned int, SharedPtr<Process> > ProcessMap;
  typedef std::pair<unsigned int,SharedPtr<Process> > ProcessPair;
  /**
   * Processes for each node in the SCassandra cluster
   */
  ProcessMap processes_;
  /**
   * Mutex for process piped buffer allocation and reads
   */
  static uv_mutex_t mutex_;

#if UV_VERSION_MAJOR == 0
  /**
   * uv_read_start callback for allocating memory for the buffer in the pipe
   *
   * @param handle Handle information for the pipe being read
   * @param suggested_size Suggested size for the buffer
   */
  static uv_buf_t handle_allocation(uv_handle_t* handle, size_t suggested_size);
#else
  /**
   * uv_read_start callback for allocating memory for the buffer in the pipe
   *
   * @param handle Handle information for the pipe being read
   * @param suggested_size Suggested size for the buffer
   * @param buffer Buffer to allocate bytes for
   */
  static void handle_allocation(uv_handle_t* handle, size_t suggested_size,
    uv_buf_t* buffer);
#endif

  /**
   * uv_spawn callback for handling the completion of the process
   *
   * @param process Process
   * @param error_code Error/Exit code
   * @param term_signal Terminating signal
   */
#if UV_VERSION_MAJOR == 0
  static void handle_exit(uv_process_t* process, int error_code,
    int term_signal);
#else
  static void handle_exit(uv_process_t* process, int64_t error_code,
    int term_signal);
#endif

  /**
   * uv_read_start callback for processing the buffer in the pipe
   *
   * @param stream Stream to process (stdout/stderr)
   * @param buffer_length Length of the buffer
   * @param buffer Buffer to process
   */
#if UV_VERSION_MAJOR == 0
  static void handle_read(uv_stream_t* stream, ssize_t buffer_length,
    uv_buf_t buffer);
#else
  static void handle_read(uv_stream_t* stream, ssize_t buffer_length,
    const uv_buf_t* buffer);
#endif

  /**
   * uv_thread_create callback for executing the SCassandra process
   *
   * @param arg Information to start the SCassandra process
   */
  static void handle_thread_create(void* arg);

    /**
   * Handle the connect (callback) when the connection has been established to
   * the REST server of SCassandra.
   *
   * @param nc Connection
   * @param ev Event type
   * @param ev_data
   */
  static void handle_connected(struct mg_connection* nc, int ev, void* ev_data);

  /**
   * Generate the HTTP message for the REST request.
   *
   * @param http_request HTTP request to generate message from
   * @return String representing the REST request HTTP message
   */
  std::string generate_http_message(HttpRequest* http_request) const;

  /**
   * DELETE request to send to the SCassandra REST server.
   *
   * @param node Node to send request to
   * @param endpoint The endpoint (URI) for the request
   */
  void rest_server_delete(unsigned int node, const std::string& endpoint);

  /**
   * GET request to send to the SCassandra REST server.
   *
   * @param node Node to send request to
   * @param endpoint The endpoint (URI) for the request
   */
  std::string rest_server_get(unsigned int node,
    const std::string& endpoint) const;

  /**
   * POST request to send to the SCassandra REST server.
   *
   * @param node Node to send request to
   * @param content The content of the POST
   * @param endpoint The endpoint (URI) for the request
   */
  void rest_server_post(unsigned int node, const std::string& content,
    const std::string& endpoint);

  /**
   * Send the HTTP request to the SCassandra REST server.
   *
   * @param node Node to send request to
   * @param http_request HTTP request to send to the REST server
   */
  void send_http_request(unsigned int node, HttpRequest* http_request) const;

  /**
   * Determine if a node is available
   *
   * @param node SCassandra node to check
   * @return True if node is available; false otherwise
   */
  bool is_node_available(unsigned int node);
  /**
   * Determine if a node is available
   *
   * @param ip_address IPv4 address of the SCassandra node
   * @param port Port of the SCassandra node
   * @return True if node is available; false otherwise
   */
  bool is_node_available(const std::string& ip_address, unsigned short port);
};

} // namespace test

#endif

#endif // __TEST_SCASSANDRA_CLUSTER_HPP__
