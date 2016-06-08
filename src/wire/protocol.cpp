//===----------------------------------------------------------------------===//
//
//                         Peloton
//
// protocol.cpp
//
// Identification: src/wire/protocol.cpp
//
// Copyright (c) 2015-16, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include <cstdio>
#include <unordered_map>

#include "common/cache.h"
#include "common/types.h"
#include "common/macros.h"

#include "wire/marshal.h"
#include "common/portal.h"
#include "tcop/tcop.h"

#include <boost/algorithm/string.hpp>

#define PROTO_MAJOR_VERSION(x) x >> 16

namespace peloton {
namespace wire {

// Prepares statment cache
thread_local peloton::Cache<std::string, Statement> cache_;

// Query portal handler
thread_local std::unordered_map<std::string, std::shared_ptr<Portal>> portals_;

// Hardcoded authentication strings used during session startup. To be removed
const std::unordered_map<std::string, std::string>
    PacketManager::parameter_status_map =
        boost::assign::map_list_of("application_name", "psql")(
            "client_encoding", "UTF8")("DateStyle", "ISO, MDY")(
            "integer_datetimes", "on")("IntervalStyle", "postgres")(
            "is_superuser", "on")("server_encoding", "UTF8")(
            "server_version", "9.5devel")("session_authorization", "postgres")(
            "standard_conforming_strings", "on")("TimeZone", "US/Eastern");

/*
 * close_client - Close the socket of the underlying client
 */
void PacketManager::CloseClient() { client.sock->CloseSocket(); }

void PacketManager::MakeHardcodedParameterStatus(
    ResponseBuffer &responses, const std::pair<std::string, std::string> &kv) {
  std::unique_ptr<Packet> response(new Packet());
  response->msg_type = 'S';
  PacketPutString(response, kv.first);
  PacketPutString(response, kv.second);
  responses.push_back(std::move(response));
}
/*
 * process_startup_packet - Processes the startup packet
 * 	(after the size field of the header).
 */
bool PacketManager::ProcessStartupPacket(Packet *pkt,
                                         ResponseBuffer &responses) {
  std::string token, value;
  std::unique_ptr<Packet> response(new Packet());

  int32_t proto_version = PacketGetInt(pkt, sizeof(int32_t));

  // Only protocol version 3 is supported
  if (PROTO_MAJOR_VERSION(proto_version) != 3) {
    LOG_ERROR("Protocol error: Only protocol version 3 is supported.");
    exit(EXIT_FAILURE);
  }

  // TODO: check for more malformed cases
  // iterate till the end
  for (;;) {
    // loop end case?
    if (pkt->ptr >= pkt->len) break;
    GetStringToken(pkt, token);

    // if the option database was found
    if (token.compare("database") == 0) {
      // loop end?
      if (pkt->ptr >= pkt->len) break;
      GetStringToken(pkt, client.dbname);
    } else if (token.compare(("user")) == 0) {
      // loop end?
      if (pkt->ptr >= pkt->len) break;
      GetStringToken(pkt, client.user);
    } else {
      if (pkt->ptr >= pkt->len) break;
      GetStringToken(pkt, value);
      client.cmdline_options[token] = value;
    }
  }

  // send auth-ok ('R')
  response->msg_type = 'R';
  PacketPutInt(response, 0, 4);
  responses.push_back(std::move(response));

  // Send the parameterStatus map ('S')
  for (auto it = parameter_status_map.begin(); it != parameter_status_map.end();
       it++) {
    MakeHardcodedParameterStatus(responses, *it);
  }

  // ready-for-query packet -> 'Z'
  SendReadyForQuery(TXN_IDLE, responses);
  return true;
}

void PacketManager::PutRowDesc(std::vector<FieldInfoType> &rowdesc,
                               ResponseBuffer &responses) {
  if (!rowdesc.size()) return;

  LOG_INFO("Put RowDescription");
  std::unique_ptr<Packet> pkt(new Packet());
  pkt->msg_type = 'T';
  PacketPutInt(pkt, rowdesc.size(), 2);

  for (auto col : rowdesc) {
    LOG_INFO("column name: %s", std::get<0>(col).c_str());
    PacketPutString(pkt, std::get<0>(col));
    // TODO: Table Oid (int32)
    PacketPutInt(pkt, 0, 4);
    // TODO: Attr id of column (int16)
    PacketPutInt(pkt, 0, 2);
    // Field data type (int32)
    PacketPutInt(pkt, std::get<1>(col), 4);
    // Data type size (int16)
    PacketPutInt(pkt, std::get<2>(col), 2);
    // Type modifier (int32)
    PacketPutInt(pkt, -1, 4);
    // Format code for text
    PacketPutInt(pkt, 0, 2);
  }
  responses.push_back(std::move(pkt));
}

void PacketManager::SendDataRows(std::vector<ResType> &results,
                                 int colcount, int &rows_affected,
                                 ResponseBuffer &responses) {
  if (!results.size() || !colcount) return;

  LOG_INFO("Flatten result size: %lu", results.size());
  size_t numrows = results.size() / colcount;

  // 1 packet per row
  for (size_t i = 0; i < numrows; i++) {
    std::unique_ptr<Packet> pkt(new Packet());
    pkt->msg_type = 'D';
    PacketPutInt(pkt, colcount, 2);
    for (int j = 0; j < colcount; j++) {
      // length of the row attribute
      PacketPutInt(pkt, results[i * colcount + j].second.size(), 4);
      // contents of the row attribute
      PacketPutBytes(pkt, results[i * colcount + j].second);
    }
    responses.push_back(std::move(pkt));
  }
  rows_affected = numrows;
  LOG_INFO("Rows affected: %d", rows_affected);
}

/* Gets the first token of a query */
std::string get_query_type(std::string query) {
  std::vector<std::string> query_tokens;
  boost::split(query_tokens, query, boost::is_any_of(" "),
               boost::token_compress_on);
  return query_tokens[0];
}

void PacketManager::CompleteCommand(const std::string &query_type, int rows,
                                    ResponseBuffer &responses) {
  std::unique_ptr<Packet> pkt(new Packet());
  pkt->msg_type = 'C';
  std::string tag = query_type;
  /* After Begin, we enter a txn block */
  if (query_type.compare("BEGIN") == 0) txn_state = TXN_BLOCK;
  /* After commit, we end the txn block */
  else if (query_type.compare("COMMIT") == 0)
    txn_state = TXN_IDLE;
  /* After rollback, the txn block is ended */
  else if (!query_type.compare("ROLLBACK"))
    txn_state = TXN_IDLE;
  /* the rest are custom status messages for each command */
  else if (!query_type.compare("INSERT"))
    tag += " 0 " + std::to_string(rows);
  else
    tag += " " + std::to_string(rows);
  LOG_INFO("complete command tag: %s", tag.c_str());
  PacketPutString(pkt, tag);

  responses.push_back(std::move(pkt));
}

/*
 * put_empty_query_response - Informs the client that an empty query was sent
 */
void PacketManager::SendEmptyQueryResponse(ResponseBuffer &responses) {
  std::unique_ptr<Packet> response(new Packet());
  response->msg_type = 'I';
  responses.push_back(std::move(response));
}

bool PacketManager::HardcodedExecuteFilter(std::string query_type) {
  // Skip SET
  if (query_type.compare("SET") == 0 || query_type.compare("SHOW") == 0)
    return false;
  // skip duplicate BEGIN
  if (!query_type.compare("BEGIN") && txn_state == TXN_BLOCK) return false;
  // skip duplicate Commits
  if (!query_type.compare("COMMIT") && txn_state == TXN_IDLE) return false;
  // skip duplicate Rollbacks
  if (!query_type.compare("ROLLBACK") && txn_state == TXN_IDLE) return false;
  return true;
}

// The Simple Query Protocol
void PacketManager::ExecQueryMessage(Packet *pkt, ResponseBuffer &responses) {
  std::string q_str;
  PacketGetString(pkt, pkt->len, q_str);
  LOG_INFO("Query Received: %s \n", q_str.c_str());

  std::vector<std::string> queries;
  boost::split(queries, q_str, boost::is_any_of(";"));


  // just a ';' sent
  if (queries.size() == 1) {
    SendEmptyQueryResponse(responses);
    SendReadyForQuery(txn_state, responses);
    return;
  }

  // Get traffic cop
  auto &tcop = tcop::TrafficCop::GetInstance();

  // iterate till before the trivial string after the last ';'
  for (auto query = queries.begin(); query != queries.end() - 1; query++) {
    if (query->empty()) {
      SendEmptyQueryResponse(responses);
      SendReadyForQuery(TXN_IDLE, responses);
      return;
    }

    std::vector<ResType> results;
    std::vector<FieldInfoType> rowdesc;
    std::string err_msg;
    int rows_affected;

    // execute the query in Sqlite
    auto status = tcop.PortalExec(query->c_str(), results, rowdesc, rows_affected, err_msg);

    // check status
    if (status ==  Result::RESULT_FAILURE) {
      SendErrorResponse({{'M', err_msg}}, responses);
      break;
    }

    // send the attribute names
    PutRowDesc(rowdesc, responses);

    // send the result rows
    SendDataRows(results, rowdesc.size(), rows_affected, responses);

    // TODO: should change to query_type
    CompleteCommand(*query, rows_affected, responses);
  }

  SendReadyForQuery('I', responses);
}

/*
 * exec_parse_message - handle PARSE message
 */
void PacketManager::ExecParseMessage(Packet *pkt, ResponseBuffer &responses) {
  LOG_INFO("PARSE message");
  std::string err_msg, prep_stmt_name, query, query_type;
  GetStringToken(pkt, prep_stmt_name);

  // Read prepare statement name
  PreparedStatement *stmt = nullptr;
  LOG_INFO("Prep stmt: %s", prep_stmt_name.c_str());
  // Read query string
  GetStringToken(pkt, query);
  LOG_INFO("Parse Query: %s", query.c_str());

  auto &tcop = tcop::TrafficCop::GetInstance();

  skipped_stmt_ = false;
  query_type = get_query_type(query);
  if (!HardcodedExecuteFilter(query_type)) {
    // query to be filtered, don't execute
    skipped_stmt_ = true;
    skipped_query_ = std::move(query);
    skipped_query_type_ = std::move(query_type);
    LOG_INFO("Statement to be skipped");
  } else {
    // Prepare statement
    int is_failed = tcop.PrepareStmt(query.c_str(), &stmt, err_msg);
    if (is_failed) {
      SendErrorResponse({{'M', err_msg}}, responses);
      SendReadyForQuery(txn_state, responses);
    }
  }

  // Read number of params
  int num_params = PacketGetInt(pkt, 2);
  LOG_INFO("NumParams: %d", num_params);

  // Read param types
  std::vector<int32_t> param_types(num_params);
  for (int i = 0; i < num_params; i++) {
    int param_type = PacketGetInt(pkt, 4);
    param_types[i] = param_type;
  }

  // Cache the received qury
  std::shared_ptr<Statement> entry(new Statement());
  entry->stmt_name = prep_stmt_name;
  entry->query_string = query;
  entry->query_type = std::move(query_type);
  entry->sql_stmt = stmt;
  entry->param_types = std::move(param_types);

  if (prep_stmt_name.empty()) {
    // Unnamed statement
    unnamed_entry = entry;
  } else {
    cache_.insert(std::make_pair(std::move(prep_stmt_name), entry));
  }

  std::unique_ptr<Packet> response(new Packet());

  // Send Parse complete response
  response->msg_type = '1';
  responses.push_back(std::move(response));
}

void PacketManager::ExecBindMessage(Packet *pkt, ResponseBuffer &responses) {
  std::string portal_name, prep_stmt_name;
  // BIND message
  LOG_INFO("BIND message");
  GetStringToken(pkt, portal_name);
  LOG_INFO("Portal name: %s", portal_name.c_str());
  GetStringToken(pkt, prep_stmt_name);
  LOG_INFO("Prep stmt name: %s", prep_stmt_name.c_str());

  if (skipped_stmt_) {
    // send bind complete
    std::unique_ptr<Packet> response(new Packet());
    response->msg_type = '2';
    responses.push_back(std::move(response));
    return;
  }

  // Read parameter format
  int num_params_format = PacketGetInt(pkt, 2);

  // get the format of each parameter
  std::vector<int16_t> formats(num_params_format);
  for (int i = 0; i < num_params_format; i++) {
    formats[i] = PacketGetInt(pkt, 2);
  }

  // error handling
  int num_params = PacketGetInt(pkt, 2);
  if (num_params_format != num_params) {
    std::string err_msg =
        "Malformed request: num_params_format is not equal to num_params";
    SendErrorResponse({{'M', err_msg}}, responses);
    return;
  }

  // Get statement info generated in PARSE message
  PreparedStatement *stmt = nullptr;
  std::shared_ptr<Statement> entry;
  if (prep_stmt_name.empty()) {
    LOG_INFO("Unnamed statement");
    entry = unnamed_entry;
  } else {
    // fetch the statement ID from the cache
    auto itr = cache_.find(prep_stmt_name);
    if (itr != cache_.end()) {
      entry = *itr;
    } else {
      std::string err_msg = "Prepared statement name already exists";
      SendErrorResponse({{'M', err_msg}}, responses);
      return;
    }
  }
  stmt = entry->sql_stmt;
  const auto &query_string = entry->query_string;
  const auto &query_type = entry->query_type;

  // check if the loaded statement needs to be skipped
  skipped_stmt_ = false;
  if (!HardcodedExecuteFilter(query_type)) {
    skipped_stmt_ = true;
    skipped_query_ = query_string;
    LOG_INFO("Statement skipped: %s", skipped_query_.c_str());
    std::unique_ptr<Packet> response(new Packet());
    // Send Parse complete response
    response->msg_type = '2';
    responses.push_back(std::move(response));
    return;
  }

  // Group the parameter types and thae parameters in this vector
  std::vector<std::pair<int, std::string>> bind_parameters;
  PktBuf param;
  for (int param_idx = 0; param_idx < num_params; param_idx++) {
    int param_len = PacketGetInt(pkt, 4);
    // BIND packet NULL parameter case
    if (param_len == -1) {
      // NULL mode
      bind_parameters.push_back(std::make_pair(ValueType::VALUE_TYPE_INTEGER, std::string("")));
    } else {
      PacketGetBytes(pkt, param_len, param);

      if (formats[param_idx] == 0) {
        // TEXT mode
        std::string param_str = std::string(std::begin(param), std::end(param));
        bind_parameters.push_back(std::make_pair(ValueType::VALUE_TYPE_VARCHAR, param_str));
      } else {
        // BINARY mode
        switch (entry->param_types[param_idx]) {
          case POSTGRES_VALUE_TYPE_INTEGER: {
            int int_val = 0;
            for (size_t i = 0; i < sizeof(int); ++i) {
              int_val = (int_val << 8) | param[i];
            }
            bind_parameters.push_back(
                std::make_pair(ValueType::VALUE_TYPE_INTEGER, std::to_string(int_val)));
          } break;
          case POSTGRES_VALUE_TYPE_DOUBLE: {
            double float_val = 0;
            unsigned long buf = 0;
            for (size_t i = 0; i < sizeof(double); ++i) {
              buf = (buf << 8) | param[i];
            }
            memcpy(&float_val, &buf, sizeof(double));
            bind_parameters.push_back(
                std::make_pair(ValueType::VALUE_TYPE_DOUBLE, std::to_string(float_val)));
            // LOG_INFO("Bind param (size: %d) : %lf", param_len, float_val);
          } break;
          default: {
            LOG_ERROR("Do not support data type: %d",
                      entry->param_types[param_idx]);
          } break;
        }
      }
    }
  }

  std::string err_msg;
  auto &tcop = tcop::TrafficCop::GetInstance();
  bool is_failed = tcop.BindStmt(bind_parameters, &stmt, err_msg);
  if (is_failed) {
    SendErrorResponse({{'M', err_msg}}, responses);
    SendReadyForQuery(txn_state, responses);
  }

  // TODO: replace this with a constructor
  std::shared_ptr<Portal> portal(new Portal());
  portal->query_string = query_string;
  portal->stmt = stmt;
  portal->prep_stmt_name = prep_stmt_name;
  portal->portal_name = portal_name;
  portal->query_type = query_type;

  auto itr = portals_.find(portal_name);
  if (itr == portals_.end()) {
    portals_.insert(std::make_pair(portal_name, portal));
  } else {
    std::shared_ptr<Portal> p = itr->second;
    itr->second = portal;
  }

  // send bind complete
  std::unique_ptr<Packet> response(new Packet());
  response->msg_type = '2';
  responses.push_back(std::move(response));
}

void PacketManager::ExecDescribeMessage(Packet *pkt,
                                        ResponseBuffer &responses) {
  PktBuf mode;
  std::string name;
  LOG_INFO("DESCRIBE message");
  PacketGetBytes(pkt, 1, mode);
  LOG_INFO("mode %c", mode[0]);
  GetStringToken(pkt, name);
  LOG_INFO("name: %s", name.c_str());
  if (mode[0] == 'P') {
    auto portal_itr = portals_.find(name);
    if (portal_itr == portals_.end()) {
      // TODO: error handling here
      std::vector<FieldInfoType> rowdesc;
      PutRowDesc(rowdesc, responses);
      return;
    }
    std::shared_ptr<Portal> p = portal_itr->second;
    auto &tcop = tcop::TrafficCop::GetInstance();
    tcop.GetRowDesc(p->stmt, p->tuple_desc);
    PutRowDesc(p->tuple_desc, responses);
  }
}

void PacketManager::ExecExecuteMessage(Packet *pkt, ResponseBuffer &responses) {
  // EXECUTE message
  LOG_INFO("EXECUTE message");
  std::vector<ResType> results;
  std::string err_msg, portal_name;
  PreparedStatement *stmt = nullptr;
  int rows_affected = 0, is_failed;
  GetStringToken(pkt, portal_name);

  // covers weird JDBC edge case of sending double BEGIN statements. Don't
  // execute them
  if (skipped_stmt_) {
    LOG_INFO("Statement skipped: %s", skipped_query_.c_str());
    CompleteCommand(skipped_query_type_, rows_affected, responses);
    skipped_stmt_ = false;
    return;
  }

  auto portal = portals_[portal_name];
  const auto &query_string = portal->query_string;
  const auto &query_type = portal->query_type;
  stmt = portal->stmt;
  PL_ASSERT(stmt != nullptr);

  bool unnamed = portal->prep_stmt_name.empty();

  LOG_INFO("Executing query: %s", query_string.c_str());

  // acquire the mutex if we are starting a txn
  if (query_string.compare("BEGIN") == 0) {
    LOG_WARN("BEGIN - acquire lock");
  }

  auto &tcop = tcop::TrafficCop::GetInstance();
  is_failed = tcop.ExecPrepStmt(stmt, unnamed, results, rows_affected, err_msg);
  if (is_failed) {
    LOG_INFO("Failed to execute: %s", err_msg.c_str());
    SendErrorResponse({{'M', err_msg}}, responses);
    SendReadyForQuery(txn_state, responses);
  }

  // release the mutex after a txn commit
  if (query_string.compare("COMMIT") == 0) {
    LOG_WARN("COMMIT - release lock");
  }

  // put_row_desc(portal->rowdesc, responses);
  SendDataRows(results, portal->tuple_desc.size(), rows_affected, responses);
  CompleteCommand(query_type, rows_affected, responses);
}

/*
 * process_packet - Main switch block; process incoming packets,
 *  Returns false if the session needs to be closed.
 */
bool PacketManager::ProcessPacket(Packet *pkt, ResponseBuffer &responses) {
  switch (pkt->msg_type) {
    case 'Q': {
      ExecQueryMessage(pkt, responses);
    } break;
    case 'P': {
      ExecParseMessage(pkt, responses);
    } break;
    case 'B': {
      ExecBindMessage(pkt, responses);
    } break;
    case 'D': {
      ExecDescribeMessage(pkt, responses);
    } break;
    case 'E': {
      ExecExecuteMessage(pkt, responses);
    } break;
    case 'S': {
      // SYNC message
      SendReadyForQuery(txn_state, responses);
    } break;
    case 'X': {
      LOG_INFO("Closing client");
      return false;
    } break;
    default: {
      LOG_INFO("Packet type not supported yet: %d (%c)", pkt->msg_type,
               pkt->msg_type);
    }
  }
  return true;
}

/*
 * send_error_response - Sends the passed string as an error response.
 * 		For now, it only supports the human readable 'M' message body
 */
void PacketManager::SendErrorResponse(
    std::vector<std::pair<uchar, std::string>> error_status,
    ResponseBuffer &responses) {
  std::unique_ptr<Packet> pkt(new Packet());
  pkt->msg_type = 'E';

  for (auto entry : error_status) {
    PacketPutByte(pkt, entry.first);
    PacketPutString(pkt, entry.second);
  }

  // put null terminator
  PacketPutByte(pkt, 0);

  // don't care if write finished or not, we are closing anyway
  responses.push_back(std::move(pkt));
}

void PacketManager::SendReadyForQuery(uchar txn_status,
                                      ResponseBuffer &responses) {
  std::unique_ptr<Packet> pkt(new Packet());
  pkt->msg_type = 'Z';

  PacketPutByte(pkt, txn_status);

  responses.push_back(std::move(pkt));
}

/*
 * PacketManager - Main wire protocol logic.
 * 		Always return with a closed socket.
 */
void PacketManager::ManagePackets() {
  Packet pkt;
  ResponseBuffer responses;
  bool status;

  // fetch the startup packet
  if (!ReadPacket(&pkt, false, &client)) {
    CloseClient();
    return;
  }

  status = ProcessStartupPacket(&pkt, responses);
  if (!WritePackets(responses, &client) || !status) {
    // close client on write failure or status failure
    CloseClient();
    return;
  }

  pkt.Reset();
  while (ReadPacket(&pkt, true, &client)) {
    status = ProcessPacket(&pkt, responses);
    if (!WritePackets(responses, &client) || !status) {
      // close client on write failure or status failure
      CloseClient();
      return;
    }
    pkt.Reset();
  }
}

}  // End wire namespace
}  // End peloton namespace
