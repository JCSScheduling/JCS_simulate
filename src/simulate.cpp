/* Copyright (c) 2010-2020. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

/* This example shows how to block on the completion of a set of communications.
 *
 * As for the other asynchronous examples, the sender initiate all the message it wants to send and
 * pack the resulting simgrid::s4u::CommPtr objects in a vector. All message thus occur concurrently.
 *
 * The sender then blocks until all ongoing communication terminate, using simgrid::s4u::Comm::wait_all()
 *
 */

#include "simgrid/s4u.hpp"
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <string>
#include <cmath>
#include <regex>
#include <algorithm>
#include <boost/stacktrace.hpp>
using std::string;
using std::vector;
using std::to_string;
using simgrid::s4u::Mailbox;
using simgrid::s4u::CommPtr;
using simgrid::s4u::Comm;
using simgrid::s4u::Host;
using simgrid::s4u::Link;
// using simgrid::s4u::this_actor;

XBT_LOG_NEW_DEFAULT_CATEGORY(dml, "message specific for dml simulation");

struct timeInfo {
    int workerid;
    string host;
    double comm_time;
    double compute_time;
};

struct Flow {
    string flow_id; // [coflow_id]-src-dst
    simgrid::s4u::ActorPtr src_actor;
    simgrid::s4u::ActorPtr dst_actor;
    simgrid::s4u::Host* src_host;
    simgrid::s4u::Host* dst_host;
    string src_gpu;
    string dst_gpu;
    int src_port;
    int dst_port;
    int flow_size;   //MB
    double compute_time;
    string flow_type; //PULL / PUSH
    string status;    // init, active, suspend, done,注意，暂停flow时，只能suspend 发送者，suspend接收者没有效果
    int locof;         // 端口争用系数
    double prio;          // 优先级，考虑size
    double prio_c;        // 考虑计算时间的终极优先级


    Flow(string id): flow_id(id) { }

    bool operator()(const Flow* obj) const {
        return (obj->flow_id == flow_id);
    }
};

struct CoFlow {
    string coflow_id;  // jobid-iter-[PULL/PUSH]
    int flow_num;      // coflow应该有的flow数，初始一次性设置
    int flow_cnt;      // coflow中未完成的flow数，初始等于flow_num， 每删除一个流减1. 当flow_cnt=0时，表示该coflow结束
    int locof;         // 端口争用系数
    double prio;          // 优先级，考虑size
    double prio_c;        // 考虑计算时间的终极优先级
    double avg_ct;     // 平均计算时间
    std::vector<Flow*> flows;
    CoFlow(string id): coflow_id(id) { }
    bool operator()(const CoFlow* obj) const {
        return (obj->coflow_id == coflow_id);
    }
};

struct JobInfo {
    int jobid;  
    int param_size;
    vector<double> compute_time;
    vector<string> placement;
};

bool job_cmp(JobInfo a, JobInfo b){
    if(a.jobid < b.jobid)
        return true;
    else
        return false;
}


bool is_nvlink(simgrid::s4u::Link* link) {
    string name = link->get_name();
    if(name.find_first_of("gpu")!= std::string::npos && 
        name.find_first_of("gpu") != name.find_last_of("gpu"))
        return true;
    else
        return false;
}

bool is_pcielink(simgrid::s4u::Link* link) {
    string name = link->get_name();
    if(name.find_first_of("gpu")!= std::string::npos && 
        name.find_first_of("gpu") == name.find_last_of("gpu"))
        return true;
    else
        return false;
}

bool is_ethlink(simgrid::s4u::Link* link) {
    string name = link->get_name();
    if(name.find("l-sw0")!= std::string::npos)
        return true;
    else
        return false;
}

bool in_same_machine(simgrid::s4u::ActorPtr ps, simgrid::s4u::ActorPtr worker) {
    string ps_name = ps->get_host()->get_name();
    string worker_name = worker->get_host()->get_name();
    int ps_pos = stoi(ps_name.erase(0, 3));
    int worker_pos = stoi(worker_name.erase(0, 3));
    if(abs(worker_pos - ps_pos) < 8)
        return true;
    else
        return false;
}

int get_gpuid(string gpu_name) {
    return stoi(gpu_name.substr(3));
}

bool cmp(timeInfo a, timeInfo b){
    // if(std::min(a.comm_time, b.compute_time) <= std::min(a.compute_time, b.comm_time))
    // if(b.compute_time < a.compute_time)
    if(std::min(a.comm_time+a.compute_time, b.compute_time) < std::min(a.compute_time, b.comm_time+b.compute_time))
        return true;
    else
        return false;
}


// 所有优先级的计算全部基于目前存在于flows集合中的flow，不假设未来会到来的flow，因为这样后面无法调度，而且征用程度和实际有偏差
bool cmp_sjf(CoFlow* c1, CoFlow* c2) {
    // 把空coflow始终排到后面
    if(c1->flows.size() ==0 && c2->flows.size() != 0)
        return false;
    if(c1->flows.size() !=0 && c2->flows.size() == 0)
        return true;
        // std::cout<<"ERROR sjf_cmp no flow"<<std::endl;
    return c1->flows[0]->flow_size < c2->flows[0]->flow_size;
}

bool cmp_locof(CoFlow* c1, CoFlow* c2) {
    // 把空coflow始终排到后面
    if(c1->flows.size() ==0 && c2->flows.size() != 0)
        return false;
    if(c1->flows.size() !=0 && c2->flows.size() == 0)
        return true;
        // std::cout<<"ERROR locof_cmp no flow"<<std::endl;
    return c1->locof < c2->locof;
}

//不考虑计算时间的优先级比较
bool cmp_prio(CoFlow* c1, CoFlow* c2) {
    // 把空coflow始终排到后面
    if(c1->flows.size() ==0 && c2->flows.size() != 0)
        return false;
    if(c1->flows.size() !=0 && c2->flows.size() == 0)
        return true;
        // std::cout<<"ERROR prio_cmp no flow"<<std::endl;
    return c1->prio < c2->prio;
}
// 考虑计算时间的优先级比较
bool cmp_prio_c(CoFlow* c1, CoFlow* c2) {
    // 把空coflow始终排到后面
    if(c1->flows.size() ==0 && c2->flows.size() != 0)
        return false;
    if(c1->flows.size() !=0 && c2->flows.size() == 0)
        return true;
        // std::cout<<"ERROR prio_c_cmp no flow"<<std::endl;
    return c1->prio_c < c2->prio_c;
}


string intra_job_sche = "none"; //none, rr, prio
string inter_job_sche = "none"; // none, sjf, locof, prio
vector<vector<CoFlow*> > samesrc_coflows(250, vector<CoFlow*>() );  //保存占用同一src端口的coflow,这里变量名写错了
vector<vector<CoFlow*> > samedst_coflows(250, vector<CoFlow*>() );  //保存占用同一dst端口的coflow
vector<Flow*> send_onport(250, nullptr);
vector<Flow*> recv_onport(250, nullptr);

class JobNode {
    int jobid;             // jobid
    int workers_count;     // number of workers
    int param_size;        // parameter size (in MB)
    int FLOPs;             // the flops of this job (in G)
    int mem_access;        // the memory access of this job (in G), equivalent to flops
    int iter;              // all iter number
    string model_name;     // model name
    string m_type;         // machine type, nvlink or pcie
    simgrid::s4u::Engine* e;

public:
    explicit JobNode(std::vector<std::string> args) {
        xbt_assert(args.size() == 8, "Expecting 8 parameters from the XML deployment file but got %zu", args.size());
        jobid         = std::stoi(args[1]);  // jobid
        workers_count = std::stoi(args[2]);  // number of workers  
        param_size    = std::stoi(args[3]);  // parameter size (in bytes)
        FLOPs         = std::stoi(args[4]);  // the flops of this job (in G)
        mem_access    = std::stoi(args[5]);  // the memory access of this job (in G), equivalent to flops
        iter          = std::stoi(args[6]);  // iter number
        model_name    = args[7];             // model name
    }


    double get_bw(simgrid::s4u::ActorPtr ps, simgrid::s4u::ActorPtr worker) {
        std::vector<simgrid::s4u::Link*> nvlinks = e->get_filtered_links(is_nvlink);
        std::vector<simgrid::s4u::Link*> pcielinks = e->get_filtered_links(is_pcielink);
        std::vector<simgrid::s4u::Link*> ethlinks = e->get_filtered_links(is_ethlink);
        if(in_same_machine(ps, worker)){
            if(nvlinks.size() > 0)
                return nvlinks[0]->get_bandwidth();
            else
                return pcielinks[0]->get_bandwidth();
        }
        else{
            return ethlinks[0]->get_bandwidth();
        }
    }

