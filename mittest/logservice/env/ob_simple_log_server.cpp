/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */
#define private public
#include "ob_simple_log_server.h"
#undef private

#include "lib/file/file_directory_utils.h"
#include "logservice/palf/log_define.h"
#include <iostream>
#define USING_LOG_PREFIX RPC_TEST
#include<dirent.h>
#include <memory>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include "util/easy_mod_stat.h"
#include "lib/oblog/ob_log.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/random/ob_random.h"
#include "common/log/ob_log_constants.h"
#include "share/ob_io_device_helper.h"
#include "share/ob_thread_mgr.h"
#include "logservice/palf/palf_options.h"

namespace oceanbase
{

namespace unittest
{
using namespace oceanbase;
using namespace oceanbase::rpc;
using namespace oceanbase::rpc::frame;
using namespace oceanbase::obrpc;
using namespace oceanbase::common;
using namespace oceanbase::share;
using namespace palf;
using namespace palf::election;
using namespace logservice;

int MockNetKeepAliveAdapter::init(unittest::ObLogDeliver *log_deliver)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(log_deliver)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    log_deliver_ = log_deliver;
  }
  return ret;
}

bool MockNetKeepAliveAdapter::in_black_or_stopped(const common::ObAddr &server)
{
  return log_deliver_->need_filter_packet_by_blacklist(server);
}

bool MockNetKeepAliveAdapter::is_server_stopped(const common::ObAddr &server)
{
  return log_deliver_->need_filter_packet_by_blacklist(server);
}

uint32_t get_local_addr(const char *dev_name)
{
  int fd, intrface;
  struct ifreq buf[16];
  struct ifconf ifc;

  if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
    return 0;
  }

  ifc.ifc_len = sizeof(buf);
  ifc.ifc_buf = (caddr_t)buf;
  if (ioctl(fd, SIOCGIFCONF, (char *)&ifc) != 0) {
    close(fd);
    return 0;
  }

  intrface = static_cast<int>(ifc.ifc_len / sizeof(struct ifreq));
  while (intrface-- > 0) {
    if (ioctl(fd, SIOCGIFFLAGS, (char *)&buf[intrface]) != 0) {
      continue;
    }
    if ((buf[intrface].ifr_flags & IFF_LOOPBACK) != 0)
      continue;
    if (!(buf[intrface].ifr_flags & IFF_UP))
      continue;
    if (dev_name != NULL && strcmp(dev_name, buf[intrface].ifr_name))
      continue;
    if (!(ioctl(fd, SIOCGIFADDR, (char *)&buf[intrface]))) {
      close(fd);
      return ((struct sockaddr_in *)(&buf[intrface].ifr_addr))->sin_addr.s_addr;
    }
  }
  close(fd);
  return 0;
}

std::string get_local_ip()
{
  uint32_t ip = get_local_addr("bond0");
 if (ip == 0) {
    ip = get_local_addr("eth0");
  }
  if (ip == 0) {
    return "";
  }
  return inet_ntoa(*(struct in_addr *)(&ip));
}

int ObSimpleLogServer::simple_init(
    const std::string &cluster_name,
    const common::ObAddr &addr,
    const int64_t node_id,
    const bool is_bootstrap = false)
{
  int ret = OB_SUCCESS;
  ObTimeGuard guard("simple_init", 0);
  SERVER_LOG(INFO, "simple_log_server simple_init start", K(node_id), K(addr_));
  tenant_base_ = OB_NEW(ObTenantBase, "TestBase", node_id);
  auto malloc = ObMallocAllocator::get_instance();
  if (NULL == malloc->get_tenant_ctx_allocator(node_id, 0)) {
    malloc->create_and_add_tenant_allocator(node_id);
  }
  tenant_base_->init();
  tenant_base_->set(&log_service_);
  tenant_base_->set(&detector_);
  ObTenantEnv::set_tenant(tenant_base_);
  assert(&log_service_ == MTL(logservice::ObLogService*));
  guard.click("init tenant_base");
  node_id_ = node_id;

  if (is_bootstrap && OB_FAIL(init_memory_dump_timer_())) {
    SERVER_LOG(ERROR, "init_memory_dump_timer_ failed", K(ret), K_(node_id));
  } else if (FALSE_IT(guard.click("init_memory_dump_timer_"))
      || OB_FAIL(init_network_(addr, is_bootstrap))) {
    SERVER_LOG(WARN, "init_network failed", K(ret), K(addr));
  } else if (FALSE_IT(guard.click("init_network_")) || OB_FAIL(init_io_(cluster_name))) {
    SERVER_LOG(WARN, "init_io failed", K(ret), K(addr));
  } else if (FALSE_IT(guard.click("init_io_")) || OB_FAIL(init_log_service_())) {
    SERVER_LOG(WARN, "init_log_service failed", K(ret), K(addr));
  } else {
    guard.click("init_log_service_");
    SERVER_LOG(INFO, "simple_log_server init success", KPC(this), K(guard));
  }
  return ret;
}

