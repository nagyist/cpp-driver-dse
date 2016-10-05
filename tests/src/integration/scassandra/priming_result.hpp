/*
  Copyright (c) 2016 DataStax, Inc.

  This software can be used solely with DataStax Enterprise. Please consult the
  license at http://www.datastax.com/terms/datastax-dse-driver-license-terms
*/

#ifndef __PRIMING_RESULT_HPP__
#define __PRIMING_RESULT_HPP__

#include <string>

/**
 * Priming result request response
 */
class PrimingResult {
public:
  static const PrimingResult SUCCESS;
  static const PrimingResult READ_REQUEST_TIMEOUT;
  static const PrimingResult UNAVAILABLE;
  static const PrimingResult WRITE_REQUEST_TIMEOUT;
  static const PrimingResult SERVER_ERROR;
  static const PrimingResult PROTOCOL_ERROR;
  static const PrimingResult BAD_CREDENTIALS;
  static const PrimingResult OVERLOADED;
  static const PrimingResult IS_BOOTSTRAPPING;
  static const PrimingResult TRUNCATE_ERROR;
  static const PrimingResult SYNTAX_ERROR;
  static const PrimingResult UNAUTHORIZED;
  static const PrimingResult INVALID;
  static const PrimingResult CONFIG_ERROR;
  static const PrimingResult ALREADY_EXISTS;
  static const PrimingResult UNPREPARED;
  static const PrimingResult CLOSED_CONNECTION;

  PrimingResult(const std::string& json_value);

  const std::string& json_value() const;

private:
  std::string json_value_;
};

#endif //__PRIMING_RESULT_HPP__