    void operator()() {
        /* Make a vector of the mailboxes to use */
        e = simgrid::s4u::Engine::get_instance();
        std::vector<simgrid::s4u::ActorPtr> workers;
        std::vector<simgrid::s4u::Host*> hosts;
        std::vector<timeInfo> comm_comp_times;  //communication time and compute time for each worker
        std::vector<simgrid::s4u::Mailbox*> pullcontrolmboxes, pushcontrolmboxes;
        simgrid::s4u::Mailbox* reportbox;       //update status to scheduler
        string my_actor_name = simgrid::s4u::this_actor::get_name();
        string boxname = "REG-" + my_actor_name;
        reportbox = simgrid::s4u::Mailbox::by_name(boxname);
        string my_host_name = simgrid::s4u::this_actor::get_host()->get_name();
        string placement = my_host_name+"_";
        double avg_compute_time = 0.0;
        string ct_list = "";
        // exitnode, 防止出现所有actor结束后scheduler中止抛出异常
        if(jobid == 9999){
            simgrid::s4u::this_actor::sleep_for(10000);
            reportbox->put(new string("FINISH"), 1);
            simgrid::s4u::this_actor::exit();
        }

        // get worker pulling order
        std::vector<simgrid::s4u::ActorPtr> actors = e->get_all_actors();
        int worker_id = 0;
        for(auto a : actors) {
            if(a->get_name() == "Scheduler"){
                reportbox->set_receiver(a);
            }
            std::string name = "Job-" + to_string(jobid) + "-Worker";
            if(a->get_name().find(name) != std::string::npos) {
                workers.push_back(a);
                simgrid::s4u::Host *host = a->get_host();
                hosts.push_back(host);
                int membw = std::stoi(host->get_property("membw"));
                double memacc_duration = (mem_access/1000.0)/membw;
                double compute_time = FLOPs*(1.0e+9) / host->get_speed() + memacc_duration;
                double comm_time = param_size*(1.0e+6) / this->get_bw(simgrid::s4u::Actor::self(), a);
                comm_comp_times.push_back({worker_id, host->get_name(), comm_time, compute_time});
                worker_id++;
            }
        }
        // sort according to comm time and compute time, intra-job scheduling
        if(intra_job_sche == "prio" || intra_job_sche == "prio_c")
            std::sort(comm_comp_times.begin(), comm_comp_times.end(), cmp);
        xbt_assert(workers.size() == workers_count, "Expecting %d workers but got %zu", workers_count, workers.size());

        // 0. Prepareing the pulling, pushing, and flowinfo mailboxs
        // pull order as the sort principle
        for (int i = 0; i < workers_count; i++) {
            boxname = "PULLCTRL-Job-" + to_string(jobid) + "-PS-" + to_string(comm_comp_times[i].workerid); //PULLCTRL-Job-0-PS-0
            pullcontrolmboxes.push_back(simgrid::s4u::Mailbox::by_name(boxname));
            boxname = "PUSHCTRL-Job-" + to_string(jobid) + "-PS-" + to_string(comm_comp_times[i].workerid); //PUSHCTRL-Job-0-PS-0
            pushcontrolmboxes.push_back(simgrid::s4u::Mailbox::by_name(boxname));
            pushcontrolmboxes[i]->set_receiver(simgrid::s4u::Actor::self());

            // std::cout<<"worker:"<< comm_comp_times[i].workerid << "comm_time:"<< comm_comp_times[i].comm_time << "compute:" << comm_comp_times[i].compute_time << std::endl;
            // 拼接placement字符串 gpu0_gpu2, 第一个是ps，后面的是按照拉取顺序的worker所在gpu
            avg_compute_time += comm_comp_times[i].compute_time;
            if(i == (workers_count-1)) {
                placement += (to_string(comm_comp_times[i].workerid) +":"+comm_comp_times[i].host);
                ct_list += to_string(comm_comp_times[i].compute_time);
            }
            else {
                placement += (to_string(comm_comp_times[i].workerid) +":"+comm_comp_times[i].host+"_");
                ct_list += (to_string(comm_comp_times[i].compute_time) + "_");
            }
        }
        // 先向scheduler发送placement信息
        // PULL report message format: Job-0-Node-INFO-[param_size]-[compute_time]-[placement]
        // [placement] = gpu0_gpu2
        // [compute_tiem] = avgt_t1_t2_t3 各个worker计算时间连接起来, 第一项是平均计算时间
        avg_compute_time /= workers_count;
        ct_list = to_string(avg_compute_time) + "_" + ct_list;
        reportbox->put(new string(my_actor_name + "-INFO-"+to_string(param_size)+"-"+ct_list+"-" + placement), 1);

        // Start iteration
        for (int n = 0; n < iter; n++) {
            std::vector<CommPtr> pullcontrol_comms, pushcontrol_comms;
            // 0. 提前设置push异步接收，防止pull，push交错的情况下push通知被pull阻塞
            void** recvdata = new void*[workers_count];
            for (int i = 0; i < workers_count; i++) {
                CommPtr recvcomm = pushcontrolmboxes[i]->get_async(&recvdata[i]);
                pushcontrol_comms.push_back(recvcomm);
            }

            // 1.Parameter Pull
            XBT_INFO("Job-%d-iter-%d-begin", jobid, n);
            // 如果是多个worker同时开始pull，那么发送批量更新消息来更新pull coflow
            // 如果是轮训或者作业内调度，那么一个一个地更新flow
            vector<CommPtr> report_comms;
            CommPtr report_comm = reportbox->put_async(new string(my_actor_name + "-PULL-"+to_string(n)), 1);
            report_comms.push_back(report_comm);
            for (int i = 0; i < workers_count; i++) {
                CommPtr sendcomm;
                if(intra_job_sche == "none") {
                    sendcomm = pullcontrolmboxes[i]->put_async(new string(my_actor_name+"_"+to_string(n)), 1);
                    pullcontrol_comms.push_back(sendcomm);
		    }
            else if(intra_job_sche == "prio" && inter_job_sche == "none") {
		        pullcontrolmboxes[i]->put(new string(my_actor_name+"_"+to_string(n)), 1);
                pullcontrolmboxes[i]->get();
		    } 
		    else {
                    // 按序逐个pull
                    sendcomm = pullcontrolmboxes[i]->put_async(new string(my_actor_name+"_"+to_string(n)), 1);
		    pullcontrol_comms.push_back(sendcomm);
                    // PULL report message format: Job-0-Node-PULL-[iter]-[src gpu]-[dst gpu]-[param_size]
                    // 删掉，用一条消息更新pull coflow，避免消息丢失产生的问题
                    // CommPtr report_comm = reportbox->put_async(new string(my_actor_name + "-PULL-"+to_string(n)+"-"+ my_host_name +"-" + comm_comp_times[i].host+"-"+to_string(param_size) ), 1);
                    // report_comms.push_back(report_comm);
                    // 收到pull结束消息，开始下一次pull, 删掉，因为这样轮询使得scheduler无法独立控制PS
                    // pullcontrolmboxes[i]->get();
                }
                // pullcontrol_comms.push_back(sendcomm);
            }
            // update pull coflow info to scheduler
            // PULL report message format: Job-0-Node-PULL-[iter]
            Comm::wait_all(&pullcontrol_comms);
            Comm::wait_all(&report_comms);

            // 2.Waiting for receiving all parameter from workers async
            // XBT_INFO("Job %d iter %d listening for parameter push", jobid, n);
            Comm::wait_all(&pushcontrol_comms);

            // 3. Aggregation
            simgrid::s4u::this_actor::sleep_for(0.1);
            XBT_INFO("Job-%d-iter-%d-end", jobid, n);
        }
        // grabage collection
        for (int i = 0; i < workers_count; i++) {
            pushcontrolmboxes[i]->set_receiver(nullptr);
        }
    }
};


class PS {
    int jobid;             // jobid
    int ps_id;     // number of workers
    int param_size;        // parameter size (in MB)
    int FLOPs;             // the flops of this job (in G)
    int mem_access;        // the memory access of this job (in G), equivalent to flops
    int iter;              // all iter number
    string model_name;     // model name
    string m_type;         // machine type, nvlink or pcie
    simgrid::s4u::Engine* e;
public:
    explicit PS(std::vector<std::string> args) {
        xbt_assert(args.size() == 8, "Expecting 8 parameters from the XML deployment file but got %zu", args.size());
        jobid         = std::stoi(args[1]);  // jobid
        ps_id = std::stoi(args[2]);  // number of workers  
        param_size    = std::stoi(args[3]);  // parameter size (in bytes)
        FLOPs         = std::stoi(args[4]);  // the flops of this job (in G)
        mem_access    = std::stoi(args[5]);  // the memory access of this job (in G), equivalent to flops
        iter          = std::stoi(args[6]);  // iter number
        model_name    = args[7];             // model name
    }

