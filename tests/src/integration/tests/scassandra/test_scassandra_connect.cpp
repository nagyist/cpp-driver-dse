/*
  Copyright (c) 2016 DataStax, Inc.

  This software can be used solely with DataStax Enterprise. Please consult the
  license at http://www.datastax.com/terms/datastax-dse-driver-license-terms
*/

#include "scassandra_integration.hpp"

/**
 * Connection integration tests
 */
class SConnectionIntegrationTest : public SCassandraIntegration {
public:
  void SetUp() {
    number_dc1_nodes_ = 3;
    SCassandraIntegration::SetUp();
  }
};

TEST_F(SConnectionIntegrationTest, Connect) {
  ASSERT_TRUE(true);
}