int ObSimpleLogServer::update_disk_opts(const PalfDiskOptions &opts)
{
  return palf_env_->palf_env_impl_.disk_options_wrapper_.update_disk_options(opts);
}

int ObSimpleLogServer::init_memory_dump_timer_()
{
  int ret = OB_SUCCESS;
  common::ObFunction<bool()> print_memory_info = [=](){
      ObMallocAllocator::get_instance()->print_tenant_memory_usage(node_id_);
      ObMallocAllocator::get_instance()->print_tenant_ctx_memory_usage(node_id_);
      ObMallocAllocator::get_instance()->print_tenant_memory_usage(OB_SERVER_TENANT_ID);
      ObMallocAllocator::get_instance()->print_tenant_ctx_memory_usage(OB_SERVER_TENANT_ID);
      return false;};
  if (OB_FAIL(timer_.init_and_start(1, 1_s, "test timer"))) {
    SERVER_LOG(ERROR, "ObOccamTimer init failed", K(ret), K_(node_id));
  } else if (OB_FAIL(timer_.schedule_task_repeat(timer_handle_, 1_s, print_memory_info))) {
    SERVER_LOG(ERROR, "schedule_task failed", K(ret), K_(node_id));
  }
  return ret;
}

int ObSimpleLogServer::init_network_(const common::ObAddr &addr, const bool is_bootstrap = false)
{
  int ret = OB_SUCCESS;
  ObNetOptions opts;
  opts.rpc_io_cnt_ = 10;
  opts.tcp_user_timeout_ = 10 * 1000 * 1000; // 10s
  addr_ = addr;
  obrpc::ObRpcNetHandler::CLUSTER_ID = 1;
  if (is_bootstrap && OB_FAIL(net_.init(opts, 0))) {
    SERVER_LOG(ERROR, "net init fail", K(ret));
  } else if (OB_FAIL(deliver_.init(addr, is_bootstrap))) {
    SERVER_LOG(ERROR, "deliver_ init failed", K(ret));
  } else if (is_bootstrap && OB_FAIL(net_.add_rpc_listen(addr_.get_port(), handler_, transport_))) {
//  } else if (is_bootstrap &&(
//             OB_FAIL(net_.set_rpc_port(addr_.get_port()))
//             || OB_FAIL(net_.rpc_net_register(handler_, transport_))
//             || OB_FAIL(net_.net_keepalive_register()))) {
    SERVER_LOG(ERROR, "net_ listen failed", K(ret));
  } else if (is_bootstrap && OB_FAIL(srv_proxy_.init(transport_))) {
    SERVER_LOG(ERROR, "init srv_proxy_ failed");
  } else {
    deliver_.node_id_ = node_id_;
    SERVER_LOG(INFO, "init_network success", K(ret), K(addr_), K(node_id_), K(opts));
  }
  return ret;
}

