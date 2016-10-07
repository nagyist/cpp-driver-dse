/*
  Copyright (c) 2016 DataStax, Inc.

  This software can be used solely with DataStax Enterprise. Please consult the
  license at http://www.datastax.com/terms/datastax-dse-driver-license-terms
*/

#ifndef __TEST_RETRY_POLICY_HPP__
#define __TEST_RETRY_POLICY_HPP__
#include "cassandra.h"

#include "objects/object_base.hpp"

namespace test {
namespace driver {

/**
 * Wrapped retry policy object
 */
class RetryPolicy : public Object<CassRetryPolicy, cass_retry_policy_free> {
public:
  /**
   * Create the retry policy object from the native driver retry policy object
   *
   * @param retry_policy Native driver object
   */
  RetryPolicy(CassRetryPolicy* retry_policy)
    : Object<CassRetryPolicy, cass_retry_policy_free>(retry_policy) {}

  /**
   * Create the retry policy object from the shared reference
   *
   * @param retry_policy Shared reference
   */
  RetryPolicy(Ptr retry_policy)
    : Object<CassRetryPolicy, cass_retry_policy_free>(retry_policy) {}
};

} // namespace driver
} // namespace test

#endif // __TEST_RETRY_POLICY_HPP__