    void operator()() {
        /* Make a vector of the mailboxes to use */
        e = simgrid::s4u::Engine::get_instance();
        simgrid::s4u::Mailbox *pullmbox, *pushmbox, *pullcontrolmbox, *pushcontrolmbox;
        string this_actor_name = simgrid::s4u::this_actor::get_name(); // Job-0-PS-3
        // PULLCTRL-Job-0-PS-3
        string boxname;
        boxname = "PULLCTRL-" + this_actor_name;
        pullcontrolmbox = simgrid::s4u::Mailbox::by_name(boxname);
        // PUSHCTRL-Job-0-PS-3
        boxname = "PUSHCTRL-" + this_actor_name;
        pushcontrolmbox = simgrid::s4u::Mailbox::by_name(boxname);

        //PULLMJ1W0
        boxname = string("PULLMJ") + to_string(jobid) + string("W") + to_string(ps_id); 
        pullmbox = simgrid::s4u::Mailbox::by_name(boxname);
        boxname = string("PUSHMJ") + to_string(jobid) + string("W") + to_string(ps_id); 
        pushmbox = simgrid::s4u::Mailbox::by_name(boxname);
        pushmbox->set_receiver(simgrid::s4u::Actor::self());

        // Start iteration
        for (int n = 0; n < iter; n++) {
            CommPtr pulling_comms, pushing_comms;
            // 1. start pull
            pullcontrolmbox->get();

            pullmbox->put(new double(1.0), param_size*(1.0e+6));
            // 如果是rr/prio调度，则发送信息告诉jobnodepull流结束了.删除，因为这样的轮询会使得调度器无法独立控制PS
            if(intra_job_sche == "prio" && inter_job_sche == "none") {
                 pullcontrolmbox->put(new string("pull end"), 1);
            }

            // 2. wait for push
            pushmbox->get();
            pushcontrolmbox->put(new string(this_actor_name + "-" + to_string(n) +"-FINISH"), 1 );
        }
        // grabage collection
        pushmbox->set_receiver(nullptr);
    }
};




class Worker {
    simgrid::s4u::Engine* e;
    simgrid::s4u::Mailbox* pullmbox;
    simgrid::s4u::Mailbox* pushmbox;
    int jobid;             // jobid
    int workers_id;        // id of this workers
    int param_size;        // parameter size (in bytes)
    int FLOPs;             // the flops of this job (in G)
    int mem_access;        // the memory access of this job (in G), equivalent to flops
    int iter;              // all iter number
    string model_name;     // model name

public:
    explicit Worker(std::vector<std::string> args)
    {
        //args[1]:jobid, args[2]:workerid
        xbt_assert(args.size() == 8, "Expecting 7 parameter from the XML deployment file but got %zu", args.size());
        jobid         = std::stoi(args[1]);  // jobid
        workers_id    = std::stoi(args[2]);  // woreker id 
        param_size    = std::stoi(args[3]);  // parameter size (in bytes)
        FLOPs         = std::stoi(args[4]);  // the flops of this job (in G)
        mem_access    = std::stoi(args[5]);  // the memory access of this job (in MB)
        iter          = std::stoi(args[6]);  // iter number
        model_name    = args[7];             // model name
        std::string pullmboxName = std::string("PULLMJ") + args[1] + string("W") + args[2];
        pullmbox                 = Mailbox::by_name(pullmboxName);
        std::string pushmboxName = std::string("PUSHMJ") + args[1] + string("W") + args[2];
        pushmbox                 = Mailbox::by_name(pushmboxName);
        e = simgrid::s4u::Engine::get_instance();

    }
    void operator()()
    {
        Host* this_host = simgrid::s4u::this_actor::get_host();
        int membw = std::stoi(this_host->get_property("membw"));
        double memacc_duration = (mem_access/1000.0)/membw;

        simgrid::s4u::Mailbox* reportbox;       //update status to scheduler
        string boxname = "REG-" + simgrid::s4u::this_actor::get_name();
        reportbox = simgrid::s4u::Mailbox::by_name(boxname);
        string my_actor_name = simgrid::s4u::this_actor::get_name();
        string my_host_name = simgrid::s4u::this_actor::get_host()->get_name();
        vector<simgrid::s4u::ActorPtr> all_actors = e->get_all_actors();
        simgrid::s4u::ActorPtr ps_actor;
        for(int i = 0; i < all_actors.size(); i++) {
            if(all_actors[i]->get_name() == "Scheduler"){
                reportbox->set_receiver(all_actors[i]);
            }
            if(all_actors[i]->get_name() == "Job-"+to_string(jobid)+"-Node") {
                ps_actor = all_actors[i];
                break;
            }
        }
        string ps_host_name = ps_actor->get_host()->get_name();

        for(int n = 0; n < iter; n++) {
            // 1.pull, waiting for parameter
            vector<CommPtr> report_comms;
            pullmbox->get();
            XBT_INFO("Job-%d-worker-%d-iter-%d-pull", jobid, workers_id, n);
            // 2.computing format: Job-0-Worker-0-COMPUTE-[iter]-[src gpu]-[dst gpu]-[param_size]
            CommPtr report_com = reportbox->put_async(new string(my_actor_name + "-COMPUTE-" + to_string(n)+"-"+ps_host_name+"-"+my_host_name+"-"+to_string(param_size)), 1);
            report_comms.push_back(report_com);
            simgrid::s4u::this_actor::execute(FLOPs*(1.0e+9));
            simgrid::s4u::this_actor::sleep_for(memacc_duration);
            XBT_INFO("Job-%d-worker-%d-iter-%d-compute", jobid, workers_id, n);
            // 3.push
            // report message format: Job-0-Worker-0-PUSH-[src gpu]-[dst gpu]-[param_size]
            CommPtr report_push = reportbox->put_async(new string(my_actor_name + "-PUSH-"+ to_string(n)+"-"+my_host_name+"-"+ps_host_name+"-"+to_string(param_size)), 1);
            report_comms.push_back(report_push);
            pushmbox->put(new string("push"), param_size*(1.0e+6));
            // 4. AGG
            XBT_INFO("Job-%d-worker-%d-iter-%d-push", jobid, workers_id, n);
            // report message format: Job-0-Worker-0-AGG-[iter]-[src gpu]-[dst gpu]-[param_size]
            CommPtr report_agg = reportbox->put_async(new string(my_actor_name + "-AGG-"+ to_string(n)+"-"+my_host_name+"-"+ps_host_name+"-"+to_string(param_size)), 1);
            report_comms.push_back(report_agg);
            Comm::wait_all(&report_comms);
        }
        // simgrid::s4u::this_actor::sleep_for(100);
        
    }
};


class Scheduler {
    simgrid::s4u::Engine* e;            //simulation engine
    vector<Mailbox*> ps_reportboxs;
    vector<Mailbox*> worker_reportboxs;
    vector<Mailbox*> reportboxs;
    std::vector<simgrid::s4u::ActorPtr> actor_list;  // actor list, never change
    std::vector<Host*> host_list;                    // host list, never change
    std::vector<Link*> link_list;                    // machine link, like l-sw0-*
    std::vector<JobInfo> jobs_info;
    std::vector<CoFlow*> coflow_list;                //越靠前优先级越高
    int jobnumber;



public:
    explicit Scheduler(std::vector<std::string> args) {
        jobnumber = stoi(args[1]);
        e = simgrid::s4u::Engine::get_instance();
        simgrid::s4u::Actor::self()->daemonize();
    }

    int find_flow(vector<Flow*> flist, string flow_id) {
        for(int i = 0; i < flist.size(); i++){
            if(flist[i]->flow_id == flow_id)
                return i;
        }
        return -1;
    }

    int find_coflow(vector<CoFlow*> cflist, string coflow_id) {
        for(int i = 0; i < cflist.size(); i++){
            if(cflist[i]->coflow_id == coflow_id)
                return i;
        }
        return -1;
    }