int ObSimpleLogServer::init_io_(const std::string &cluster_name)
{
  int ret = OB_SUCCESS;
  ObTimeGuard guard("init_io_", 0);
  const std::string logserver_dir = cluster_name + "/port_" + std::to_string(addr_.get_port());
  std::string data_dir = logserver_dir;
  std::string clog_dir = logserver_dir + "/clog";
  std::string slog_dir = logserver_dir + "/slog";
  std::string sstable_dir = logserver_dir + "/sstable";
  std::vector<std::string> dirs{data_dir, slog_dir, clog_dir, sstable_dir};

  for (auto &dir : dirs) {
    if (-1 == mkdir(dir.c_str(), 0777)) {
      if (errno == EEXIST) {
        ret = OB_SUCCESS;
        SERVER_LOG(INFO, "for restart");
      } else {
        SERVER_LOG(WARN, "mkdir failed", K(errno));
        ret = OB_IO_ERROR;
        break;
      }
    }
  }
  guard.click("mkdir");
  if (OB_SUCC(ret)) {
    io_device_ = OB_NEW(ObLocalDevice, "TestBase");

    blocksstable::ObStorageEnv storage_env;
    storage_env.data_dir_ = data_dir.c_str();
    storage_env.sstable_dir_ = sstable_dir.c_str();
    storage_env.default_block_size_ = OB_DEFAULT_MACRO_BLOCK_SIZE;
    storage_env.data_disk_size_ = 1024 * 1024 * 1024;
    storage_env.data_disk_percentage_ = 0;
    storage_env.log_disk_size_ = 2LL * 1024 * 1024 * 1024;
    storage_env.log_disk_percentage_ = 0;

    storage_env.log_spec_.log_dir_ = slog_dir.c_str();
    storage_env.log_spec_.max_log_file_size_ = ObLogConstants::MAX_LOG_FILE_SIZE;
    storage_env.clog_dir_ = clog_dir.c_str();
    iod_opts_.opts_ = iod_opt_array_;
    iod_opt_array_[0].set("data_dir", storage_env.data_dir_);
    iod_opt_array_[1].set("sstable_dir", storage_env.sstable_dir_);
    iod_opt_array_[2].set("block_size", reinterpret_cast<const char*>(&storage_env.default_block_size_));
    iod_opt_array_[3].set("datafile_disk_percentage", storage_env.data_disk_percentage_);
    iod_opt_array_[4].set("datafile_size", storage_env.data_disk_size_);
    iod_opts_.opt_cnt_ = MAX_IOD_OPT_CNT;
    const int64_t DEFATULT_RESERVED_SIZE = 10 * 1024 * 1024 * 1024ul;
    if (OB_FAIL(io_device_->init(iod_opts_))) {
      SERVER_LOG(ERROR, "init io device fail", K(ret));
    } else if (OB_FAIL(log_block_pool_.init(storage_env.clog_dir_))) {
      SERVER_LOG(ERROR, "init log pool fail", K(ret));
    } else {
      guard.click("init io_device");
      clog_dir_ = clog_dir;
      SERVER_LOG(INFO, "init_io_ successs", K(ret), K(guard));
    }
    if (OB_SUCC(ret)) {
      log_block_pool_.get_tenants_log_disk_size_func_ = [this, &storage_env](int64_t &log_disk_size) -> int
      {
        log_disk_size = log_block_pool_.lower_align_(storage_env.log_disk_size_);
        return OB_SUCCESS;
      };
      if (OB_FAIL(log_block_pool_.start(storage_env.log_disk_size_))) {
        LOG_ERROR("log pool start failed", KR(ret));
      }
    }
  }
  return ret;
}

