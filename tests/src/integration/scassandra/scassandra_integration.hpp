/*
  Copyright (c) 2016 DataStax, Inc.

  This software can be used solely with DataStax Enterprise. Please consult the
  license at http://www.datastax.com/terms/datastax-dse-driver-license-terms
*/

#ifndef __SCASSANDRA_INTEGRATION_HPP__
#define __SCASSANDRA_INTEGRATION_HPP__
#include "integration.hpp"
#include "scassandra_cluster.hpp"

#include <uv.h>

/**
 * Base class to provide common integration test functionality for tests against
 * SCassandra (Stubbed Cassandra)
 */
class SCassandraIntegration : public Integration {
public:

  SCassandraIntegration();

  ~SCassandraIntegration();

  void SetUp();

  void TearDown();

protected:
  /**
   * SCassandra cluster (manager) instance
   */
  SharedPtr<test::SCassandraCluster> scc_;
  /**
   * Setting to determine if SCassandra cluster should be started. True if
   * SCassandracluster should be started; false otherwise.
   * (DEFAULT: true)
   */
  bool is_scc_start_requested_;

  /**
   * Generate the token ranges (no v-nodes) for a single data center
   *
   * @param nodes Number of nodes to generate tokens for
   * @return Token ranges for each node
   */
  std::vector<std::string> generate_token_ranges(unsigned int nodes);
};

#endif //__SCASSANDRA_INTEGRATION_HPP__