    // report message format: Job-0-Worker-0-AGG-[iter]-[src gpu]-[dst gpu]-[param_size]
    // report message format: Job-0-Worker-0-PUSH-[iter]-[src gpu]-[dst gpu]-[param_size]
    // report message format: Job-0-Worker-0-COMPUTE-[iter]-[src gpu]-[dst gpu]-[param_size]
    // PULL report message format: Job-0-Node-PULL-[iter]-[src gpu]-[dst gpu]-[param_size] 逐个,弃用
    // PULL report message format: Job-0-Node-PULL-[iter] 批量
    // 使用接收到的消息更新流信息
    int update_coflow(vector<string>& recv_info, bool debug) {
        for(int i = 0; i < recv_info.size(); i++) {
            std::regex ws_re("-");
            std::vector<string> str_split(std::sregex_token_iterator(recv_info[i].begin(), recv_info[i].end(),ws_re, -1 ),std::sregex_token_iterator());
            int jobid = stoi(str_split[1]);
            int iter, workerid, param_size;
            string action;
            string src, dst;
            if(str_split[2] == "Node") {
                action = str_split[3];
                iter = stoi(str_split[4]);
                // if(intra_job_sche != "none") {
                //     src = str_split[5];
                //     dst = str_split[6];
                //     param_size = stoi(str_split[7]);
                // }
            }
            else {
                workerid = stoi(str_split[3]);
                action = str_split[4];
                iter = stoi(str_split[5]);
                src = str_split[6];
                dst = str_split[7];
                param_size = stoi(str_split[8]);
            }
            // 根据消息类型更新pull coflow信息
            if(action == "PULL"){
                // if(intra_job_sche == "none") {
                    // 批量增加pull coflow信息
                JobInfo thisjob = jobs_info[jobid];
                CoFlow *thiscoflow = new CoFlow(to_string(jobid)+"-"+to_string(iter)+"-PULL");
                thiscoflow->flow_num = thisjob.placement.size()-1;
                thiscoflow->flow_cnt = thisjob.placement.size()-1;
                for(int j = 1; j < thisjob.placement.size(); j++) {
                    src = thisjob.placement[0];
                    std::regex ws_re(":");
                    std::vector<string> id2place(std::sregex_token_iterator(thisjob.placement[j].begin(), thisjob.placement[j].end(),ws_re , -1 ), std::sregex_token_iterator());
                    dst = id2place[1];
                    Flow *thisflow = new Flow(thiscoflow->coflow_id + "-"+ src + "-" + dst);
                    thisflow->src_gpu = src;
                    thisflow->dst_gpu = dst;
                    thisflow->src_port = get_gpuid(src)/8;
                    thisflow->dst_port = get_gpuid(dst)/8;
                    //  修复一条coflow中有多个flow src/dst相同的情况下，只保存一个coflow对象导致的coflow找不到问题
                    // if(thisflow->src_port != thisflow->dst_port &&
                    //     this->find_coflow(samesrc_coflows[thisflow->src_port], thiscoflow->coflow_id) == -1)
                    if(thisflow->src_port != thisflow->dst_port)
                        samesrc_coflows[thisflow->src_port].push_back(thiscoflow);
                    // if(thisflow->src_port != thisflow->dst_port &&
                    // this->find_coflow(samedst_coflows[thisflow->dst_port], thiscoflow->coflow_id) == -1)
                    if(thisflow->src_port != thisflow->dst_port)
                        samedst_coflows[thisflow->dst_port].push_back(thiscoflow);
                    thisflow->flow_size = thisjob.param_size;
                    thiscoflow->avg_ct = thisjob.compute_time[0];
                    thisflow->compute_time = thisjob.compute_time[j];
                    thisflow->flow_type = "PULL";
                    thisflow->status = "init";
                    thisflow->src_host = e->host_by_name_or_null(src);
                    thisflow->dst_host = Host::by_name_or_null(dst);
                    thisflow->src_actor = nullptr;
                    thisflow->dst_actor = nullptr;
                    string worker_id = "Job-"+to_string(jobid)+"-Worker-" + id2place[0];
                    string ps_id = "Job-"+to_string(jobid)+"-PS-" + id2place[0];

                    for(int j = 0; j < thisflow->src_host->get_all_actors().size(); j++) {
                        if(thisflow->src_host->get_all_actors()[j]->get_name() == ps_id) {
                            thisflow->src_actor = thisflow->src_host->get_all_actors()[j];
                            break;
                        }
                    }
                    for(auto actor: thisflow->dst_host->get_all_actors()) {
                        if(actor->get_name() == worker_id) {
                            thisflow->dst_actor = actor;
                            break;
                        }
                    }
                    if(thisflow->src_host == nullptr) {
                        std::cout <<"=====error src actor:"<<ps_id << " not found"<< std::endl;
                    }
                    if(thisflow->dst_host == nullptr) {
                        std::cout <<"=====error dst actor:"<<worker_id << " not found"<< std::endl;
                    }
                    thiscoflow->flows.push_back(thisflow);

                    if(debug)
                        std::cout <<"=====add flow:"<<thisflow->flow_id << " src actor: "<< thisflow->src_actor->get_name()
                    << " dst actor"<< thisflow->dst_actor->get_name() << std::endl;                    
                }
                coflow_list.push_back(thiscoflow);
                if(debug)
                    std::cout <<"=====add coflow:"<<thiscoflow->coflow_id << "size:"<< thiscoflow->flows.size() << " success in pullstate"<< std::endl;
            }
            else if(action == "COMPUTE") {
                // 删除pull flow信息
                // report message format: Job-0-Worker-0-COMPUTE-[iter]-[src gpu]-[dst gpu]-[param_size]
                string coflow_id = to_string(jobid)+"-"+to_string(iter)+"-PULL";
                int coflow_pos = this->find_coflow(coflow_list, coflow_id);
                if(coflow_pos == -1){
                    std::cout <<"=====ERROR COM: coflow:" << coflow_id << "not exist in compute state!====="<< std::endl;
                    return -1;
                }
                CoFlow *thiscoflow = coflow_list[coflow_pos];
                string flow_id = coflow_id+"-"+src+"-"+dst;

                int flow_pos = this->find_flow(thiscoflow->flows, flow_id);
                if(flow_pos == -1) {
                    std::cout <<"=====ERROR COM: flow:"<<flow_id<< " not exist in compute state!====="<< std::endl;
                    return -1;
                }

                // 更新源 目的端口flow列表
                // pull coflow即将结束了，删掉src
                Flow *thisflow = thiscoflow->flows[flow_pos];
                if(thisflow->src_port != thisflow->dst_port) {
                    if(thiscoflow->flows.size() == 1) {
                        int src_pos = this->find_coflow(samesrc_coflows[thisflow->src_port], coflow_id);
                        if(src_pos == -1) {
                            std::cout <<"=====ERROR COM: coflow:"<<coflow_id<< " not exist in samesrc_coflows srcport:"<< thisflow->src_port << std::endl;
                            return -1;
                        }
                        samesrc_coflows[thisflow->src_port].erase(samesrc_coflows[thisflow->src_port].begin() + src_pos);
                    }
                    int dst_pos = this->find_coflow(samedst_coflows[thisflow->dst_port], coflow_id);
                    if(dst_pos == -1) {
                        std::cout <<"=====ERROR COM: coflow:"<<coflow_id<< " not exist in samedst_coflows dstport:"<< thisflow->dst_port << "dst host: "<< thisflow->dst_host->get_name() << std::endl;
                        return -1;
                    }
                    samedst_coflows[thisflow->dst_port].erase(samedst_coflows[thisflow->dst_port].begin() + dst_pos);
                }

                // 删除端口占用
                // if(thisflow->src_port != thisflow->dst_port ){
                //     if(send_onport[thisflow->src_port] !=nullptr || recv_onport[thisflow->dst_port] != nullptr)  {
                //         std::cout << "yes"<<std::endl;
                //         if(send_onport[thisflow->src_port] !=nullptr){
                //             std::cout <<"sendport: "<<  thisflow->src_port << " send not null: "<< send_onport[thisflow->src_port]->flow_id <<std::endl;
                //         }
                //         if(recv_onport[thisflow->dst_port] != nullptr) {
                //             std::cout << "recvport: "<<  thisflow->dst_port <<" recv not null: " << recv_onport[thisflow->dst_port]->flow_id <<std::endl;
                //         }
                //     }
                // }
                // if(thisflow->src_port != thisflow->dst_port) {
                //     if(send_onport[thisflow->src_port] != thisflow) {
                //         std::cout<< "com different: "<< thisflow->flow_id << " sendflow: "<< send_onport[thisflow->src_port]->flow_id<< std::endl;
                //     }
                //     if(recv_onport[thisflow->dst_port]!= thisflow) {
                //         std::cout<< "com different: "<< thisflow->flow_id << " recvflow: "<< recv_onport[thisflow->dst_port]->flow_id<< std::endl;
                //     }
                // }


                if(thisflow->src_port != thisflow->dst_port && 
                send_onport[thisflow->src_port] == thisflow && recv_onport[thisflow->dst_port] == thisflow) {
                    // if(send_onport[thisflow->src_port] != thisflow || recv_onport[thisflow->dst_port] != thisflow) {
                    //     std::cout << "sendport: " << thisflow->src_port << " send: " <<send_onport[thisflow->src_port]->flow_id <<
                    //     " recvport: "<< thisflow->dst_port<<  " recv: " << recv_onport[thisflow->dst_port]->flow_id << std::endl;
                    // }

                    
                    // std::cout << "pull flow finish: " << thisflow->flow_id << " "
                    // << thisflow->src_port << " to " << thisflow->dst_port
                    // << " inportid: "
                    // << send_onport[thisflow->src_port]->flow_id << " "
                    // << send_onport[thisflow->src_port]->src_port << " to " << send_onport[thisflow->src_port]->dst_port
                    // << std::endl;
                    send_onport[thisflow->src_port] = nullptr;
                    recv_onport[thisflow->dst_port] = nullptr;
                    if(debug)
                        std::cout<< "deleteoccupy: " << thisflow->flow_id<<std::endl;
                }

                thisflow->status = "done";
                // 删除flow
                if(inter_job_sche == "none") {
                    thiscoflow->flows.erase(thiscoflow->flows.begin() + flow_pos);
                    thiscoflow->flow_cnt-=1;
                    if(thiscoflow->flow_cnt == 0) {
                        coflow_list.erase(coflow_list.begin()+coflow_pos);
                        if(debug)
                            std::cout <<"=====delete coflow:"<<coflow_id<< " success  in compute state!"<< std::endl;
                    }
                }


                if(debug)
                    std::cout <<"=====comdeleteflow:"<<thisflow->flow_id<< " success  in compute state!"<< std::endl;
                // 流全部结束，删除coflow

            }
            else if (action == "PUSH"){
                // report message format: Job-0-Worker-0-PUSH-[iter]-[src gpu]-[dst gpu]-[param_size]
                string coflow_id = to_string(jobid)+"-"+to_string(iter)+"-PUSH";
                int coflow_pos = this->find_coflow(coflow_list, coflow_id);
                CoFlow *thiscoflow;
                JobInfo thisjob = jobs_info[jobid];
                
                if(coflow_pos == -1) {
                    // 说明这是这个coflow的第一个flow，需要新建coflow
                    thiscoflow = new CoFlow(coflow_id);
                    thiscoflow->flow_num = thisjob.placement.size()-1;
                    thiscoflow->flow_cnt = thisjob.placement.size()-1;
                    coflow_list.push_back(thiscoflow);
                    if(debug)
                        std::cout <<"=====add coflow:"<<thiscoflow->coflow_id <<" success in pushstate"<< std::endl;
                }
                else {
                    thiscoflow = coflow_list[coflow_pos];
                }
                // 构造push flow
                Flow *thisflow = new Flow(thiscoflow->coflow_id + "-"+ src + "-" + dst);
                thisflow->src_gpu = src;
                thisflow->dst_gpu = dst;
                thisflow->src_port = get_gpuid(src)/8;
                thisflow->dst_port = get_gpuid(dst)/8;
                //  修复一条coflow中有多个flow src/dst相同的情况下，只保存一个coflow对象导致的coflow找不到问题
                // if(thisflow->src_port != thisflow->dst_port &&
                // this->find_coflow(samesrc_coflows[thisflow->src_port], thiscoflow->coflow_id) == -1)
                if(thisflow->src_port != thisflow->dst_port)
                    samesrc_coflows[thisflow->src_port].push_back(thiscoflow);

                // if(thisflow->src_port != thisflow->dst_port &&
                // this->find_coflow(samedst_coflows[thisflow->dst_port], thiscoflow->coflow_id) == -1)
                if(thisflow->src_port != thisflow->dst_port)
                    samedst_coflows[thisflow->dst_port].push_back(thiscoflow);

                thisflow->flow_size = thisjob.param_size;
                thisflow->compute_time = thisjob.compute_time[workerid+1];  // push flow没有计算时间
                thiscoflow->avg_ct = thisjob.compute_time[0];
                thisflow->flow_type = "PUSH";
                thisflow->status = "init";
                thisflow->src_host = Host::by_name(src);
                thisflow->dst_host = Host::by_name(dst);

                thisflow->src_actor = nullptr;
                thisflow->dst_actor = nullptr;
                string worker_id = "Job-"+to_string(jobid)+"-Worker-" + to_string(workerid);
                string ps_id = "Job-"+to_string(jobid)+"-PS-" + to_string(workerid);
                for(auto actor: thisflow->src_host->get_all_actors()) {
                    if(actor->get_name() == worker_id) {
                        thisflow->src_actor = actor;
                        break;
                    }
                }
                for(auto actor: thisflow->dst_host->get_all_actors()) {
                    if(actor->get_name() == ps_id) {
                        thisflow->dst_actor = actor;
                        break;
                    }
                }
                if(thisflow->src_host == nullptr) {
                    if(debug)
                        std::cout <<"=====error src actor:"<<worker_id << " not found"<< std::endl;
                }
                if(thisflow->dst_host == nullptr) {
                    if(debug)
                        std::cout <<"=====error dst actor:"<<ps_id << " not found"<< std::endl;
                }
                thiscoflow->flows.push_back(thisflow);
                if(debug)
                    std::cout <<"=====add flow:"<<thisflow->flow_id<< " success in push state!" <<i<<":"<<recv_info.size() << std::endl;
            }
            else if(action == "AGG"){
                // 删除push流
                string coflow_id = to_string(jobid)+"-"+to_string(iter)+"-PUSH";
                // auto it = std::find_if(coflow_list.begin(), coflow_list.end(), CoFlow(coflow_id));
                int coflow_pos = this->find_coflow(coflow_list, coflow_id);
                if(coflow_pos == -1) {
                    std::cout <<"=====ERROR: coflow:" << coflow_id << " not exist in agg state!====="<< std::endl;
                    return -1;
                }
                CoFlow *thiscoflow = coflow_list[coflow_pos];
                string flow_id = coflow_id+"-"+src+"-"+dst;
                int flow_pos = this->find_flow(thiscoflow->flows, flow_id);
                if(flow_pos == -1) {
                    std::cout <<"=====ERROR AGG: flow:" << flow_id << " not exist in agg state!====="<< std::endl;
                    return -1;
                }

                // 更新源 目的端口flow列表
                // push coflow即将结束了，删掉dst
                Flow *thisflow = thiscoflow->flows[flow_pos];
                if(thisflow->src_port != thisflow->dst_port) {
                    if(thiscoflow->flows.size() == 1) {
                        int dst_pos = this->find_coflow(samedst_coflows[thisflow->dst_port], coflow_id);
                        if(dst_pos == -1) {
                            std::cout <<"=====ERROR AGG: coflow:"<<coflow_id<< " not exist in samedst_coflows dstport:"<< thisflow->dst_port << std::endl;
                            return -1;
                        }
                        samedst_coflows[thisflow->dst_port].erase(samedst_coflows[thisflow->dst_port].begin() + dst_pos);
                    }
                    int src_pos = this->find_coflow(samesrc_coflows[thisflow->src_port], coflow_id);
                    if(src_pos == -1) {
                        std::cout <<"=====ERROR AGG: coflow:"<<coflow_id<< " not exist in samesrc_coflows srcport:"<< thisflow->src_port << std::endl;
                        return -1;
                    }
                    samesrc_coflows[thisflow->src_port].erase(samesrc_coflows[thisflow->src_port].begin() + src_pos);
                }

                // 删除端口占用
                thisflow->status = "done";
                if(thisflow->src_port != thisflow->dst_port &&
                send_onport[thisflow->src_port] == thisflow && recv_onport[thisflow->dst_port] == thisflow) {
                        send_onport[thisflow->src_port] = nullptr;
                        recv_onport[thisflow->dst_port] = nullptr;
                }
                // 删除flow,其他调度策略的flow在schedule阶段删除
                if(inter_job_sche == "none") {
                    thiscoflow->flows.erase(thiscoflow->flows.begin()+flow_pos);
                    thiscoflow->flow_cnt-=1;
                    if(thiscoflow->flow_cnt == 0) {
                        coflow_list.erase(coflow_list.begin()+coflow_pos);
                        if(debug)
                            std::cout <<"=====delete coflow:"<<coflow_id<< " success in agg state!"<< std::endl;
                }


                if(debug)
                    std::cout <<"=====aggdeleteflow:"<<flow_id<< " success in agg state!"<< std::endl;
                }
            }
            else {
                std::cout<<"=====Undefined action!====="<< std::endl;
                return -1;
            }
        }
        if(inter_job_sche == "locof"){
            this->update_locof(debug);
        }
        else if(inter_job_sche == "prio") {
            this->update_prio(debug);
        }
        else if(inter_job_sche == "prio_c") {
            this->update_prio_c(debug);
        }
        return 0;
    }

