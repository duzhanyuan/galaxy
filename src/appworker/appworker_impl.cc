// Copyright (c) 2019, Baidu.com, Inc. All Rights Reserved
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "appworker_impl.h"

#include <algorithm>
#include <cctype>
#include <boost/bind.hpp>
#include <boost/function.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/split.hpp>
#include <gflags/gflags.h>
#include <timer.h>

#include "protocol/appmaster.pb.h"
#include "utils.h"

DECLARE_string(nexus_addr);
DECLARE_string(nexus_root_path);
DECLARE_string(appmaster_nexus_path);
DECLARE_string(appworker_exit_file);
DECLARE_string(appworker_agent_hostname_env);
DECLARE_string(appworker_agent_ip_env);
DECLARE_string(appworker_agent_port_env);
DECLARE_string(appworker_job_id_env);
DECLARE_string(appworker_pod_id_env);
DECLARE_string(appworker_task_ids_env);
DECLARE_string(appworker_cgroup_subsystems_env);
DECLARE_int32(appworker_fetch_task_timeout);
DECLARE_int32(appworker_fetch_task_interval);
DECLARE_int32(appworker_background_thread_pool_size);

namespace baidu {
namespace galaxy {

AppWorkerImpl::AppWorkerImpl() :
        mutex_(),
        start_time_(0),
        update_time_(0),
        nexus_(NULL),
        appmaster_stub_(NULL),
        backgroud_pool_(FLAGS_appworker_background_thread_pool_size) {
    nexus_ = new ::galaxy::ins::sdk::InsSDK(FLAGS_nexus_addr);
    backgroud_pool_.AddTask(boost::bind(&AppWorkerImpl::UpdateAppMasterStub, this));
    backgroud_pool_.DelayTask(
        FLAGS_appworker_fetch_task_interval,
        boost::bind(&AppWorkerImpl::FetchTask, this)
    );
}

AppWorkerImpl::~AppWorkerImpl() {
    if (NULL != nexus_) {
        delete nexus_;
    }

    if (NULL != appmaster_stub_) {
        delete appmaster_stub_;
    }

    backgroud_pool_.Stop(false);
}

void AppWorkerImpl::PrepareEnvs() {
    // 1.job_id
    char* c_job_id = getenv(FLAGS_appworker_job_id_env.c_str());

    if (NULL == c_job_id) {
        LOG(WARNING) << FLAGS_appworker_job_id_env << " is  not set";
        exit(-1);
    }

    job_id_ = std::string(c_job_id);
    // 2.pod_id
    char* c_pod_id = getenv(FLAGS_appworker_pod_id_env.c_str());

    if (NULL == c_pod_id) {
        LOG(WARNING) << FLAGS_appworker_pod_id_env << " is  not set";
        exit(-1);
    }

    pod_id_ = std::string(c_pod_id);
    // 3.hostname
    char* c_hostname = getenv(FLAGS_appworker_agent_hostname_env.c_str());

    if (NULL == c_hostname) {
        LOG(WARNING) << FLAGS_appworker_agent_hostname_env << " is  not set";
        exit(-1);
    }

    hostname_ = std::string(c_hostname);
    // 4.ip
    char* c_ip = getenv(FLAGS_appworker_agent_ip_env.c_str());

    if (NULL == c_ip) {
        LOG(WARNING) << FLAGS_appworker_agent_ip_env << " is  not set";
        exit(-1);
    }

    std::string ip = std::string(c_ip);
    // 5.port
    char* c_port = getenv(FLAGS_appworker_agent_port_env.c_str());

    if (NULL == c_port) {
        LOG(WARNING) << FLAGS_appworker_agent_port_env << " is  not set";
        exit(-1);
    }

    std::string port = std::string(c_port);
    endpoint_ = ip + ":" + port;
    // 6.task_ids
    char* c_task_ids = getenv(FLAGS_appworker_task_ids_env.c_str());

    if (NULL == c_task_ids) {
        LOG(WARNING) << FLAGS_appworker_task_ids_env << " is  not set";
        exit(-1);
    }

    std::string s_task_ids = std::string(c_task_ids);
    std::vector<std::string> task_ids;
    boost::split(task_ids,
                 s_task_ids,
                 boost::is_any_of(","),
                 boost::token_compress_on);
    // 7.cgroup subsystems
    char* c_cgroup_subsystems = getenv(FLAGS_appworker_cgroup_subsystems_env.c_str());

    if (NULL == c_cgroup_subsystems) {
        LOG(WARNING) << FLAGS_appworker_cgroup_subsystems_env << "is not set";
        exit(-1);
    }

    std::string s_cgroup_subsystems = std::string(c_cgroup_subsystems);
    std::vector<std::string> cgroup_subsystems;
    boost::split(cgroup_subsystems,
                 s_cgroup_subsystems,
                 boost::is_any_of(","),
                 boost::token_compress_on);
    // 8.task cgroup paths and ports
    std::vector<std::map<std::string, std::string> > task_cgroup_paths;
    std::vector<std::map<std::string, std::string> > task_ports;
    std::vector<std::string>::iterator t_it = task_ids.begin();

    for (; t_it != task_ids.end(); t_it++) {
        // cgroups
        std::map<std::string, std::string> cgroup_paths;
        std::vector<std::string>::iterator c_it = cgroup_subsystems.begin();

        for (; c_it != cgroup_subsystems.end(); ++c_it) {
            std::string key = "BAIDU_GALAXY_CONTAINER_" + *t_it + "_" + *c_it + "_PATH";
            transform(key.begin(), key.end(), key.begin(), toupper);
            char* c_value = getenv(key.c_str());

            if (NULL == c_value) {
                LOG(WARNING) << key << " is  not set";
                exit(-1);
            }

            std::string value = std::string(c_value);
            cgroup_paths.insert(std::make_pair(*c_it, value));
        }

        task_cgroup_paths.push_back(cgroup_paths);

        // ports
        std::map<std::string, std::string> ports;
        std::string port_names_key = "BAIDU_GALAXY_CONTAINER_" + *t_it + "_PORTLIST";
        char* c_port_names = getenv(port_names_key.c_str());

        if (NULL != c_port_names) {
            std::string s_port_names = std::string(c_port_names);

            if (s_port_names != "") {
                transform(s_port_names.begin(), s_port_names.end(),
                          s_port_names.begin(), toupper);
                std::vector<std::string> port_names;
                boost::split(port_names,
                             s_port_names,
                             boost::is_any_of(","),
                             boost::token_compress_on);
                std::vector<std::string>::iterator p_it = port_names.begin();

                for (; p_it != port_names.end(); ++p_it) {
                    std::string key = "BAIDU_GALAXY_CONTAINER_" + *t_it + "_PORT_" + *p_it;
                    transform(key.begin(), key.end(), key.begin(), toupper);
                    char* c_value = getenv(key.c_str());

                    if (NULL == c_value) {
                        LOG(WARNING) << key << " is  not set";
                        continue;
                    }

                    std::string value = std::string(c_value);
                    ports.insert(std::make_pair(*p_it, value));
                    LOG(INFO) << "port " << *p_it << " : " << value;
                }
            }
        }

        task_ports.push_back(ports);
    }

    PodEnv env;
    env.job_id = job_id_;
    env.pod_id = pod_id_;
    env.task_ids = task_ids;
    env.cgroup_subsystems = cgroup_subsystems;
    env.task_cgroup_paths = task_cgroup_paths;
    env.task_ports = task_ports;
    pod_manager_.SetPodEnv(env);

    return;
}

void AppWorkerImpl::Init() {
    start_time_ = baidu::common::timer::get_micros();
    PrepareEnvs();
    LOG(INFO)
            << "appworker start, endpoint: " << endpoint_ << ", "
            << "job_id: " << job_id_ << ", pod_id: " << pod_id_;

    return;
}

void AppWorkerImpl::UpdateAppMasterStub() {
    MutexLock lock(&mutex_);
    SDKError err;
    std::string new_endpoint;
    std::string key = FLAGS_nexus_root_path + "/" + FLAGS_appmaster_nexus_path;
    bool ok = nexus_->Get(key, &new_endpoint, &err);

    do {
        if (!ok) {
            LOG(WARNING)
                    << "get appmaster endpoint from nexus failed: "
                    << InsSDK::StatusToString(err);
            break;
        }

        if (appmaster_endpoint_ == new_endpoint
                && NULL != appmaster_stub_) {
            break;
        }

        appmaster_endpoint_ = new_endpoint;

        if (NULL != appmaster_stub_) {
            delete appmaster_stub_;
            appmaster_stub_ = NULL;
        }

        if (rpc_client_.GetStub(appmaster_endpoint_, &appmaster_stub_)) {
            LOG(INFO)
                    << "appmaster stub updated, endpoint: "
                    << appmaster_endpoint_;
        }
    } while (0);

    return;
}

void AppWorkerImpl::FetchTask() {
    MutexLock lock(&mutex_);
    // 1.validate pod status
    Pod pod;
    pod_manager_.QueryPod(pod);

    // exit or not, only when terminated, appworker exit
    if (proto::kPodTerminated == pod.status) {
        LOG(INFO) << "pod terminated";
        std::string value = boost::lexical_cast<std::string>(0);
        file::Write(FLAGS_appworker_exit_file, value);
        exit(0);
    }

    // 2.validate stub
    if (NULL == appmaster_stub_) {
        backgroud_pool_.AddTask(boost::bind(&AppWorkerImpl::UpdateAppMasterStub, this));
        backgroud_pool_.DelayTask(
            FLAGS_appworker_fetch_task_interval,
            boost::bind(&AppWorkerImpl::FetchTask, this)
        );
        return;
    }

    // 3.send request
    FetchTaskRequest* request = new FetchTaskRequest;
    FetchTaskResponse* response = new FetchTaskResponse;
    request->set_jobid(job_id_);
    request->set_podid(pod_id_);
    request->set_endpoint(endpoint_);
    request->set_start_time(start_time_);
    request->set_update_time(update_time_);
    request->set_status(pod.status);
    request->set_reload_status(pod.reload_status);
    LOG(INFO) << "task status: " << proto::PodStatus_Name(pod.status);

    if (kPodStageReloading == pod.stage) {
        LOG(INFO) << "task reload_status: " << proto::PodStatus_Name(pod.reload_status);
    }

    boost::function<void (const FetchTaskRequest*, FetchTaskResponse*, bool, int)> fetch_task_callback;
    fetch_task_callback = boost::bind(&AppWorkerImpl::FetchTaskCallback,
                                      this, _1, _2, _3, _4);
    rpc_client_.AsyncRequest(appmaster_stub_, &AppMaster_Stub::FetchTask,
                             request, response, fetch_task_callback,
                             FLAGS_appworker_fetch_task_timeout, 0);
}

void AppWorkerImpl::FetchTaskCallback(const FetchTaskRequest* request,
                                      FetchTaskResponse* response,
                                      bool failed, int /*error*/) {
    MutexLock lock(&mutex_);

    do {
        // rpc error
        if (failed) {
            LOG(WARNING) << "fetch task failed, rpc failed";
            backgroud_pool_.AddTask(boost::bind(&AppWorkerImpl::UpdateAppMasterStub, this));
            break;
        }

        if (!response->has_error_code()) {
            LOG(WARNING) << "fetch task failed, error_code not found";
            break;
        }

        ErrorCode error_code = response->error_code();
        LOG(INFO)
                << "fetch task call back, "
                << "update_time: " << response->update_time() << ", "
                << "status: " << proto::Status_Name(error_code.status());

        if (proto::kJobNotFound == error_code.status()) {
            LOG(WARNING) << "fetch task: kJobNotFound";
            break;
        }

        if (proto::kSuspend == error_code.status()) {
            LOG(WARNING) << "fetch task: kSuspend";
            break;
        }

        if (proto::kTerminate == error_code.status()) {
            LOG(WARNING) << "fetch task: kTerminate";
            pod_manager_.TerminatePod();
            break;
        }

        // ignore expired actions
        if (!response->has_update_time()
                || response->update_time() <= update_time_) {
            LOG(WARNING) << "ignore expire action, current: " << update_time_;
            break;
        }

        update_time_ = response->update_time();
        LOG(INFO) << "update_time updated";

        if (!response->has_pod()) {
            LOG(WARNING) << "ignore empty pod description";
            break;
        }

        LOG(INFO) << "fetch task: " << proto::Status_Name(error_code.status());
        pod_manager_.SetPodDescription(response->pod());

        switch (error_code.status()) {
        case proto::kReload:
            pod_manager_.ReloadPod();
            break;

        case proto::kRebuild:
            pod_manager_.RebuildPod();
            break;

        default:
            break;
        }
    } while (0);

    backgroud_pool_.DelayTask(
        FLAGS_appworker_fetch_task_interval,
        boost::bind(&AppWorkerImpl::FetchTask, this)
    );
    return;
}

}   // ending namespace galaxy
}   // ending namespace baidu
