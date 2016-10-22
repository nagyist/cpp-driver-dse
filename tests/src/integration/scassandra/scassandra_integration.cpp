/*
  Copyright (c) 2016 DataStax, Inc.

  This software can be used solely with DataStax Enterprise. Please consult the
  license at http://www.datastax.com/terms/datastax-dse-driver-license-terms
*/

#include "scassandra_integration.hpp"
#include "priming_requests.hpp"
#include "win_debug.hpp"

// Initialize the static member variables
SharedPtr<test::SCassandraCluster> SCassandraIntegration::scc_ = NULL;
bool SCassandraIntegration::is_scc_initialized_ = false;
const PrimingRequest SCassandraIntegration::mock_query_ = PrimingRequest::builder()
  .with_query("mock query")
  .with_rows(PrimingRows::builder()
    .add_row(PrimingRow::builder()
      .add_column("SUCCESS", CASS_VALUE_TYPE_BOOLEAN, "TRUE")
    )
  );

SCassandraIntegration::SCassandraIntegration()
  : is_scc_start_requested_(true)
  , is_scc_for_test_case_(true) {
}

SCassandraIntegration::~SCassandraIntegration() {
}

void SCassandraIntegration::SetUpTestCase() {
  try {
    scc_ = new test::SCassandraCluster();
  } catch (SCassandraCluster::Exception scce) {
    FAIL() << scce.what();
  }
}

void SCassandraIntegration::SetUp() {
  CHECK_SCC_AVAILABLE;

  // Initialize the SCassandra cluster instance
  if (!is_scc_initialized_) {
    // Create the data center nodes vector
    std::vector<unsigned int> data_center_nodes;
    data_center_nodes.push_back(number_dc1_nodes_);
    data_center_nodes.push_back(number_dc2_nodes_);

    MemoryLeakListener::disable();
    scc_->create_cluster(data_center_nodes);
    scc_->start_cluster();
    MemoryLeakListener::enable();
    is_scc_initialized_ = true;
  }
  scc_->prime_system_tables();

  // Generate the default contact points
  contact_points_ = scc_->cluster_contact_points();

  // Determine if the session connection should be established
  if (is_session_requested_) {
    connect();
  }
}

void SCassandraIntegration::TearDownTestCase() {
  CHECK_SCC_AVAILABLE;

  scc_->stop_cluster();
  is_scc_initialized_ = false;
}

void SCassandraIntegration::TearDown() {
  CHECK_SCC_AVAILABLE;

  session_.close();

  // Reset the SCassandra cluster (if not being used for the entire test case)
  scc_->remove_primed_queries();
  if (!is_scc_for_test_case_) {
    scc_->stop_cluster();
    is_scc_initialized_ = false;
  }
}

test::driver::Result SCassandraIntegration::execute_mock_query(
  CassConsistency consistency /*= CASS_CONSISTENCY_ONE*/) {
  return session_.execute("mock query", consistency, false);
}

void SCassandraIntegration::prime_mock_query(unsigned int node /*= 0*/) {
  // Create the mock query
  PrimingRequest mock_query = mock_query_;
  mock_query.with_result(PrimingResult::SUCCESS);

  // Determine if this is targeting a particular node
  if (node > 0) {
    scc_->prime_query(node, mock_query);
  } else {
    scc_->prime_query(mock_query);
  }
}

void SCassandraIntegration::prime_mock_query_with_error(PrimingResult result,
  unsigned int node /*= 0*/) {
  // Create the mock query
  PrimingRequest mock_query = mock_query_;
  mock_query.with_result(result);

  // Determine if this is targeting a particular node
  if (node > 0) {
    // Send the simulated error to the SCassandra node
    scc_->prime_query(node, mock_query);

    // Update the primed query to be successful on the other node
    std::vector<unsigned int> nodes = scc_->nodes();
    for (std::vector<unsigned int>::iterator iterator = nodes.begin();
      iterator != nodes.end(); ++iterator) {
      unsigned int current_node = *iterator;
      if (current_node != node) {
        prime_mock_query(current_node);
      }
    }
  } else {
    scc_->prime_query(mock_query);
  }
}