    // 更新每个coflow的co_port 争用系数，在update_coflow之后执行
    int update_locof(bool debug) {
        string debuginfo = "";
        for (int i = 0; i < coflow_list.size(); i++) {
            CoFlow *this_coflow = coflow_list[i];
            if(this_coflow->flows.size() == 0)
                continue;
            vector<int> src_port_list, dst_port_list;
            std::set<std::string> content_set;  //保存coflowid
            // 分别对pull push coflow统计src dst
            if(this_coflow->coflow_id.find("PULL") != string::npos) {
                src_port_list.push_back(this_coflow->flows[0]->src_port);
                for(int j = 0; j < this_coflow->flows.size(); j++) {
                    if(this_coflow->flows[0]->src_port != this_coflow->flows[j]->dst_port)
                        dst_port_list.push_back(this_coflow->flows[j]->dst_port);
                }
            }
            else {
                dst_port_list.push_back(this_coflow->flows[0]->dst_port);
                for(int j = 0; j < this_coflow->flows.size(); j++) {
                    if(this_coflow->flows[0]->dst_port != this_coflow->flows[j]->src_port)
                        src_port_list.push_back(this_coflow->flows[j]->src_port);
                }
            }

            for(int j = 0; j < src_port_list.size(); j++) {
                // 将占用同一个src port的coflow加入到集合中
                for(auto cf: samesrc_coflows[src_port_list[j]]) {
                    if(cf->coflow_id != this_coflow->coflow_id) {
                        content_set.insert(cf->coflow_id);
                    }
                }
            }
            for(int j = 0; j < dst_port_list.size(); j++) {
                // 将占用同一个src port的coflow加入到集合中
                for(auto cf: samedst_coflows[dst_port_list[j]]) {
                    if(cf->coflow_id != this_coflow->coflow_id)
                        content_set.insert(cf->coflow_id);
                }
            }
            // 更新优先级
            this_coflow->locof = content_set.size();
            for(int j = 0; j < this_coflow->flows.size(); j++) {
                this_coflow->flows[j]->locof = this_coflow->locof;
            }

            if(debug && content_set.size() > 0) {
                debuginfo += (this_coflow->coflow_id + " " + to_string(this_coflow->locof))+"; ";
                // std::cout<<"coflow: "<<this_coflow->coflow_id << " locofcontent_set: ";
                // for(auto id: content_set) {
                //     std::cout<< id;
                // }
                // std::cout<<" "<<std::endl;
            }
        }
        // if(debug && debuginfo!="")
        //     std::cout <<"===> locof_info: " << debuginfo <<std::endl;
        return 0;
    }