int ObSimpleLogServer::init_log_service_()
{
  int ret = OB_SUCCESS;
  // init deps of log_service
  palf::PalfOptions opts;
  opts.disk_options_.log_disk_usage_limit_size_ = 10 * 1024 * 1024 * 1024ul;
  opts.disk_options_.log_disk_utilization_threshold_ = 80;
  opts.disk_options_.log_disk_utilization_limit_threshold_ = 95;
  std::string clog_dir = clog_dir_ + "/tenant_1";
  allocator_ = OB_NEW(ObTenantMutilAllocator, "TestBase", node_id_);
  ObMemAttr attr(1, "SimpleLog");
  net_keepalive_ = MTL_NEW(MockNetKeepAliveAdapter, "SimpleLog");

  if (OB_FAIL(net_keepalive_->init(&deliver_))) {
  } else if (OB_FAIL(log_service_.init(opts, clog_dir.c_str(), addr_, allocator_, transport_, &ls_service_,
      &location_service_, &reporter_, &log_block_pool_, &sql_proxy_, net_keepalive_))) {
    SERVER_LOG(ERROR, "init_log_service_ fail", K(ret));
  } else if (OB_FAIL(mock_election_map_.init())) {
    SERVER_LOG(ERROR, "mock_election_map_ init fail", K(ret));
  } else {
    palf_env_ = log_service_.get_palf_env();
    palf_env_->palf_env_impl_.log_rpc_.tenant_id_ = OB_SERVER_TENANT_ID;
    SERVER_LOG(INFO, "init_log_service_ success", K(ret));
  }
  return ret;
}

int ObSimpleLogServer::simple_start(const bool is_bootstrap = false)
{
  int ret = OB_SUCCESS;
  if (is_bootstrap && OB_FAIL(net_.start())) {
    SERVER_LOG(ERROR, "net start fail", K(ret));
  } else if (OB_FAIL(deliver_.start())) {
    SERVER_LOG(ERROR, "deliver_ start failed", K(ret));
  } else if (OB_FAIL(log_service_.arb_service_.start())) {
    SERVER_LOG(ERROR, "arb_service start failed", K(ret));
  }
  // do not start entire log_service_ for now, it will
  // slow down cases running
  return ret;
}

int ObSimpleLogServer::simple_close(const bool is_shutdown = false)
{
  int ret = OB_SUCCESS;
  ObTimeGuard guard("simple_close", 0);
  deliver_.destroy(is_shutdown);
  guard.click("destroy");
  log_service_.destroy();

  log_block_pool_.destroy();
  guard.click("destroy_palf_env");

  if (is_shutdown) {
    net_.rpc_shutdown();
    net_.stop();
    net_.wait();
    net_.destroy();

    timer_handle_.stop_and_wait();
    timer_.stop_and_wait();
  }
  SERVER_LOG(INFO, "stop LogService success", K(ret), K(is_shutdown), K(guard));
  return ret;
}

int ObSimpleLogServer::simple_restart(const std::string &cluster_name, const int64_t node_idx)
{
  int ret = OB_SUCCESS;
  ObTimeGuard guard("simple_restart", 0);
  if (OB_FAIL(simple_close())) {
    SERVER_LOG(ERROR, "simple_close failed", K(ret));
  } else if (FALSE_IT(guard.click("simple_close")) || OB_FAIL(simple_init(cluster_name, addr_, node_idx))) {
    SERVER_LOG(ERROR, "simple_init failed", K(ret));
  } else if (FALSE_IT(guard.click("simple_init")) || OB_FAIL(simple_start())) {
    SERVER_LOG(ERROR, "simple_start failed", K(ret));
  } else {
    guard.click("simple_start");
    SERVER_LOG(INFO, "simple_restart success", K(ret), K(guard));
  }
  return ret;
}

int ObMittestBlacklist::init(const common::ObAddr &self)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(blacklist_.create(1024))) {
    SERVER_LOG(WARN, "create blacklist_ failed", K(ret));
  } else if (false == blacklist_.created()) {
    SERVER_LOG(WARN, "blacklist_ created failed");
  } else {
    self_ = self;
  }
  return ret;
}

void ObMittestBlacklist::block_net(const ObAddr &src)
{
  blacklist_.set_refactored(src);
  SERVER_LOG(INFO, "block_net", K(src), K(blacklist_));
}

void ObMittestBlacklist::unblock_net(const ObAddr &src)
{
  blacklist_.erase_refactored(src);
  SERVER_LOG(INFO, "unblock_net", K(src), K(blacklist_));
}

bool ObMittestBlacklist::need_filter_packet_by_blacklist(const ObAddr &addr)
{
  return OB_HASH_EXIST == blacklist_.exist_refactored(addr);
}


