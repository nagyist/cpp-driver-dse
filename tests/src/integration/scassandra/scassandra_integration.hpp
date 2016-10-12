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

#define CHECK_SCC_AVAILABLE \
  if (!scc_) { \
    return; \
  }

#define SKIP_TEST_IF_SCC_UNAVAILABLE \
  if (!scc_) { \
    SKIP_TEST("SCassandra is unavailable"); \
  }

/**
 * Base class to provide common integration test functionality for tests against
 * SCassandra (Stubbed Cassandra)
 */
class SCassandraIntegration : public Integration {
public:

  SCassandraIntegration();

  ~SCassandraIntegration();

  static void SetUpTestCase();

  void SetUp();

  void TearDown();

  static void TearDownTestCase();

protected:
  /**
   * SCassandra cluster (manager) instance
   */
  static SharedPtr<test::SCassandraCluster> scc_;
  /**
   * Setting to determine if SCassandra cluster should be started. True if
   * SCassandracluster should be started; false otherwise.
   * (DEFAULT: true)
   */
  bool is_scc_start_requested_;
  /**
   * Setting to determine if SCassandra cluster is being used for the entire
   * test case or if it should be re-initialized per test. True if for the
   * test case; false otherwise.
   * (DEFAULT: true)
   */
  bool is_scc_for_test_case_;

  /**
   * Execute a mock query at a given consistency level
   *
   * @param consistency Consistency level to execute mock query at
   * @return Result object
   * @see prime_mock_query Primes all nodes with a successful mock query
   * @see prime_mock_query_with_error Primes the given node with with an error
   *                                  for the mock query and primes the
   *                                  remaining nodes with a successful mock
   *                                  query
   */
  virtual test::driver::Result execute_mock_query(
    CassConsistency consistency = CASS_CONSISTENCY_ONE);
  /**
   * Prime the successful mock query on the given node
   *
   * @param node Node to apply the successful mock query on; if node == 0 the
   *             successful mock query will be applied to all nodes in the
   *             SCassandra cluster
   *             (DEFAULT: 0 - Apply mock query with success to all nodes
   */
  void prime_mock_query(unsigned int node = 0);
  /**
   * Prime the mock query with a simulated error result on the given node while
   * priming the remaining nodes in the SCassandra cluster with a successful
   * mock query
   *
   * @param result Resulting error to apply to the given node
   * @param node Node to apply the error on; if node == 0 the mock query will
   *             be applied to all nodes in the SCassandra cluster with the
   *             resulting error
   *             (DEFAULT: 0 - Apply mock query with error to all nodes
   */
  void prime_mock_query_with_error(PrimingResult result, unsigned int node = 0);

private:
  /**
   * Flag to determine if the SCassandra cluster instance has already been
   * initialized
   */
  static bool is_scc_initialized_;
  /**
   * A mocked query without the PrimingResult assigned
   */
  static const PrimingRequest mock_query_;
};

#endif //__SCASSANDRA_INTEGRATION_HPP__