    // 更新每个coflow的prio，在update_coflow之后执行
    int update_prio(bool debug) {
        string debuginfo = "";
        for (int i = 0; i < coflow_list.size(); i++) {
            CoFlow *this_coflow = coflow_list[i];
            if(this_coflow->flows.size() == 0)
                continue;
            vector<int> src_port_list, dst_port_list;
            std::set<std::pair<std::string, int>> content_set;  //保存coflowid, param_size
            // 分别对pull push coflow统计src dst
            if(this_coflow->coflow_id.find("PULL") != string::npos) {
                src_port_list.push_back(this_coflow->flows[0]->src_port);
                for(int j = 0; j < this_coflow->flows.size(); j++) {
                    if(this_coflow->flows[0]->src_port != this_coflow->flows[j]->dst_port)
                        dst_port_list.push_back(this_coflow->flows[j]->dst_port);
                }
            }
            else {
                dst_port_list.push_back(this_coflow->flows[0]->dst_port);
                for(int j = 0; j < this_coflow->flows.size(); j++) {
                    if(this_coflow->flows[0]->dst_port != this_coflow->flows[j]->src_port)
                        src_port_list.push_back(this_coflow->flows[j]->src_port);
                }
            }

            for(int j = 0; j < src_port_list.size(); j++) {
                // 将占用同一个src port的coflow加入到集合中
                for(auto cf: samesrc_coflows[src_port_list[j]]) {
                    if(cf->coflow_id != this_coflow->coflow_id)
                        content_set.insert(std::make_pair(cf->coflow_id, cf->flows[0]->flow_size) );
                }
            }
            for(int j = 0; j < dst_port_list.size(); j++) {
                // 将占用同一个src port的coflow加入到集合中
                for(auto cf: samedst_coflows[dst_port_list[j]]) {
                    if(cf->coflow_id != this_coflow->coflow_id)
                        content_set.insert(std::make_pair(cf->coflow_id, cf->flows[0]->flow_size) );
                }
            }
            double prio = 0.0;
            for (auto kv: content_set) {
                prio += ( double(this_coflow->flows[0]->flow_size) / kv.second);
            }
            // 更新优先级
            this_coflow->prio = prio;
            for(int j = 0; j < this_coflow->flows.size(); j++) {
                this_coflow->flows[j]->prio = prio;
            }

            if(debug && content_set.size() > 0) {
                debuginfo += (this_coflow->coflow_id + " " + to_string(this_coflow->prio)) +"; ";
                std::cout<<"coflow: "<<this_coflow->coflow_id << " prio : " << prio;
                // for(auto id: content_set) {
                //     std::cout<< id.first;
                // }
                std::cout<<" "<<std::endl;
            }
        }
        if(debug && debuginfo!="")
            std::cout<<"===> prio_info: " << debuginfo <<std::endl;
        return 0;
    }

    // 更新每个coflow的prio_c，在update_coflow之后执行
    int update_prio_c(bool debug) {
        string debuginfo = "";
        for (int i = 0; i < coflow_list.size(); i++) {
            CoFlow *this_coflow = coflow_list[i];
            if(this_coflow->flows.size() == 0)
                continue;
            vector<int> src_port_list, dst_port_list;
            std::set<std::pair<double, int>> content_set;  //保存coflowid, param_size
            // 分别对pull push coflow统计src dst
            if(this_coflow->coflow_id.find("PULL") != string::npos) {
                src_port_list.push_back(this_coflow->flows[0]->src_port);
                for(int j = 0; j < this_coflow->flows.size(); j++) {
                    if(this_coflow->flows[0]->src_port != this_coflow->flows[j]->dst_port)
                        dst_port_list.push_back(this_coflow->flows[j]->dst_port);
                }
            }
            else {
                dst_port_list.push_back(this_coflow->flows[0]->dst_port);
                for(int j = 0; j < this_coflow->flows.size(); j++) {
                    if(this_coflow->flows[0]->dst_port != this_coflow->flows[j]->src_port)
                        src_port_list.push_back(this_coflow->flows[j]->src_port);
                }
            }

            for(int j = 0; j < src_port_list.size(); j++) {
                // 将占用同一个src port的coflow加入到集合中
                for(auto cf: samesrc_coflows[src_port_list[j]]) {
                    if(cf->coflow_id != this_coflow->coflow_id)
                        content_set.insert(std::make_pair(cf->avg_ct, cf->flows[0]->flow_size) );
                }
            }
            for(int j = 0; j < dst_port_list.size(); j++) {
                // 将占用同一个src port的coflow加入到集合中
                for(auto cf: samedst_coflows[dst_port_list[j]]) {
                    if(cf->coflow_id != this_coflow->coflow_id)
                        content_set.insert(std::make_pair(cf->avg_ct, cf->flows[0]->flow_size) );
                }
            }
            double prio_c = 0.0;
            for (auto kv: content_set) {
                prio_c += ( double(this_coflow->flows[0]->flow_size) / kv.second);
                // prio_c += ( kv.first / this_coflow->avg_ct);
            }
            // 更新优先级
            this_coflow->prio_c = prio_c;
            for(int j = 0; j < this_coflow->flows.size(); j++) {
                this_coflow->flows[j]->prio_c = prio_c;
            }

            if(debug && content_set.size() > 0) {
                debuginfo += (this_coflow->coflow_id + " " + to_string(this_coflow->prio_c)) +"; ";
                std::cout<<"coflow: "<<this_coflow->coflow_id << " prio_c: " << prio_c << " compu time: "<< this_coflow->avg_ct;
                // for(auto id: content_set) {
                //     std::cout<< id.first;
                // }
                std::cout<<" "<<std::endl;
            }
        }
        if(debug && debuginfo!="")
            std::cout<<"===> prio_c_info: " << debuginfo <<std::endl;
        return 0;
    }

    int can_alloc(bool debug) {
        int cnt = 0, init = 0, susp = 0, act = 0, done = 0, realact = 0;
        for(int i = 0; i < coflow_list.size(); i++) {
            CoFlow* c = coflow_list[i];
            for(int j = 0; j < c->flows.size(); j++) {
                Flow* f = c->flows[j];
                int src = f->src_port;
                int dst = f->dst_port;
                if(src == dst)
                    continue;
                if(send_onport[src] == nullptr && recv_onport[dst] == nullptr)
                    cnt++;
                else {
                    if(f->status == "init")
                        init++;
                    else if(f->status == "suspend")
                        susp++;
                    else if(f->status == "active") {
                        act++;
                        if(debug) {
                            std::cout << f->flow_id <<" "<<f->src_port <<" to " << f->dst_port << " " <<
                            " srcactor: "<< f->src_actor->get_name()<<":" << f->src_actor->is_suspended() <<
                            " dstactor: "<< f->dst_actor->get_name()<<":" << f->dst_actor->is_suspended() <<std::endl;
                        }
                    }
                    else
                        done++;
                }
            }
        }
        // if(debug) {
        //     std::cout << "statistic: " <<init << " "<<susp<<" "<<act<<" "<<realact<<" "<<done<<std::endl;
        // }
        return cnt;
    }

