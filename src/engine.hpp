#pragma once
// ============================================================================
// engine.hpp — C++ 引擎 (子进程, 无外部依赖)
//
// 进度显示对齐原版 pbb_all.cpp:
//   每 100 task 输出两行:
//     taskX finished,task_mex=Y,count:Z.ZZZT
//     tot=N, (八围,XP,XD),time: X.XXs, speed: X.XXXT/d,time left:XhXmXs
// ============================================================================
#include "common.hpp"
#include "charset_data.hpp"
#include "charset.hpp"
#include "model_data.hpp"
#include "utils.hpp"
#include "name.hpp"
#include "scoring.hpp"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cstdio>
#include <vector>
#include <cstring>
#include <string>
#include <random>
#include <chrono>
#include <atomic>
#include <algorithm>
#include <unordered_map>
#include <cstdlib>

#define MAX_QUEUE_LEN 4        // 任务队列容量
#define NUMBER_PER_TASK 1000000ULL  // 每任务处理的名字数

// ---- 任务数据结构 ----
struct TaskData{uint64_t L,R;int prefix_id,suffix_id,type,task_id;};
struct TaskQueue{TaskData d[MAX_QUEUE_LEN];int h=0,t=0;bool closed=false;std::mutex mtx;std::condition_variable cv_add,cv_get;};

// 环形队列操作 (生产者-消费者)
static void q_add(TaskQueue& q,const TaskData& d){std::unique_lock lk(q.mtx);q.cv_add.wait(lk,[&]{return q.t-q.h<MAX_QUEUE_LEN||q.closed;});if(!q.closed){q.d[q.t++%MAX_QUEUE_LEN]=d;q.cv_get.notify_one();}}
static bool q_get(TaskQueue& q,TaskData& d){std::unique_lock lk(q.mtx);q.cv_get.wait(lk,[&]{return q.h<q.t||q.closed;});if(q.closed&&q.h>=q.t)return false;d=q.d[q.h++%MAX_QUEUE_LEN];q.cv_add.notify_one();return true;}
static void q_close(TaskQueue& q){std::unique_lock lk(q.mtx);q.closed=true;q.cv_add.notify_all();q.cv_get.notify_all();}

// hex 字符串 → 字节流 (charset_bytes 解码)
static std::string hex_decode(const std::string& h){std::string o;for(size_t i=0;i+1<h.length();i+=2){int hi=h[i]>='a'?h[i]-'a'+10:h[i]>='A'?h[i]-'A'+10:h[i]-'0';int lo=h[i+1]>='a'?h[i+1]-'a'+10:h[i+1]>='A'?h[i+1]-'A'+10:h[i+1]-'0';o+=(char)((hi<<4)|lo);}return o;}

// CSV 分割 (prefixes/suffixes 列表)
static std::vector<std::string> split_csv(const std::string& s){std::vector<std::string> v;if(s.empty()){v.push_back("");return v;}std::string cur;for(char c:s){if(c==','){v.push_back(cur);cur.clear();}else cur+=c;}v.push_back(cur);return v;}

