/*
  Copyright (c) 2016 DataStax, Inc.

  This software can be used solely with DataStax Enterprise. Please consult the
  license at http://www.datastax.com/terms/datastax-dse-driver-license-terms
*/

#include "scassandra_integration.hpp"
#include "priming_requests.hpp"

#include <algorithm>
#include <math.h>
#include <sstream>

#define MAX_TOKEN static_cast<uint64_t>(INT64_MAX) - 1

SCassandraIntegration::SCassandraIntegration()
  : scc_(NULL)
  , is_scc_start_requested_(true) {
}

SCassandraIntegration::~SCassandraIntegration() {
}

void SCassandraIntegration::SetUp() {
  // Initialize the SCassandra cluster instance
  // TODO: Allow for multiple DCs (two to start; mimic base class))
  unsigned int nodes = number_dc1_nodes_; // + number_dc2_nodes_;
  scc_ = new test::SCassandraCluster();
  scc_->create_cluster(nodes);
  scc_->start_cluster();

  // Generate the token ranges for the Murmur3Partitioner
  std::vector<std::string> token_ranges = generate_token_ranges(nodes);

  // Prime the system.local table
  PrimingRequest system_local = PrimingRequest::builder()
    .withQuery("SELECT data_center, rack, release_version FROM system.local WHERE key='local'")
    .withConsistency(CASS_CONSISTENCY_ANY)
    .withConsistency(CASS_CONSISTENCY_ONE)
    .withConsistency(CASS_CONSISTENCY_TWO)
    .withConsistency(CASS_CONSISTENCY_THREE)
    .withConsistency(CASS_CONSISTENCY_QUORUM)
    .withConsistency(CASS_CONSISTENCY_ALL)
    .withConsistency(CASS_CONSISTENCY_LOCAL_QUORUM)
    .withConsistency(CASS_CONSISTENCY_EACH_QUORUM)
    .withConsistency(CASS_CONSISTENCY_SERIAL)
    .withConsistency(CASS_CONSISTENCY_LOCAL_SERIAL)
    .withConsistency(CASS_CONSISTENCY_LOCAL_ONE)
    .withRows(PrimingRows::builder()
      .addColumn(PrimingRow::builder()
        .addColumn("data_center", CASS_VALUE_TYPE_VARCHAR, "dc1")
        .addColumn("rack", CASS_VALUE_TYPE_VARCHAR, "dc1")
        .addColumn("release_version", CASS_VALUE_TYPE_VARCHAR, "2.0.17")
      )
    );
  scc_->prime_query(system_local);
  for (unsigned int n = 0; n < nodes; ++n) {
    PrimingRequest system_local_tokens = PrimingRequest::builder()
      .withQuery("SELECT data_center, rack, release_version, partitioner, tokens FROM system.local WHERE key='local'")
      .withConsistency(CASS_CONSISTENCY_ANY)
      .withConsistency(CASS_CONSISTENCY_ONE)
      .withConsistency(CASS_CONSISTENCY_TWO)
      .withConsistency(CASS_CONSISTENCY_THREE)
      .withConsistency(CASS_CONSISTENCY_QUORUM)
      .withConsistency(CASS_CONSISTENCY_ALL)
      .withConsistency(CASS_CONSISTENCY_LOCAL_QUORUM)
      .withConsistency(CASS_CONSISTENCY_EACH_QUORUM)
      .withConsistency(CASS_CONSISTENCY_SERIAL)
      .withConsistency(CASS_CONSISTENCY_LOCAL_SERIAL)
      .withConsistency(CASS_CONSISTENCY_LOCAL_ONE)
      .withRows(PrimingRows::builder()
        .addColumn(PrimingRow::builder()
          .addColumn("data_center", CASS_VALUE_TYPE_VARCHAR, "dc1")
          .addColumn("rack", CASS_VALUE_TYPE_VARCHAR, "dc1")
          .addColumn("release_version", CASS_VALUE_TYPE_VARCHAR, "2.0.17")
          .addColumn("partitioner", CASS_VALUE_TYPE_VARCHAR,
            "org.apache.cassandra.dht.Murmur3Partitioner")
          .addColumn("tokens", "set<text>", "[" + token_ranges.at(n) + "]")
        )
      );
    scc_->prime_query(n + 1, system_local_tokens);
  }

  // Generate the default contact points
  contact_points_ = generate_contact_points(scc_->get_ip_prefix(), nodes);

  // Determine if the session connection should be established
  if (is_session_requested_) {
    connect();
  }
}

void SCassandraIntegration::TearDown() {
}

std::vector<std::string> SCassandraIntegration::generate_token_ranges(
  unsigned int nodes) {
  // Create and sort the token ranges
  std::vector<int64_t> token_ranges;
  for(unsigned int n = 0; n < nodes; ++n) {
    int64_t token_range = static_cast<uint64_t>(((UINT64_MAX / nodes) * n)) -
      MAX_TOKEN;
    token_ranges.push_back(token_range);
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