    // 有空闲就分配,fifo
    int alloc_bw(Flow* flow, bool debug) {
        int src_port = flow->src_port, dst_port = flow->dst_port;
        if(src_port == dst_port)
            return 0;
        if(send_onport[src_port]==nullptr && recv_onport[dst_port]==nullptr) {
            send_onport[src_port] = flow;
            recv_onport[dst_port] = flow;
            return 0;
        }
        return -1;
    }
     
    // 抢占式带宽分配, work-conservation
    int preem_alloc_bw(Flow* flow, bool debug) {
        int src_port = flow->src_port, dst_port = flow->dst_port;
        double my_prio = 0.0, src_prio = 999999.0, dst_prio = 999999.0;
        if(src_port == dst_port)
            return 0;
        if(inter_job_sche == "sjf") {
            my_prio = flow->flow_size;
            if(send_onport[src_port]!=nullptr)
                src_prio = send_onport[src_port]->flow_size;
            if(recv_onport[dst_port]!=nullptr)
                dst_prio = recv_onport[dst_port]->flow_size;
        }
        else if(inter_job_sche == "locof") {
            my_prio = flow->locof;
            if(send_onport[src_port]!=nullptr)
                src_prio = send_onport[src_port]->locof;
            if(recv_onport[dst_port]!=nullptr)
                dst_prio = recv_onport[dst_port]->locof;
        }
        else if(inter_job_sche == "prio") {
            my_prio = flow->prio;
            if(send_onport[src_port]!=nullptr)
                src_prio = send_onport[src_port]->prio;
            if(recv_onport[dst_port]!=nullptr)
                dst_prio = recv_onport[dst_port]->prio;
        }
        else if(inter_job_sche == "prio_c"){
            my_prio = flow->prio_c;
            if(send_onport[src_port]!=nullptr)
                src_prio = send_onport[src_port]->prio_c;
            if(recv_onport[dst_port]!=nullptr)
                dst_prio = recv_onport[dst_port]->prio_c;
        }
        // 调度

        if(send_onport[src_port]==nullptr && recv_onport[dst_port]==nullptr) {
            send_onport[src_port] = flow;
            recv_onport[dst_port] = flow;
            return 0;
        }
        else if(send_onport[src_port]==nullptr && recv_onport[dst_port]!=nullptr) {
            if(my_prio < dst_prio) {
                // pull flow不能被push flow抢占
                if(inter_job_sche == "prio_c" && 
                recv_onport[dst_port]->flow_type == "PULL" && flow->flow_type == "PUSH" )
                    return -1;
                send_onport[src_port] = flow;
                // 清除低优先级flow的端口占用
                Flow* samedst_flow = recv_onport[dst_port];
                int pre_src = recv_onport[dst_port]->src_port;  // 要被抢占的低优先级流的srcport
                send_onport[pre_src] = nullptr;
                recv_onport[dst_port]->src_actor->suspend();
                recv_onport[dst_port]->status = "suspend";
                recv_onport[dst_port] = flow;
                if(debug){
                    std::cout<<"dst preemitive I: "<< flow->flow_id<< " prio: "<< my_prio << " size: "<<flow->flow_size<<
                    " samedst: "<< samedst_flow->flow_id << " prio: "<< dst_prio<<" size: "<<samedst_flow->flow_size <<std::endl;
                }
                return 0;
            }
        } 
        else if(send_onport[src_port]!=nullptr && recv_onport[dst_port]==nullptr) {
            if(my_prio < src_prio) {
                // pull flow不能被push flow抢占
                if(inter_job_sche == "prio_c" && 
                send_onport[src_port]->flow_type == "PULL" && flow->flow_type == "PUSH" )
                    return -1;
                recv_onport[dst_port] = flow;
                Flow* samesrc_flow = send_onport[src_port];
                int pre_dst = send_onport[src_port]->dst_port;
                recv_onport[pre_dst] = nullptr;
                send_onport[src_port]->src_actor->suspend();
                send_onport[src_port]->status = "suspend";
                send_onport[src_port] = flow;
                if(debug) {
                    std::cout<<"src preemitive I: "<< flow->flow_id<< " prio: "<< my_prio << " size: "<<flow->flow_size<<
                    " samesrc: "<< samesrc_flow->flow_id << " prio: "<< src_prio<< " size: "<<samesrc_flow->flow_size <<std::endl;
                }
                return 0;
            }
        }
        else {
            // 抢占
            if(my_prio < src_prio && my_prio < dst_prio) {
                // pull flow不能被push flow抢占
                if(inter_job_sche == "prio_c" && flow->flow_type == "PUSH" && 
                (send_onport[src_port]->flow_type == "PULL" || recv_onport[dst_port]->flow_type =="PUSH" ) )
                    return -1;
                // 分两种情况，被抢占的是同一条流，还是两条流
                // 如果是同一条流
                if(recv_onport[dst_port] == send_onport[src_port]) {
                    send_onport[src_port]->src_actor->suspend();
                    send_onport[src_port]->status = "suspend";
                }
                else {
                    // 对于src相同的flow，清除dst占用并暂停
                    int pre_dst = send_onport[src_port]->dst_port;
                    send_onport[src_port]->src_actor->suspend();
                    send_onport[src_port]->status = "suspend";
                    recv_onport[pre_dst] = nullptr;
                    // 对于dst相同的flow，清除src并暂停
                    int pre_src = recv_onport[dst_port]->src_port;
                    recv_onport[dst_port]->src_actor->suspend();
                    recv_onport[dst_port]->status = "suspend";
                    send_onport[pre_src] = nullptr;
                }
                send_onport[src_port] = flow;
                recv_onport[dst_port] = flow;
                return 0;
            }
        }
        return -1;
    }

    // prio下 抢占式带宽分配后可能会出现空闲的src-dst可供调度
    int work_conservation(bool debug) {
        int ret = 0;
        for(int i = 0; i < coflow_list.size(); i++) {
            CoFlow* c = coflow_list[i];
            for(int j = 0; j < c->flows.size(); j++) {
                Flow* f = c->flows[j];
                int src = f->src_port;
                int dst = f->dst_port;
                if(src == dst)
                    continue;
                if(send_onport[src] == nullptr && recv_onport[dst] == nullptr) {
                    if(f->status == "suspend") {
                        ret = alloc_bw(f, debug);
                        f->status = "active";
                        if(f->src_actor->is_suspended()) {
                            f->src_actor->resume();
                        }
                        if(debug) {
                            std::cout<< "work conservation: "<< f->flow_id << 
                            " src_port: "<<f->src_port << " dst_port: "<< f->dst_port <<std::endl;
                        }
                    }
                }
            }
        }
        return 0;
    }

    int schedule(bool debug) {
        for(int i = 0; i < coflow_list.size(); i++) {
            if(coflow_list[i]->flows.size() == 0)
                continue;
            CoFlow *this_coflow = coflow_list[i];
            string type = this_coflow->flows[0]->flow_type;
            int ret;
            for(auto it = this_coflow->flows.begin(); it!= this_coflow->flows.end(); ) {
                Flow* this_flow = *it;
                if(this_flow->src_port == this_flow->dst_port) {
                    ++it;
                    continue;
                }
                if(this_flow->status == "init") {
                    // ret = this->alloc_bw(this_flow, debug);                    
                    if(inter_job_sche == "prio" || inter_job_sche =="prio_c")
                        ret = this->preem_alloc_bw(this_flow, debug);
                    else
                        ret = this->alloc_bw(this_flow, debug);
                    if(ret == 0) {
                        this_flow->status = "active";
                        if(debug) {
                            std::cout << "flow: "<< this_flow->flow_id << " status change: init to active"<<std::endl; 
                        }
                    }
                    else {
                        if(!( intra_job_sche == "none" && inter_job_sche == "prio" )) { 
                            this_flow->status = "suspend";
                            this_flow->src_actor->suspend();
                            if(debug) {
                                std::cout<< "suspend flow: "<< this_flow->flow_id << " actor: " <<this_flow->src_actor->get_name()<<std::endl;
                            }
                        }
                    }
                    ++it;
                }
                else if(this_flow->status == "suspend") {
                    // ret = this->alloc_bw(this_flow, debug);
                    if(inter_job_sche == "prio" || inter_job_sche=="prio_c")
                        ret = this->preem_alloc_bw(this_flow, debug);
                    else
                        ret = this->alloc_bw(this_flow, debug);
                    if(ret == 0) {
                        this_flow->status = "active";
                        if(this_flow->src_actor->is_suspended()) {
                            this_flow->src_actor->resume();
                            if(debug) {
                                std::cout<< "resume flow: "<< this_flow->flow_id << " actor: " <<this_flow->src_actor->get_name()<<std::endl;
                            }
                        }
                    }
                    ++it;
                }
                else if(this_flow->status == "done") {
                    it = this_coflow->flows.erase(it);
                    this_coflow->flow_cnt-=1;
                }
                else{
                    if(this_flow->src_actor->is_suspended())
                        this_flow->src_actor->resume();
                    ++it;
                }
            }
        }
        // if(can_alloc(true) > 0) {
        //     std::cout << "before can alloc: "<<can_alloc(debug)<<std::endl;
        // }
        if(inter_job_sche == "prio" || inter_job_sche=="prio_c") {
            this->work_conservation(debug);
        }
        for(auto it = coflow_list.begin(); it!=coflow_list.end(); ) {
            if((*it)->flow_cnt == 0) {
                string coflow_id = (*it)->coflow_id;
                it =  coflow_list.erase(it);
                if(debug)
                    std::cout <<"=====delete coflow:"<<coflow_id<< " success  in compute state!"<< std::endl;
            }
            else {
                ++it;
            }
        }
        return 0;
    }



