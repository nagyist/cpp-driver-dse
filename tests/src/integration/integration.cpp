/*
  Copyright (c) 2016 DataStax, Inc.

  This software can be used solely with DataStax Enterprise. Please consult the
  license at http://www.datastax.com/terms/datastax-dse-driver-license-terms
*/

#include "integration.hpp"
#include "options.hpp"

#include <cstdarg>
#include <iostream>
#include <sys/stat.h>

#define FORMAT_BUFFER_SIZE 10240
#define KEYSPACE_MAXIMUM_LENGTH 48
#define SIMPLE_KEYSPACE_FORMAT "CREATE KEYSPACE %s WITH replication = %s"
#define REPLICATION_STRATEGY "{ 'class': %s }"
#define SELECT_SERVER_VERSION "SELECT release_version FROM system.local"

Integration::Integration()
  : ccm_(NULL)
  , session_()
  , keyspace_name_("")
  , table_name_("")
  , uuid_generator_()
  , server_version_(Options::server_version())
  , number_dc1_nodes_(1)
  , number_dc2_nodes_(0)
  , replication_factor_(0)
  , replication_strategy_("")
  , contact_points_("")
  , is_client_authentication_(false)
  , is_ssl_(false)
  , is_with_vnodes_(false)
  , is_randomized_contact_points_(false)
  , is_schema_metadata_(false)
  , is_ccm_start_requested_(true)
  , is_ccm_start_node_individually_(false)
  , is_session_requested_(true)
  , create_keyspace_query_("")
  , start_time_(0ull) {
  // Get the name of the test and the case/suite it belongs to
  const testing::TestInfo* test_information = testing::UnitTest::GetInstance()->current_test_info();
  test_name_ = test_information->name();

  // Determine if this is a typed test (e.g. ends in a number)
  const char* type_param = test_information->type_param();
  if (type_param) {
    std::vector<std::string> tokens = explode(test_information->test_case_name(), '/');
    for (std::vector<std::string>::const_iterator iterator = tokens.begin();
      iterator < tokens.end(); ++iterator) {
      std::string token = *iterator;

      // Determine if we are looking at the last token
      if ((iterator + 1) == tokens.end()) {
        size_t number = 0;
        std::stringstream tokenStream(token);
        if (!(tokenStream >> number).fail()) {
          std::vector<std::string> type_param_tokens = explode(type_param, ':');
          size_t size = type_param_tokens.size();
          test_case_name_ += type_param_tokens[size - 1];
        }
      } else {
        test_case_name_ += token + "_";
      }
    }
  } else {
    test_case_name_ = test_information->test_case_name();
  }

  // Determine if file logging should be enabled for the integration tests
  if (Options::log_tests()) {
    logger_.initialize(test_case_name_, test_name_);
  }
}

Integration::~Integration() {
  try {
    session_.close(false);
  } catch (...) {}
}

void Integration::SetUp() {
  // Initialize the DSE workload (iff not set)
  if (dse_workload_.empty()) {
    dse_workload_.push_back(CCM::DSE_WORKLOAD_CASSANDRA);
  }
  // Generate the keyspace name
  keyspace_name_ = replace_all(to_lower(test_case_name_), "integrationtest", "") +
                   "_" + to_lower(test_name_);
  if (keyspace_name_.size() > KEYSPACE_MAXIMUM_LENGTH) {
    // Update the keyspace name with a UUID (first portions of v4 UUID)
    std::vector<std::string> uuid_octets = explode(uuid_generator_.generate_timeuuid().str(), '-');
    std::string id = uuid_octets[0] + uuid_octets[3];
    keyspace_name_ = keyspace_name_.substr(0, KEYSPACE_MAXIMUM_LENGTH - id.size()) +
                     id;
  }

  // Generate the table name
  table_name_ = to_lower(test_name_);

  // Determine the replication strategy
  std::stringstream replication_strategy;
  if (number_dc2_nodes_ > 0) {
    replication_strategy << "'NetworkTopologyStrategy', 'dc1': " <<
                            number_dc1_nodes_ << ", " <<
                            "'dc2': " << number_dc2_nodes_;
  } else {
    replication_strategy << "'SimpleStrategy', 'replication_factor': ";
    if (replication_factor_ == 0) {
      // Calculate the default replication factor
      replication_factor_ = (number_dc1_nodes_ % 2 == 0) ? number_dc1_nodes_ / 2 : (number_dc1_nodes_ + 1) / 2;
    }
    replication_strategy << replication_factor_;
  }
  replication_strategy_ = format_string(REPLICATION_STRATEGY,
                                        replication_strategy.str().c_str());

  // Generate the keyspace query
  create_keyspace_query_ = format_string(SIMPLE_KEYSPACE_FORMAT,
                                         keyspace_name_.c_str(),
                                         replication_strategy_.c_str());

  // Create the data center nodes vector
  std::vector<unsigned short> data_center_nodes;
  data_center_nodes.push_back(number_dc1_nodes_);
  data_center_nodes.push_back(number_dc2_nodes_);

  try {
    //Create and start the CCM cluster (if not already created)
    ccm_ = new CCM::Bridge(server_version_,
      Options::use_git(), Options::branch_tag(),
      Options::use_install_dir(), Options::install_dir(),
      Options::is_dse(), dse_workload_,
      Options::cluster_prefix(),
      Options::dse_credentials(),
      Options::dse_username(), Options::dse_password(),
      Options::deployment_type(), Options::authentication_type(),
      Options::host(), Options::port(),
      Options::username(), Options::password(),
      Options::public_key(), Options::private_key());
    if (ccm_->create_cluster(data_center_nodes, is_with_vnodes_, is_ssl_,
      is_client_authentication_)) {
      if (is_ccm_start_requested_) {
        if (is_ccm_start_node_individually_) {
          for (unsigned short node = 1;
            node <= (number_dc1_nodes_ + number_dc2_nodes_); ++node) {
            ccm_->start_node(node);
          }
        } else {
          ccm_->start_cluster();
        }
      }
    }

    // Generate the default contact points
    contact_points_ = generate_contact_points(ccm_->get_ip_prefix(),
                      number_dc1_nodes_ + number_dc2_nodes_);

    // Determine if the session connection should be established
    if (is_session_requested_ && is_ccm_start_requested_) {
      connect();
    }
  } catch (CCM::BridgeException be) {
    // Issue creating the CCM bridge instance (force failure)
    FAIL() << be.what();
  }
}