// ===== 引擎入口 =====
inline int engine_main(int argc,char**argv){
    (void)argc;(void)argv;
    init_exhanzi();

    // 从 stdin 读取参数 (Python 通过管道传入)
    std::unordered_map<std::string,std::string> kv;std::string line;
    while(std::getline(std::cin,line)){if(line.empty())continue;size_t eq=line.find('=');if(eq!=std::string::npos)kv[line.substr(0,eq)]=line.substr(eq+1);}

    // 读取参数
    std::string team=kv["team_name"];int n_threads=std::stoi(kv["n_threads"]);
    int scl=std::stoi(kv["scl"]),clen=std::stoi(kv["charset_len"]);
    std::string cbytes=hex_decode(kv["charset_bytes"]);
    auto prefixes=split_csv(kv["prefixes"]),suffixes=split_csv(kv["suffixes"]);
    int np=(int)prefixes.size(),ns=(int)suffixes.size();
    int mode=std::stoi(kv["mode"]),vlen=std::stoi(kv["variable_len"]);
    uint64_t rL=std::stoull(kv["range_L"]),rR=std::stoull(kv["range_R"]);
    int xpm=std::stoi(kv["xp_min"]),xdm=std::stoi(kv["xd_min"]);
    int collect_mode=kv.count("collect_mode")?std::stoi(kv["collect_mode"]):0;  // 0=否, 1=硬编码阈值, 2=自定义阈值
    // collect_mode=2: 自定义阈值 (对齐原版 special_thresholds)
    int c_8v=0,c_7v=0,c_hl=0,c_hp=0;
    if(collect_mode==2){
        c_8v=std::stoi(kv["c_eight_v_min"]);c_7v=std::stoi(kv["c_seven_v_min"]);
        c_hl=std::stoi(kv["c_hl_min"]);c_hp=std::stoi(kv["c_hp398_min"]);
    }
    int output_xp=kv.count("output_xp")?std::stoi(kv["output_xp"]):1;
    int output_log=kv.count("output_log")?std::stoi(kv["output_log"]):1;
    int output_speed=kv.count("output_speed")?std::stoi(kv["output_speed"]):1;

    // 打开输出文件
    std::string out_path="out/"+kv["result_file"];
    FILE*fp=fopen(out_path.c_str(),"a");if(!fp)return 1;
    FILE*fp_blue=nullptr;if(collect_mode>=1)fp_blue=fopen("out/blue.txt","a");  // mode 1/2 均收集
    FILE*flog=output_log?stderr:fopen("out/task_log.txt","a");
    FILE*fspeed=output_speed?stderr:fopen("out/speed_log.txt","a");

    // 初始化 Name 状态机 (队伍名 KSA)
    Name name_init;name_init.load_team(team.c_str());
    TaskQueue q;std::mutex out_mtx;std::atomic<int>tasks_done{0},total_found{0};
    int max_sum=0,max_xp=0,max_xd=0;       // 全局最大值 (out_mtx 保护)
    int mex_cur=0;                          // 最小未完成 task_id
    std::vector<bool> mex_vis;              // task_id 完成标记
    unsigned long long ALL_totnum=(rR-rL)*np;  // 总名字数 (对齐原版, 用于 time left)
    auto t_start=std::chrono::steady_clock::now();
    std::mt19937_64 rng_global(std::chrono::steady_clock::now().time_since_epoch().count());

    // mode=2/3/4 预处理
    int varlen_task=vlen;uint64_t random_range_max=1;
    if(mode>=2){varlen_task=0;uint64_t x=1;while(x<NUMBER_PER_TASK){varlen_task++;x*=clen;}if(varlen_task>vlen)varlen_task=vlen;random_range_max=x;}

    // 预分配 mex_vis (预计 task 数, 留 20% 余量)
    int est_tasks=(int)((rR-rL+NUMBER_PER_TASK-1)/NUMBER_PER_TASK*np*1.2)+10;
    mex_vis.resize(est_tasks,false);

    // ---- task_id 序号生成器 ----
    int task_id_counter=0;

    // ---- Producer: 生成任务 ----
    auto prod=[&]{
        if(mode==1)for(int j=0;j<np;j++)for(uint64_t i=rL;i<rR;i+=NUMBER_PER_TASK){
            uint64_t tr=i+NUMBER_PER_TASK;if(tr>rR)tr=rR;
            TaskData t;t.L=i;t.R=tr;t.type=1;t.prefix_id=j+1;t.suffix_id=(j%ns)+1;
            t.task_id=task_id_counter++;q_add(q,t);}
        else for(uint64_t i=rL;i<rR;i+=NUMBER_PER_TASK){
            uint64_t tr=i+NUMBER_PER_TASK;if(tr>rR)tr=rR;
            TaskData t;t.L=i;t.R=tr;t.type=mode;
            t.prefix_id=rng_global()%np+1;t.suffix_id=mode==4?t.prefix_id:rng_global()%ns+1;
            t.task_id=task_id_counter++;q_add(q,t);}
        q_close(q);
    };

    // ---- Consumer: 处理任务 (编码 + RC4 + 评分) ----
    auto cons=[&](int cid){Name name;memcpy(name.val_base,name_init.val_base,sizeof(name.val_base));std::mt19937_64 rng(rng_global()+cid*1234567);TaskData t;
        int local_found=0,local_max_sum=0,local_max_xp=0,local_max_xd=0;
        const char* cb=cbytes.data();  // 局部缓存指针
        // 编码宏: 全内联, 编译器在 scl==4 分支将 memcpy(...,4) 优化为单条 mov
        #define ENC(dst,ci) do{const char*_s=cb+(ci)*scl;if(scl==4)memcpy((dst),_s,4);else if(scl==1)*(dst)=*_s;else if(scl==2)memcpy((dst),_s,2);else if(scl==3)memcpy((dst),_s,3);else memcpy((dst),_s,scl);}while(0)
        while(q_get(q,t)){
            // 构建名字缓冲区: prefix + 占位 + suffix
            const std::string&p=prefixes[(t.prefix_id-1)%np];const std::string&s=suffixes[(t.suffix_id-1)%ns];
            int plen=(int)p.size(),slen=(int)s.size(),nlen=plen+vlen*scl+slen;
            char buf[512];memcpy(buf,p.data(),plen);memset(buf+plen,0,vlen*scl);
            if(slen)memcpy(buf+plen+vlen*scl,s.data(),slen);
            uint64_t L=t.L,R=t.R;int evar=vlen,epre=plen;
            if(mode>=2){
                int extra=vlen-varlen_task;if(extra>0)for(int pos=plen;pos<plen+extra*scl;pos+=scl){int ci=rng()%clen;ENC(buf+pos,ci);}
                epre+=extra*scl;evar=varlen_task;name.PRELEN=epre;name.load_prefix(buf,nlen);
                if(mode==2||mode==4){L=rng()%random_range_max;R=L+NUMBER_PER_TASK;}
            }else{
                name.PRELEN=plen;name.load_prefix(buf,nlen);uint8_t dl[16],dr[16];uint64_t now;
                now=L;for(int d=vlen-1;d>=0;d--){dl[d]=now%clen;now/=clen;}
                now=R-1;for(int d=vlen-1;d>=0;d--){dr[d]=now%clen;now/=clen;}
                int up=0;while(up<vlen&&dl[up]==dr[up])up++;
                now=L;for(int d=vlen-1;d>=0;d--){int ci=now%clen;ENC(buf+epre+d*scl,ci);now/=clen;}
                epre+=up*scl;evar-=up;
            }
            // 热路径: 编码 → score_full
            if(mode==3){
                for(uint64_t i=L;i<R;i++){
                    for(int pos=epre+evar*scl-scl;pos>=epre;pos-=scl){int ci=rng()%clen;ENC(buf+pos,ci);}
                    ScoreResult r=score_full(buf,nlen,name);
                    if(!r.flag)continue;
                    int emin=xpm;if(r.flag3>=50)emin-=300;
                    // 追踪本 task 的局部最大值 (所有结果, 不是只达标)
                    if(r.sum>local_max_sum)local_max_sum=r.sum;
                    if(r.xp>local_max_xp)local_max_xp=r.xp;
                    if(r.xd>local_max_xd)local_max_xd=r.xd;
                    if(r.xp>=emin||r.xd>=xdm){std::lock_guard lk(out_mtx);local_found++;
                        if(output_xp)fprintf(fp,"%.*s@%s %d %d\n",nlen,buf,team.c_str(),r.xp,r.xd);
                        else fprintf(fp,"%.*s@%s\n",nlen,buf,team.c_str());}
                    if(collect_mode>=1&&fp_blue){int sum=r.sum,raw_hp=r.props[7]-36,hl=*std::min_element(r.props,r.props+7);bool blue=false;
                        if(collect_mode==1){if(sum>=777||(sum*3-raw_hp)>=2000||(raw_hp==398&&sum>=741)||(hl>=93))blue=true;}
                        else{if(sum>=c_8v||(sum-raw_hp/3)>=c_7v||(raw_hp==398&&sum>=c_hp)||(hl>=c_hl))blue=true;}
                        if(blue){std::lock_guard lk(out_mtx);fprintf(fp_blue,"%.*s@%s\n",nlen,buf,team.c_str());}}
                }
            }else{
                for(uint64_t i=L;i<R;i++){uint64_t now=i;for(int pos=epre+evar*scl-scl;pos>=epre;pos-=scl){int ci=now%clen;ENC(buf+pos,ci);now/=clen;}
                    ScoreResult r=score_full(buf,nlen,name);
                    if(!r.flag)continue;
                    int emin=xpm;if(r.flag3>=50)emin-=300;
                    if(r.sum>local_max_sum)local_max_sum=r.sum;
                    if(r.xp>local_max_xp)local_max_xp=r.xp;
                    if(r.xd>local_max_xd)local_max_xd=r.xd;
                    if(r.xp>=emin||r.xd>=xdm){std::lock_guard lk(out_mtx);local_found++;
                        if(output_xp)fprintf(fp,"%.*s@%s %d %d\n",nlen,buf,team.c_str(),r.xp,r.xd);
                        else fprintf(fp,"%.*s@%s\n",nlen,buf,team.c_str());}
                    if(collect_mode>=1&&fp_blue){int sum=r.sum,raw_hp=r.props[7]-36,hl=*std::min_element(r.props,r.props+7);bool blue=false;
                        if(collect_mode==1){if(sum>=777||(sum*3-raw_hp)>=2000||(raw_hp==398&&sum>=741)||(hl>=93))blue=true;}
                        else{if(sum>=c_8v||(sum-raw_hp/3)>=c_7v||(raw_hp==398&&sum>=c_hp)||(hl>=c_hl))blue=true;}
                        if(blue){std::lock_guard lk(out_mtx);fprintf(fp_blue,"%.*s@%s\n",nlen,buf,team.c_str());}}
                }
            }
            // ===== task 完成: 更新全局状态 + 进度显示 (对齐原版 pbb_all.cpp) =====
            {
                std::lock_guard lk(out_mtx);
                total_found+=local_found;
                if(local_max_sum>max_sum)max_sum=local_max_sum;
                if(local_max_xp>max_xp)max_xp=local_max_xp;
                if(local_max_xd>max_xd)max_xd=local_max_xd;
                // mex 追踪 (原版 mex_vis + mex_cur)
                if(t.task_id>=(int)mex_vis.size())mex_vis.resize(t.task_id+256,false);
                mex_vis[t.task_id]=true;
                while(mex_cur<(int)mex_vis.size()&&mex_vis[mex_cur])mex_cur++;
                // 进度显示: 每 100 task (对齐原版)
                int td=++tasks_done;
                if(td%100==0){
                    auto now=std::chrono::steady_clock::now();
                    double sec=std::chrono::duration<double>(now-t_start).count();
                    long long done_num=1ll*td*NUMBER_PER_TASK;
                    long long tmlft=ALL_totnum>done_num?((ALL_totnum-done_num)*1.0/done_num)*sec:0;
                    // 原版第一行: taskX finished, task_mex=Y, count:Z.ZZZT
                    if(output_log)fprintf(flog,"task%d finished,task_mex=%d,count:%.6lfT\n",
                        t.task_id,mex_cur,done_num/1e12);
                    else fprintf(flog,"task%d finished,task_mex=%d,count:%.6lfT\n",
                        t.task_id,mex_cur,done_num/1e12);
                    // 原版第二行: tot=N, (八围,XP,XD), time: Xs, speed: XT/d, time left:XhXmXs
                    if(output_speed)fprintf(fspeed,"tot=%d, (%d,%d,%d),time: %.2fs, speed: %.6fT/d,time left:%lldh%lldm%llds\n",
                        total_found.load(),max_sum,max_xp,max_xd,sec,
                        td/sec*86400*NUMBER_PER_TASK/1e12,
                        tmlft/3600,(tmlft%3600)/60,tmlft%60);
                    else fprintf(fspeed,"tot=%d, (%d,%d,%d),time: %.2fs, speed: %.6fT/d,time left:%lldh%lldm%llds\n",
                        total_found.load(),max_sum,max_xp,max_xd,sec,
                        td/sec*86400*NUMBER_PER_TASK/1e12,
                        tmlft/3600,(tmlft%3600)/60,tmlft%60);
                    fflush(output_log?stderr:flog);
                    fflush(output_speed?stderr:fspeed);
                }
            }
            local_found=0;local_max_sum=local_max_xp=local_max_xd=0;
        }};

    // 启动线程
    std::thread pt(prod);std::vector<std::thread>cts;
    for(int i=0;i<n_threads;i++)cts.emplace_back(cons,i);
    pt.join();for(auto&t:cts)t.join();

    // 关闭文件
    fclose(fp);if(fp_blue)fclose(fp_blue);
    if(!output_log&&flog!=stderr)fclose(flog);
    if(!output_speed&&fspeed!=stderr)fclose(fspeed);
    return 0;
}