    void operator()() 
    {
        // prepare 
        actor_list = e->get_all_actors();
        host_list = e->get_all_hosts();
        link_list = e->get_all_links();
        // 现在有三种actors: Job-0-Node, Job-0-PS-0, Job-0-Worker-0
        // scheduler需要和JobNode worker保持连接
        for (auto actor: actor_list) {
            // 跳过scheduler和PS actor
            // std::cout<< "report actor: " <<  actor->get_name() <<std::endl;
            if(actor == simgrid::s4u::Actor::self() || actor->get_name().find("PS")!=string::npos )
                continue;
            string mailboxname = "REG-" + actor->get_name();
            Mailbox* mb = Mailbox::by_name(mailboxname);
            mb->set_receiver(simgrid::s4u::Actor::self());
            reportboxs.push_back(mb);

            if(mailboxname.find("Node") != string::npos)
                ps_reportboxs.push_back(mb);
            else
                worker_reportboxs.push_back(mb);
            // std::cout<< "report mailbox: " <<  mailboxname <<std::endl;
        }
        
        // 接收job node注册消息
        // PULL report message format: Job-0-Node-INFO-[param_size]-[compute_time_list]-[placement]
        // [placement] = 1:gpu0_2:gpu2    workerid:gpuid
        // [compute_time_list] = t1_t2_t3
        for(int i = 0; i < ps_reportboxs.size(); i++) {
            if(ps_reportboxs[i]->get_name() == "REG-Job-9999-Node")
                continue;
            string* status = static_cast<std::string*>(ps_reportboxs[i]->get());
            // std::cout << "Register msg: " << *status <<std::endl;
            std::regex ws_re("-");
            std::vector<string> str_split(std::sregex_token_iterator(status->begin(), status->end(),ws_re, -1 ),                    std::sregex_token_iterator());
            int jobid = stoi(str_split[1]);
            int param_size = stoi(str_split[4]);
            std::regex spe("_");
            std::vector<string> str_ct(std::sregex_token_iterator(str_split[5].begin(), str_split[5].end(),spe, -1 ),         std::sregex_token_iterator());
            std::vector<double> ct_list;
            for(int j = 0; j < str_ct.size(); j++) {
                ct_list.push_back(stod(str_ct[j]));
            }

            std::vector<string> placement(std::sregex_token_iterator(str_split[6].begin(), str_split[6].end(),spe, -1 ),         std::sregex_token_iterator());
            jobs_info.push_back({jobid, param_size, ct_list, placement});
        }
        //���jobinfo按照jobid排序，便于查找
        std::sort(jobs_info.begin(), jobs_info.end(), job_cmp);

        // mailbox 与msg_buffer一一对应
        int msg_cnt = 0;
        std::vector<simgrid::s4u::CommPtr> msg_comms;
        std::vector<bool> has_comm(reportboxs.size(), false);  //判断对应位置的mailbox是否有comm存在
        int arr;
        void** msg_buffer = new void*[reportboxs.size()];  //应该放在外面，因为有的comm可能好多轮都收不到消息，但得保持buffer指针
        int printcnt = 10;
        while(true) {
            vector<string> recv_info;
            for(int i = 0; i < reportboxs.size(); i++) {
                if(!has_comm[i]) {
                    CommPtr msg_comm = reportboxs[i]->get_async(&msg_buffer[i]);
                    msg_comms.push_back(msg_comm);
                    has_comm[i]  = true;
                    // XBT_INFO( "comms: %s, index: %d", msg_comm->get_mailbox()->get_name().c_str(), i );
                }
            }
            // 时间间隔0.05 不能改,每50ms调度一次
            arr = Comm::wait_any_for(&msg_comms, 0.001);
            while( arr != -1 ){
                // XBT_INFO( "in while: %d", arr );
                simgrid::s4u::Mailbox* this_mb = msg_comms[arr]->get_mailbox();
                vector<Mailbox*>::iterator mb_pos = find(reportboxs.begin(), reportboxs.end(), this_mb);
                int index = mb_pos - reportboxs.begin();
                // 去掉已经完成的，并显示消息内容
                string* msg = static_cast<std::string*>(msg_buffer[index]);
                recv_info.push_back(*msg);
                // XBT_INFO( "while-message: %s mb: %s, index: %d", msg->c_str(), this_mb->get_name().c_str(), index );
                has_comm[index] = false;
                msg_comms.erase(msg_comms.begin() + arr);
                // 收到结束消息
                if(this_mb->get_name() == "REG-Job-9999-Node") {
                    simgrid::s4u::this_actor::exit();
                }
                if(msg_comms.size() == 0)
                    break;
                arr = Comm::wait_any_for(&msg_comms, 0.001);
            }
            int ret;
            bool debug = false;
            // 没有消息更新可以跳过调度
            if(inter_job_sche != "none" && recv_info.size() > 0){
                // if(recv_info.size() > 0) {
                //     std::cout<<"UPDATE size: "<<recv_info.size() << std::endl;
                //     for(int i = 0; i < actor_list.size(); i++) {
                //         if(actor_list[i]->is_suspended())
                //             std::cout<<" "<<actor_list[i]->get_name()<<std::endl;
                //     }
                //     std::cout<<" "<<std::endl;
                // }
                ret = this->update_coflow(recv_info, debug);
                if(ret == -1) {
                    std::cout<<"UPDATE ERROR"<< std::endl;
                }
                // std::cout<<"before sort"<<std::endl;
                // std::cout <<"coflow lisrt size: "<< coflow_list.size() << std::endl;
                // for(int i = 0 ;i<coflow_list.size(); i++) {
                //     std::cout << "coflow " << i <<" : "<<coflow_list[i]->coflow_id;
                // }
                // std::cout << std::endl;

                // 策略组合 [none, none] [rr, none|sjf|locof|prio] [prio, none|sjf|locof|prio]
                if(inter_job_sche == "sjf") {
                    std::sort(coflow_list.begin(), coflow_list.end(), cmp_sjf);
                }
                else if(inter_job_sche == "locof") {
                    std::sort(coflow_list.begin(), coflow_list.end(), cmp_locof);
                }
                else if(inter_job_sche == "prio") {
                    std::sort(coflow_list.begin(), coflow_list.end(), cmp_prio);
                }
                else if(inter_job_sche == "prio_c") {
                    std::sort(coflow_list.begin(), coflow_list.end(), cmp_prio_c);
                }
                this->schedule(debug);
                // for(int i = 0; i < send_onport.size(); i++) {
                //     if(send_onport[i] != nullptr )
                //         std::cout << "send_onport:"<<i<<" "<< send_onport[i]->flow_id <<" "<<send_onport[i]->src_actor->is_suspended()<<std::endl;
                // }
            }
        }

    }
};

int main(int argc, char *argv[])
{
    xbt_assert(argc > 2, "Usage: %s platform_file deployment_file\n", argv[0]);

    simgrid::s4u::Engine e(&argc, argv);
    std::string fname =  string(argv[2]) + ".name";
    std::ifstream actor_name(fname.c_str());

    if(actor_name) {
        std::string line;
        while(getline(actor_name, line)) {
            if(line.find("PS") != string::npos)
                e.register_actor<PS>(line.c_str());
            else if(line.find("Node") != string::npos)
                e.register_actor<JobNode>(line.c_str());
            else
                e.register_actor<Worker>(line.c_str());
        }
    }
    e.register_actor<Scheduler>("Scheduler");
    e.load_platform(argv[1]);
    e.load_deployment(argv[2]);
    intra_job_sche = argv[3];
    inter_job_sche = argv[4];
    e.run();

    return 0;
}
