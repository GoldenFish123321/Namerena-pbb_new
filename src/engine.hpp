#pragma once
// ============================================================================
// engine.hpp — C++ 引擎 (子进程, 无外部依赖)
//
// 参数通过 stdin 管道传入 (Python 写, 引擎读), charset 以 hex 编码。
// 支持 mode=1/2, collect_mode=0/1, output_xp/log/speed。
// 编译: g++ -std=c++17 -mavx2 -O3 -Isrc -o pbb_engine engine_main.cpp
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
struct TaskData{uint64_t L,R;int prefix_id,suffix_id,type;};
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
    int scl=std::stoi(kv["scl"]),clen=std::stoi(kv["charset_len"]);  // scl=单字符字节数, clen=字符集大小
    std::string cbytes=hex_decode(kv["charset_bytes"]);               // 解码后的字符集字节流
    auto prefixes=split_csv(kv["prefixes"]),suffixes=split_csv(kv["suffixes"]);
    int np=(int)prefixes.size(),ns=(int)suffixes.size();
    int mode=std::stoi(kv["mode"]),vlen=std::stoi(kv["variable_len"]);  // mode=1顺序/2随机, vlen=可变字符数
    uint64_t rL=std::stoull(kv["range_L"]),rR=std::stoull(kv["range_R"]);  // 枚举区间
    int xpm=std::stoi(kv["xp_min"]),xdm=std::stoi(kv["xd_min"]);           // 评分阈值
    int collect_mode=kv.count("collect_mode")?std::stoi(kv["collect_mode"]):0;  // 0=否, 1=收集破纪录号
    int output_xp=kv.count("output_xp")?std::stoi(kv["output_xp"]):1;           // 是否输出分数
    int output_log=kv.count("output_log")?std::stoi(kv["output_log"]):1;        // 日志输出: 1=屏幕 0=文件
    int output_speed=kv.count("output_speed")?std::stoi(kv["output_speed"]):1;

    // 打开输出文件
    std::string out_path="out/"+kv["result_file"];
    FILE*fp=fopen(out_path.c_str(),"a");if(!fp)return 1;
    FILE*fp_blue=nullptr;if(collect_mode==1)fp_blue=fopen("out/blue.txt","a");  // 破纪录号输出
    FILE*flog=output_log?stderr:fopen("out/task_log.txt","a");
    FILE*fspeed=output_speed?stderr:fopen("out/speed_log.txt","a");

    // 初始化 Name 状态机 (队伍名 KSA)
    Name name_init;name_init.load_team(team.c_str());
    TaskQueue q;std::mutex out_mtx;std::atomic<int>tasks_done{0},total_found{0};
    auto t_start=std::chrono::steady_clock::now();
    std::mt19937_64 rng_global(std::chrono::steady_clock::now().time_since_epoch().count());

    // mode=2 预处理: 计算每任务的可变字符数
    // mode=2/3/4 预处理
    int varlen_task=vlen;uint64_t random_range_max=1;
    if(mode>=2){varlen_task=0;uint64_t x=1;while(x<NUMBER_PER_TASK){varlen_task++;x*=clen;}if(varlen_task>vlen)varlen_task=vlen;random_range_max=x;}

    // ---- Producer: 生成任务 ----
    auto prod=[&]{
        if(mode==1)for(int j=0;j<np;j++)for(uint64_t i=rL;i<rR;i+=NUMBER_PER_TASK){uint64_t tr=i+NUMBER_PER_TASK;if(tr>rR)tr=rR;TaskData t;t.L=i;t.R=tr;t.type=1;t.prefix_id=j+1;t.suffix_id=(j%ns)+1;q_add(q,t);}
        else for(uint64_t i=rL;i<rR;i+=NUMBER_PER_TASK){uint64_t tr=i+NUMBER_PER_TASK;if(tr>rR)tr=rR;TaskData t;t.L=i;t.R=tr;t.type=mode;
            t.prefix_id=rng_global()%np+1;t.suffix_id=mode==4?t.prefix_id:rng_global()%ns+1;q_add(q,t);}
        q_close(q);
    };

    // ---- Consumer: 处理任务 (编码 + RC4 + 评分) ----
    auto cons=[&](int cid){Name name;memcpy(name.val_base,name_init.val_base,sizeof(name.val_base));std::mt19937_64 rng(rng_global()+cid*1234567);TaskData t;int found=0;
        while(q_get(q,t)){
            // 构建名字缓冲区: prefix + 占位 + suffix
            const std::string&p=prefixes[(t.prefix_id-1)%np];const std::string&s=suffixes[(t.suffix_id-1)%ns];int plen=(int)p.size(),slen=(int)s.size(),nlen=plen+vlen*scl+slen;char buf[512];memcpy(buf,p.data(),plen);memset(buf+plen,0,vlen*scl);if(slen)memcpy(buf+plen+vlen*scl,s.data(),slen);
            uint64_t L=t.L,R=t.R;int evar=vlen,epre=plen;
            if(mode>=2){
                // 随机前缀扩展
                int extra=vlen-varlen_task;if(extra>0)for(int pos=plen;pos<plen+extra*scl;pos+=scl){int ci=rng()%clen;memcpy(buf+pos,cbytes.data()+ci*scl,scl);}
                epre+=extra*scl;evar=varlen_task;name.PRELEN=epre;name.load_prefix(buf,nlen);
                if(mode==2||mode==4){L=rng()%random_range_max;R=L+NUMBER_PER_TASK;}
            }else{
                // 公共前缀优化 (mode=1)
                name.PRELEN=plen;name.load_prefix(buf,nlen);uint8_t dl[16],dr[16];uint64_t now;now=L;for(int d=vlen-1;d>=0;d--){dl[d]=now%clen;now/=clen;}now=R-1;for(int d=vlen-1;d>=0;d--){dr[d]=now%clen;now/=clen;}int up=0;while(up<vlen&&dl[up]==dr[up])up++;now=L;for(int d=vlen-1;d>=0;d--){int ci=now%clen;memcpy(buf+epre+d*scl,cbytes.data()+ci*scl,scl);now/=clen;}epre+=up*scl;evar-=up;
            }
            // 热路径: 编码 → score_full
            if(mode==3){
                // 逐位随机: 每个字符独立随机
                for(uint64_t i=L;i<R;i++){
                    for(int pos=epre+evar*scl-scl;pos>=epre;pos-=scl){int ci=rng()%clen;memcpy(buf+pos,cbytes.data()+ci*scl,scl);}
                    ScoreResult r=score_full(buf,nlen,name);
                    if(!r.flag)continue;int emin=xpm;if(r.flag3>=50)emin-=300;
                    if(r.xp>=emin||r.xd>=xdm){std::lock_guard lk(out_mtx);found++;if(output_xp)fprintf(fp,"%.*s@%s %d %d\n",nlen,buf,team.c_str(),r.xp,r.xd);else fprintf(fp,"%.*s@%s\n",nlen,buf,team.c_str());}
                    if(collect_mode==1&&fp_blue){int sum=r.sum,prop7=r.props[7],hl=*std::min_element(r.props,r.props+7);if(sum>=777||(sum*3-prop7)>=2000||(prop7==398&&sum>=741)||(hl>=93)){std::lock_guard lk(out_mtx);fprintf(fp_blue,"%.*s@%s\n",nlen,buf,team.c_str());}}
                }
            }else{
                for(uint64_t i=L;i<R;i++){uint64_t now=i;for(int pos=epre+evar*scl-scl;pos>=epre;pos-=scl){int ci=now%clen;memcpy(buf+pos,cbytes.data()+ci*scl,scl);now/=clen;}ScoreResult r=score_full(buf,nlen,name);if(!r.flag)continue;int emin=xpm;if(r.flag3>=50)emin-=300;if(r.xp>=emin||r.xd>=xdm){std::lock_guard lk(out_mtx);found++;if(output_xp)fprintf(fp,"%.*s@%s %d %d\n",nlen,buf,team.c_str(),r.xp,r.xd);else fprintf(fp,"%.*s@%s\n",nlen,buf,team.c_str());}if(collect_mode==1&&fp_blue){int sum=r.sum,prop7=r.props[7],hl=*std::min_element(r.props,r.props+7);if(sum>=777||(sum*3-prop7)>=2000||(prop7==398&&sum>=741)||(hl>=93)){std::lock_guard lk(out_mtx);fprintf(fp_blue,"%.*s@%s\n",nlen,buf,team.c_str());}}}
            }
            tasks_done+=1;int td=tasks_done.load();if(td%100==0||td<=10){auto now=std::chrono::steady_clock::now();double sec=std::chrono::duration<double>(now-t_start).count();fprintf(flog,"task%d done, tot=%d, speed=%.4fT/d\n",td,total_found.load()+found,td*NUMBER_PER_TASK/sec*86400/1e12);fflush(flog);}}
        total_found+=found;};

    // 启动线程
    std::thread pt(prod);std::vector<std::thread>cts;for(int i=0;i<n_threads;i++)cts.emplace_back(cons,i);pt.join();for(auto&t:cts)t.join();

    // 关闭文件
    fclose(fp);if(fp_blue)fclose(fp_blue);if(!output_log&&flog!=stderr)fclose(flog);if(!output_speed&&fspeed!=stderr)fclose(fspeed);

    // 最终报告
    auto t_end=std::chrono::steady_clock::now();double sec=std::chrono::duration<double>(t_end-t_start).count();
    fprintf(stderr,"tot=%d, time: %.2fs, speed: %.6fT/d\n",total_found.load(),sec,tasks_done.load()*NUMBER_PER_TASK/sec*86400/1e12);
    return 0;
}