void Integration::TearDown() {
  // Drop keyspace for integration test (may or may have not been created)
  std::stringstream use_keyspace_query;
  use_keyspace_query << "DROP KEYSPACE " << keyspace_name_;
  try {
    session_.execute(use_keyspace_query.str(), CASS_CONSISTENCY_ANY, false);
  } catch (...) {}
}

void Integration::connect(Cluster cluster) {
  // Establish the session connection
  cluster_ = cluster;
  session_ = cluster.connect();
  CHECK_FAILURE;

  // Update the server version if branch_tag was specified
  if (Options::use_git() && !Options::branch_tag().empty()) {
    if (Options::is_dse()) {
      server_version_ = ccm_->get_dse_version();
    } else {
      server_version_ = ccm_->get_cassandra_version();
    }
    LOG("Branch/Tag Option was Used: Retrieved server version is " << server_version_.to_string());
  }

  // Create the keyspace for the integration test
  session_.execute(create_keyspace_query_);
  CHECK_FAILURE;

  // Update the session to use the new keyspace by default
  std::stringstream use_keyspace_query;
  use_keyspace_query << "USE " << keyspace_name_;
  session_.execute(use_keyspace_query.str());
}

void Integration::connect() {
  // Create the cluster configuration and establish the session connection
  cluster_ = default_cluster();
  connect(cluster_);
}

test::driver::Cluster Integration::default_cluster() {
  return Cluster::build()
    .with_contact_points(contact_points_)
    .with_randomized_contact_points(is_randomized_contact_points_)
    .with_schema_metadata(is_schema_metadata_);
}

void Integration::enable_cluster_tracing(bool enable /*= true*/) {
  std::vector<std::string> active_nodes = ccm_->cluster_ip_addresses();
  for (std::vector<std::string>::iterator iterator = active_nodes.begin();
    iterator != active_nodes.end(); ++iterator) {
    // Get the node number from the IP address
    std::string node_ip_address = *iterator;
    std::stringstream node_value;
    node_value << node_ip_address.at(node_ip_address.length() - 1);

    // Enable tracing on the node
    unsigned int node;
    node_value >> node;
    ccm_->enable_node_trace(node);
  }
}

std::string Integration::generate_contact_points(const std::string& ip_prefix,
  size_t number_of_nodes) {
  // Iterate over the total number of nodes to create the contact list
  std::vector<std::string> contact_points;
  for (size_t i = 1; i <= number_of_nodes; ++i) {
    std::stringstream contact_point;
    contact_point << ip_prefix << i;
    contact_points.push_back(contact_point.str());
  }
  return implode(contact_points, ',');
}

std::string Integration::format_string(const char* format, ...) const {
  // Create a buffer for the formatting of the string
  char buffer[FORMAT_BUFFER_SIZE] = { '\0' };

  // Parse the arguments into the buffer
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, FORMAT_BUFFER_SIZE, format, args);
  va_end(args);

  // Return the formatted string
  return buffer;
}