void ObMittestBlacklist::set_rpc_loss(const ObAddr &src, const int loss_rate)
{
  // not thread safe
  int old_loss_rate = -1;
  bool is_exist = false;
  for (int64_t i = 0; i < rpc_loss_config_.count(); ++i) {
    LossConfig &tmp_config = rpc_loss_config_[i];
    if (tmp_config.src_ == src) {
      is_exist = true;
      // src exists, update its loss_rate
      old_loss_rate = tmp_config.loss_rate_;
      tmp_config.loss_rate_ = loss_rate;
    }
  }
  if (!is_exist) {
    LossConfig loss_config(src, loss_rate);
    rpc_loss_config_.push_back(loss_config);
  }
  SERVER_LOG(INFO, "set_rpc_loss", K(src), K(loss_rate), K(old_loss_rate));
}

void ObMittestBlacklist::reset_rpc_loss(const ObAddr &src)
{
  // not thread safe
  int64_t idx = -1;
  for (int64_t i = 0; i < rpc_loss_config_.count(); ++i) {
    LossConfig tmp_config;
    int ret = rpc_loss_config_.at(i, tmp_config);
    if (OB_SUCCESS != ret) {
      SERVER_LOG(WARN, "at failed", K(ret), K(i), "count", rpc_loss_config_.count());
    } else if (tmp_config.src_ == src) {
      // src exists, update its loss_rate
      idx = i;
    }
  }
  if (idx >= 0) {
    rpc_loss_config_.remove(idx);
  }
  SERVER_LOG(INFO, "reset_rpc_loss", K(src), K(idx));
}

void ObMittestBlacklist::get_loss_config(const ObAddr &src, bool &exist, LossConfig &loss_config)
{
  // not thread safe
  exist = false;
  for (int64_t i = 0; i < rpc_loss_config_.count(); ++i) {
    LossConfig tmp_config;
    int ret = rpc_loss_config_.at(i, tmp_config);
    if (OB_SUCCESS != ret) {
      SERVER_LOG(WARN, "at failed", K(ret));
    } else if (tmp_config.src_ == src) {
      exist = true;
      loss_config = tmp_config;
    }
  }
  SERVER_LOG(INFO, "get_loss_config", K(src), K(loss_config));
}

bool ObMittestBlacklist::need_drop_by_loss_config(const ObAddr &addr)
{
  bool bool_ret = false;
  bool exist = false;
  LossConfig loss_config;
  get_loss_config(addr, exist, loss_config);
  if (exist && loss_config.loss_rate_ > 0) {
    const int64_t random_num = ObRandom::rand(1, 100);
    bool_ret = random_num <= loss_config.loss_rate_ ? true : false;
    if (bool_ret) {
      SERVER_LOG(INFO, "need drop req by loss config", K(addr));
    }
  }
  return bool_ret;
}

int ObLogDeliver::init(const common::ObAddr &self, const bool is_bootstrap)
{
  int ret = OB_SUCCESS;
  // init_all_propocessor_();
  if (is_bootstrap && OB_FAIL(ObMittestBlacklist::init(self))) {
    SERVER_LOG(WARN, "ObMittestBlacklist init failed", K(ret));
  } else if (OB_FAIL(TG_CREATE_TENANT(lib::TGDefIDs::TEST7, tg_id_))) {
    SERVER_LOG(WARN, "ObSimpleThreadPool::init failed", K(ret));
  } else {
    is_inited_ = true;
    SERVER_LOG(INFO, "ObLogDeliver init success", KP(&blacklist_));
  }
  return ret;
}

void ObLogDeliver::destroy(const bool is_shutdown)
{
  if (IS_NOT_INIT) {
    SERVER_LOG(INFO, "ObLogDeliver not init");
  } else {
    RWLock::WLockGuard guard(lock_);
    stop();
    wait();
    is_inited_ = false;
    TG_DESTROY(tg_id_);
    if (is_shutdown) {
      blacklist_.destroy();
    }
    tg_id_ = 0;
    SERVER_LOG(INFO, "destroy ObLogDeliver");
  }
}

