#include <sstream>
#include <vector>
#include <algorithm>
#include <glog/logging.h>
#include "pika_server.h"
#include "pika_conf.h"
#include "pika_client_conn.h"

extern PikaServer* g_pika_server;
extern PikaConf* g_pika_conf;
static const int RAW_ARGS_LEN = 1024 * 1024; 

PikaClientConn::PikaClientConn(int fd, std::string ip_port, pink::Thread* thread) :
  RedisConn(fd, ip_port) {
  self_thread_ = dynamic_cast<PikaWorkerThread*>(thread);
  auth_stat_.Init();
}

PikaClientConn::~PikaClientConn() {
}

std::string PikaClientConn::RestoreArgs() {
  std::string res;
  res.reserve(RAW_ARGS_LEN);
  RedisAppendLen(res, argv_.size(), "*");
  PikaCmdArgsType::const_iterator it = argv_.begin();
  for ( ; it != argv_.end(); ++it) {
    RedisAppendLen(res, (*it).size(), "$");
    RedisAppendContent(res, *it);
  }
  return res;
}

std::string PikaClientConn::DoCmd(const std::string& opt) {
  // Get command info
  const CmdInfo* const cinfo_ptr = GetCmdInfo(opt);
  Cmd* c_ptr = self_thread_->GetCmd(opt);
  if (!cinfo_ptr || !c_ptr) {
      return "-Err unknown or unsupported command \'" + opt + "\'\r\n";
  }

  // Check authed
  if (!auth_stat_.IsAuthed(cinfo_ptr)) {
    LOG(INFO) << "(" << ip_port() << ")Authentication required, close connection";
    return "-ERR NOAUTH Authentication required.\r\n";
  }
  
  uint64_t start_us, end_us;
  if (g_pika_conf->slowlog_slower_than() >= 0) {
    start_us = slash::NowMicros();
  }

  // For now, only shutdown need check local
  if (cinfo_ptr->is_local()) {
    if (ip_port().find("127.0.0.1") == std::string::npos
        && ip_port().find(g_pika_server->host()) == std::string::npos) {
      LOG(WARNING) << "\'shutdown\' should be localhost";
      return "-ERR \'shutdown\' should be localhost\r\n";
    }
  }

  // Initial
  c_ptr->Initial(argv_, cinfo_ptr);
  if (!c_ptr->res().ok()) {
    return c_ptr->res().message();
  }

  // Add read lock for no suspend command
  if (!cinfo_ptr->is_suspend()) {
    pthread_rwlock_rdlock(g_pika_server->rwlock());
  }

  std::string raw_args;
  if (cinfo_ptr->is_write()) {
      if (g_pika_conf->readonly()) {
        if (!cinfo_ptr->is_suspend()) {
            pthread_rwlock_unlock(g_pika_server->rwlock());
        }
        return "-ERR Server in read-only\r\n";
      }
      raw_args = RestoreArgs();
      g_pika_server->logger_->Lock();
  }

  c_ptr->Do();

  if (cinfo_ptr->is_write()) {
      if (c_ptr->res().ok()) {
          g_pika_server->logger_->Put(raw_args);
      }
      g_pika_server->logger_->Unlock();
  }

  if (!cinfo_ptr->is_suspend()) {
      pthread_rwlock_unlock(g_pika_server->rwlock());
  }

  if (g_pika_conf->slowlog_slower_than() >= 0) {
    uint64_t duration = slash::NowMicros() - start_us;
    if (duration > g_pika_conf->slowlog_slower_than()) {
      LOG(ERROR) << "command:" << opt << ", start_time(s): " << start_us / 1000000 << ", duration(us): " << duration;
    }
  }

  if (opt == kCmdNameAuth) {
    if(!auth_stat_.ChecknUpdate(c_ptr->res().raw_message())) {
      LOG(WARNING) << "(" << ip_port() << ")Wrong Password, close connection";
    }
  }
  return c_ptr->res().message();
}

int PikaClientConn::DealMessage() {
  
  self_thread_->PlusThreadQuerynum();
  
  if (argv_.empty()) return -2;
  std::string opt = argv_[0];
  slash::StringToLower(opt);
  std::string res = DoCmd(opt);
  
  memcpy(wbuf_ + wbuf_len_, res.data(), res.size());
  wbuf_len_ += res.size();
  set_is_reply(true);
  return 0;
}

// Initial permission status
void PikaClientConn::AuthStat::Init() {
  // Check auth required
  stat_ = g_pika_conf->userpass() == "" ?
    kLimitAuthed : kNoAuthed;
  if (stat_ == kLimitAuthed 
      && g_pika_conf->requirepass() == "") {
    stat_ = kAdminAuthed;
  }
}

// Check permission for current command
bool PikaClientConn::AuthStat::IsAuthed(const CmdInfo* const cinfo_ptr) {
  std::string opt = cinfo_ptr->name();
  if (opt == kCmdNameAuth) {
    return true;
  }
  const std::vector<std::string>& blacklist = g_pika_conf->vuser_blacklist();
  switch (stat_) {
    case kNoAuthed:
      return false;
    case kAdminAuthed:
      break;
    case kLimitAuthed:
      if (cinfo_ptr->is_admin_require() 
          || find(blacklist.begin(), blacklist.end(), opt) != blacklist.end()) {
      return false;
      }
      break;
    default:
      LOG(WARNING) << "Invalid auth stat : " << static_cast<unsigned>(stat_);
      return false;
  }
  return true;
}

// Update permission status
bool PikaClientConn::AuthStat::ChecknUpdate(const std::string& message) {
  // Situations to change auth status
  if (message == "USER") {
    stat_ = kLimitAuthed;
  } else if (message == "ROOT"){
    stat_ = kAdminAuthed;
  } else {
    return false;
  }
  return true;
}