int ObLogDeliver::start()
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    SERVER_LOG(ERROR, "ObLogDeliver not init");
  } else if (OB_FAIL(TG_SET_HANDLER_AND_START(tg_id_, *this))) {
    SERVER_LOG(ERROR, "start ObLogDeliver failed");
  } else {
    is_stopped_ = false;
    SERVER_LOG(INFO, "start ObLogDeliver success");
  }
  return ret;
}

void ObLogDeliver::stop()
{
  if (IS_NOT_INIT) {
    SERVER_LOG_RET(WARN, OB_NOT_INIT, "ObLogDeliver stop failed");
  } else {
    is_stopped_ = true;
    TG_STOP(tg_id_);
    SERVER_LOG(INFO, "stop ObLogDeliver success");
  }
}

int ObLogDeliver::wait()
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    SERVER_LOG(ERROR, "ObLogDeliver not inited!!!", K(ret));
  } else {
    TG_WAIT(tg_id_);
    SERVER_LOG(INFO, "wait ObLogDeliver success", K(tg_id_));
  }
  return ret;
}

int ObLogDeliver::deliver(rpc::ObRequest &req)
{
  int ret = OB_SUCCESS;
  const ObRpcPacket &pkt = dynamic_cast<const ObRpcPacket&>(req.get_packet());
  SERVER_LOG(INFO, "deliver request", K(pkt), K(req.ez_req_), KPC(palf_env_impl_));
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else {
    do {
      RWLock::RLockGuard guard(lock_);
      if (false == is_stopped_) {
        ret = TG_PUSH_TASK(tg_id_, &req);
        if (OB_SUCCESS != ret && OB_EAGAIN != ret) {
          SERVER_LOG(ERROR, "deliver request failed", K(ret), K(pkt), K(req.ez_req_), KPC(palf_env_impl_));
        }
      } else {
        break;
      }
    } while (OB_EAGAIN == ret);
  }
  return ret;
}

void ObLogDeliver::handle(void *task)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    SERVER_LOG(WARN, "ObLogDeliver not init", K(ret));
  } else if (OB_ISNULL(task)) {
    SERVER_LOG(WARN, "invalid argument", KP(task));
  } else {
    rpc::ObRequest *req = static_cast<rpc::ObRequest *>(task);
    SERVER_LOG(INFO, "handle request", KP(req->ez_req_));
    (void) handle_req_(*req);
  }
}

int ObLogDeliver::handle_req_(rpc::ObRequest &req)
{
  int ret = OB_SUCCESS;
  // tenant_id of log servers are different for clear log record,
  // so set tenant_id of RpcPacket to OB_INVALID_TENANT_ID, this
  // will make the PALF_ENV_ID to OB_SERVER_TENANT_ID
  const ObRpcPacket &pkt = dynamic_cast<const ObRpcPacket&>(req.get_packet());
  ObRpcPacket &modify_pkt = const_cast<ObRpcPacket&>(pkt);
  modify_pkt.set_tenant_id(OB_SERVER_TENANT_ID);
  SERVER_LOG(TRACE, "handle_req_ trace", K(pkt), K(node_id_));
  const ObRpcPacketCode pcode = pkt.get_pcode();
  ObFunction<bool(const ObAddr &src)> filter = [&](const ObAddr &src) -> bool {
    if (this->need_filter_packet_by_blacklist(src)) {
      SERVER_LOG(WARN, "need_filter_packet_by_blacklist", K(ret), K(pcode), K(src), K(blacklist_), KPC(palf_env_impl_));
      return true;
    } else if (this->need_drop_by_loss_config(src)) {
      SERVER_LOG(WARN, "need_drop_by_loss_config", K(ret), K(pcode), K(src), K(rpc_loss_config_), KPC(palf_env_impl_));
      return true;
    }
    return false;
  };
  switch (pkt.get_pcode()) {
    #define PROCESS(processer) \
    processer p;\
    p.init();\
    p.set_ob_request(req);\
    p.set_filter(&filter);  \
    p.run();      \
    break;
    case obrpc::OB_LOG_PUSH_REQ: {
      PROCESS(LogPushReqP);
    }
    case obrpc::OB_LOG_PUSH_RESP: {
      PROCESS(LogPushRespP);
    }
    case obrpc::OB_LOG_FETCH_REQ: {
      PROCESS(LogFetchReqP);
    }
    case obrpc::OB_LOG_PREPARE_REQ: {
      PROCESS(LogPrepareReqP);
    }
    case obrpc::OB_LOG_PREPARE_RESP: {
      PROCESS(LogPrepareRespP);
    }
    case obrpc::OB_LOG_CHANGE_CONFIG_META_REQ: {
      PROCESS(LogChangeConfigMetaReqP);
    }
    case obrpc::OB_LOG_CHANGE_CONFIG_META_RESP: {
      PROCESS(LogChangeConfigMetaRespP);
    }
    case obrpc::OB_LOG_CHANGE_MODE_META_REQ: {
      PROCESS(LogChangeModeMetaReqP);
    }
    case obrpc::OB_LOG_CHANGE_MODE_META_RESP: {
      PROCESS(LogChangeModeMetaRespP);
    }
    case obrpc::OB_LOG_NOTIFY_REBUILD_REQ: {
      PROCESS(LogNotifyRebuildReqP)
    }
    case obrpc::OB_LOG_COMMITTED_INFO: {
      PROCESS(CommittedInfoP)
    }
    case obrpc::OB_LOG_LEARNER_REQ: {
      PROCESS(LogLearnerReqP)
    }
    case obrpc::OB_LOG_REGISTER_PARENT_REQ: {
      PROCESS(LogRegisterParentReqP)
    }
    case obrpc::OB_LOG_REGISTER_PARENT_RESP: {
      PROCESS(LogRegisterParentRespP)
    }
    case obrpc::OB_LOG_ELECTION_PREPARE_REQUEST : {
      PROCESS(ElectionPrepareRequestMsgP)
    }
    case obrpc::OB_LOG_ELECTION_PREPARE_RESPONSE : {
      PROCESS(ElectionPrepareResponseMsgP)
    }
    case obrpc::OB_LOG_ELECTION_ACCEPT_REQUEST : {
      PROCESS(ElectionAcceptRequestMsgP)
    }
    case obrpc::OB_LOG_ELECTION_ACCEPT_RESPONSE : {
      PROCESS(ElectionAcceptResponseMsgP)
    }
    case obrpc::OB_LOG_ELECTION_CHANGE_LEADER_REQUEST : {
      PROCESS(ElectionChangeLeaderMsgP)
    }
    case obrpc::OB_LOG_GET_MC_ST: {
      modify_pkt.set_tenant_id(node_id_);
      PROCESS(LogGetMCStP)
    }
    case obrpc::OB_LOG_ARB_PROBE_MSG: {
      modify_pkt.set_tenant_id(node_id_);
      PROCESS(logservice::LogServerProbeP)
    }
    case obrpc::OB_LOG_CONFIG_CHANGE_CMD: {
      modify_pkt.set_tenant_id(node_id_);
      PROCESS(LogMembershipChangeP)
    }
    case obrpc::OB_LOG_GET_PALF_STAT: {
      modify_pkt.set_tenant_id(node_id_);
      PROCESS(LogGetPalfStatReqP)
    }
    case obrpc::OB_LOG_CHANGE_ACCESS_MODE_CMD: {
      modify_pkt.set_tenant_id(node_id_);
      PROCESS(LogChangeAccessModeP)
    }
    case obrpc::OB_LOG_FLASHBACK_CMD: {
      modify_pkt.set_tenant_id(node_id_);
      PROCESS(LogFlashbackMsgP)
    }
    case obrpc::OB_LOG_GET_LEADER_MAX_SCN: {
      modify_pkt.set_tenant_id(node_id_);
      PROCESS(LogGetLeaderMaxScnP)
    }
    case obrpc::OB_LOG_GET_STAT: {
      modify_pkt.set_tenant_id(node_id_);
      PROCESS(LogGetStatP)
    }
    default:
      SERVER_LOG(ERROR, "invalid req type", K(pkt.get_pcode()));
      break;
  }
  return ret;
}

} // unittest
} // oceanbase
