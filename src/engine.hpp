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
#include "scoring_1035.hpp"
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
#include <limits>

#define MAX_QUEUE_LEN 4        // 任务队列容量
// ---- 两层并行术语 (第一性原理: f(i) 无依赖, 切 i 空间) ----
//   chunk: 引擎内部线程级切分单位 (本文件), 生产者按 CHUNK_SIZE 切 [rL,rR) 喂给消费者线程
//   task : 分布式级切分单位 (服务端), 服务端按更大粒度切总 range 下发给各客户端
//          客户端拿到一个 task 后, 在本进程内再被切成多个 chunk 并行
// 二者是同一抽象的两层, 不可混用。本宏仅指 chunk。
#define CHUNK_SIZE 1000000ULL  // 每个 chunk 处理的名字数 (引擎内部线程级单位)

// ---- 确定性子种子派生 (splitmix64) ----
// 由 (g_seed, task_id) 混合出每个 chunk 的专属种子。
// 第一性原理: chunk 的随机性必须由其逻辑坐标 (seed, task_id) 唯一决定,
//             与"哪个线程跑、跑的顺序、用几个线程"等执行环境细节完全无关。
//             这样同 seed + 同 range 在任意线程数下结果可复现。
static inline uint64_t splitmix64(uint64_t x){
    x+=0x9E3779B97F4A7C15ULL;
    x=(x^(x>>30))*0xBF58476D1CE4E5B9ULL;
    x=(x^(x>>27))*0x94D049BB133111EBULL;
    return x^(x>>31);
}
static inline uint64_t derive_chunk_seed(uint64_t g_seed,uint64_t task_id){
    return splitmix64(g_seed ^ splitmix64(task_id));
}

static inline uint64_t ceil_div_u64(uint64_t n,uint64_t d){
    return n/d + (n%d!=0);
}

static inline uint64_t saturating_mul_u64(uint64_t a,uint64_t b){
    const uint64_t umax=std::numeric_limits<uint64_t>::max();
    if(a!=0&&b>umax/a)return umax;
    return a*b;
}

static inline uint64_t round_up_to_chunk_u64(uint64_t n){
    return saturating_mul_u64(ceil_div_u64(n,CHUNK_SIZE),CHUNK_SIZE);
}

static inline uint64_t chunk_end_u64(uint64_t begin,uint64_t end){
    return end-begin>CHUNK_SIZE?begin+CHUNK_SIZE:end;
}

// ---- 任务数据结构 ----
// chunk_seed: 该 chunk 的确定性随机种子 (producer 创建时派生, 与线程无关)
struct TaskData{uint64_t L,R;int prefix_id,suffix_id,type;uint64_t task_id,chunk_seed;};
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

    // 从 stdin 读取参数 (Python 通过管道传入)
    std::unordered_map<std::string,std::string> kv;std::string line;
    while(std::getline(std::cin,line)){if(line.empty())continue;size_t eq=line.find('=');if(eq!=std::string::npos)kv[line.substr(0,eq)]=line.substr(eq+1);}

    int debug_mode=kv.count("debug_mode")?std::stoi(kv["debug_mode"]):0;
#define DBG(fmt,...) do{if(debug_mode){fprintf(stderr,"[engine] " fmt "\n",##__VA_ARGS__);fflush(stderr);}}while(0)
    DBG("=== START ===");

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
    FILE*fp=fopen(out_path.c_str(),"a");if(!fp){fprintf(stderr,"[engine] ERROR: cannot open %s\n",out_path.c_str());return 1;}
    FILE*fp_blue=nullptr;if(collect_mode>=1)fp_blue=fopen("out/blue.txt","a");  // mode 1/2 均收集
    FILE*flog=output_log?stderr:fopen("out/task_log.txt","a");
    FILE*fspeed=output_speed?stderr:fopen("out/speed_log.txt","a");
    fprintf(flog,"[engine] SIMD: %s\n",PBB_SIMD_NAME);fflush(flog);
    DBG("files opened, n_threads=%d mode=%d range=[%llu,%llu)",
        n_threads,mode,(unsigned long long)rL,(unsigned long long)rR);

    // 初始化 Name 状态机 (队伍名 KSA)
    Name name_init;name_init.load_team(team.c_str());
    TaskQueue q;std::mutex out_mtx;std::atomic<uint64_t>tasks_done{0};std::atomic<int>total_found{0};
    int max_sum=0,max_xp=0,max_xd=0;       // 全局最大值 (out_mtx 保护)
    uint64_t mex_cur=0;                     // 最小未完成 task_id
    std::vector<bool> mex_vis;              // task_id 完成标记
    uint64_t range_len=rR-rL;
    uint64_t range_chunks=ceil_div_u64(range_len,CHUNK_SIZE);
    unsigned long long ALL_totnum=saturating_mul_u64(range_len,(uint64_t)np);  // 总名字数 (对齐原版, 用于 time left)
    // mode 2/3/4: 随机模式不按 prefix 翻倍, 总名数 = range 长度
    if(mode>=2) ALL_totnum = range_len;
    // mode 2/4: consumer 中 R=L+CHUNK_SIZE 覆盖了 producer 区间,
    // 每个 chunk 固定处理 CHUNK_SIZE 个名字, 最后一块可能超出理论 range,
    // 因此 ALL_totnum 需向上取整到 CHUNK_SIZE 边界 (否则 SUMMARY speed 和进度条 time-left 偏小)
    if(mode==2||mode==4) ALL_totnum = round_up_to_chunk_u64(range_len);
    auto t_start=std::chrono::steady_clock::now();
    // ---- 随机种子 (A1: 种子驱动随机 mode, 为分布式可复现性预留) ----
    //   seed 缺失 / 为空 / 为 "-1": 用时间熵 (单机默认, 每次结果不同, 行为不变)
    //   seed 为具体数值        : 确定性种子 (分布式: 服务端给每个 task 存固定 seed,
    //                            超时重发同 seed → 结果可复现/可去重)
    // 问题3修复 (2026-06-01): 每个 chunk 的随机性派生自 (g_seed, task_id),
    //   与线程数/调度顺序完全无关。同 seed + 同 range → 任意线程数下结果可复现。
    uint64_t g_seed;
    {
        auto it=kv.find("seed");
        if(it!=kv.end() && !it->second.empty() && it->second!="-1")
            g_seed=std::stoull(it->second);
        else
            g_seed=(uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
    }

    // mode=2/3/4 预处理
    int varlen_task=vlen;uint64_t random_range_max=1;
    if(mode>=2){varlen_task=0;uint64_t x=1;while(x<CHUNK_SIZE){varlen_task++;x*=clen;}if(varlen_task>vlen)varlen_task=vlen;random_range_max=x;}

    // 预分配 mex_vis (预计 task 数, 留 20% 余量)
    // mode 1: 每个 prefix 跑完整区间 → 总 task = range_chunks × np
    // mode 2/3/4: producer 只按 interval 切 chunk → 总 task = range_chunks (不乘 np)
    uint64_t est_tasks_u = (mode == 1 ? saturating_mul_u64(range_chunks,(uint64_t)np) : range_chunks);
    // 安全上限: 10M 条已覆盖任何实际运行场景, 防止超大 range 或 uint64 溢出 (mex_vis 支持动态扩容)
    if (est_tasks_u > 10000000ULL) est_tasks_u = 10000000ULL;
    int est_tasks = (int)(est_tasks_u * 1.2) + 10;
    mex_vis.resize(est_tasks,false);

    // ---- task_id 序号生成器 ----
    uint64_t task_id_counter=0;

    // ---- Producer: 生成任务 ----
    // 每个 chunk 的随机决策 (prefix/suffix 选择) 由其 chunk_seed 派生,
    // 不再共享 rng_global, 确保与执行顺序/线程数无关。
    auto prod=[&]{
        if(mode==1)for(int j=0;j<np;j++)for(uint64_t i=rL;i<rR;){
            uint64_t tr=chunk_end_u64(i,rR);
            TaskData t;t.L=i;t.R=tr;t.type=1;t.prefix_id=j+1;t.suffix_id=(j%ns)+1;
            t.task_id=task_id_counter++;t.chunk_seed=derive_chunk_seed(g_seed,t.task_id);q_add(q,t);
            if(rR-i<=CHUNK_SIZE)break;
            i+=CHUNK_SIZE;}
        else for(uint64_t i=rL;i<rR;){
            uint64_t tr=chunk_end_u64(i,rR);
            TaskData t;t.L=i;t.R=tr;t.type=mode;
            t.task_id=task_id_counter++;t.chunk_seed=derive_chunk_seed(g_seed,t.task_id);
            std::mt19937_64 prng(t.chunk_seed);
            t.prefix_id=prng()%np+1;t.suffix_id=mode==4?t.prefix_id:prng()%ns+1;
            q_add(q,t);
            if(rR-i<=CHUNK_SIZE)break;
            i+=CHUNK_SIZE;}
        q_close(q);
    };

    // ---- Consumer: 处理任务 (编码 + RC4 + 评分) ----
    // rng 改为 per-chunk 重建 (用 t.chunk_seed), 不再按线程 id 初始化、跨 chunk 连续使用。
    // 重构 (2026-06-02): 四种模式编码逻辑抽出为独立 helper — 消除分支交织与评分/blue 重复代码。
    //   process_one()   — 评分 + blue 判定 (四种模式共享)
    //   consume_seq()   — 顺序进位编码 (mode 1/2/4 共用)
    //   consume_rand()  — 随机逐位编码 (mode 3 专用)
    //   consume_mode1() — mode 1 进位检测 + 顺序编码
    //   consume_mode24()— mode 2/4 随机前缀/LR + 顺序编码
    //   consume_mode3() — mode 3 随机前缀 + 随机逐位编码
    auto cons=[&](int cid){(void)cid;Name name;memcpy(name.val_base,name_init.val_base,sizeof(name.val_base));TaskData t;
        int local_found=0,local_max_sum=0,local_max_xp=0,local_max_xd=0;
        const char* cb=cbytes.data();  // 局部缓存指针
        // 编码宏: 全内联, 编译器在 scl==4 分支将 memcpy(...,4) 优化为单条 mov
        #define ENC(dst,ci) do{const char*_s=cb+(ci)*scl;if(scl==4)memcpy((dst),_s,4);else if(scl==1)*(dst)=*_s;else if(scl==2)memcpy((dst),_s,2);else if(scl==3)memcpy((dst),_s,3);else memcpy((dst),_s,scl);}while(0)

        // ---- 公共 helper: 评分 + blue 判定 (四种模式共享, 消除重复) ----
        auto process_one=[&](const char* buf,int nlen,const ScoreResult& r){
            if(!r.flag)return;
            int emin=xpm;if(r.flag3>=50)emin-=300;
            if(r.sum>local_max_sum)local_max_sum=r.sum;
            if(r.xp>local_max_xp)local_max_xp=r.xp;
            if(r.xd>local_max_xd)local_max_xd=r.xd;
            if(r.xp>=emin||r.xd>=xdm){std::lock_guard lk(out_mtx);local_found++;
                if(output_xp)fprintf(fp,"%.*s@%s %d %d\n",nlen,buf,team.c_str(),r.xp,r.xd);
                else fprintf(fp,"%.*s@%s\n",nlen,buf,team.c_str());
                fflush(fp);}
            if(collect_mode>=1&&fp_blue){int sum=r.sum,raw_hp=r.props[7]-36,hl=*std::min_element(r.props,r.props+7);bool blue=false;
                if(collect_mode==1){if(sum>=777||(sum*3-raw_hp)>=2000||(raw_hp==398&&sum>=741)||(hl>=93))blue=true;}
                else{if(sum>=c_8v||(sum-raw_hp/3)>=c_7v||(raw_hp==398&&sum>=c_hp)||(hl>=c_hl))blue=true;}
                if(blue){std::lock_guard lk(out_mtx);fprintf(fp_blue,"%.*s@%s\n",nlen,buf,team.c_str());fflush(fp_blue);}}
        };

        // ---- 编码 helper: 顺序进位 (mode 1/2/4 共用) ----
        auto consume_seq=[&](char* buf,int nlen,Name& name,int epre,int evar,uint64_t L,uint64_t R){
            for(uint64_t i=L;i<R;i++){uint64_t now=i;
                for(int pos=epre+evar*scl-scl;pos>=epre;pos-=scl){int ci=now%clen;ENC(buf+pos,ci);now/=clen;}
                process_one(buf,nlen,score_full(buf,nlen,name));}
        };

        // ---- 编码 helper: 随机逐位 (mode 3 专用) ----
        auto consume_rand=[&](char* buf,int nlen,Name& name,int epre,int evar,uint64_t L,uint64_t R,std::mt19937_64& rng){
            for(uint64_t i=L;i<R;i++){
                for(int pos=epre+evar*scl-scl;pos>=epre;pos-=scl){int ci=rng()%clen;ENC(buf+pos,ci);}
                process_one(buf,nlen,score_full(buf,nlen,name));}
        };

        // ---- mode 1: 顺序区间, 前缀/后缀全组合, 进位检测跳过共同前缀 ----
        auto consume_mode1=[&](char* buf,int nlen,Name& name,int plen,int vlen,uint64_t L,uint64_t R){
            name.PRELEN=plen;name.load_prefix(buf,nlen);
            uint8_t dl[16],dr[16];uint64_t now;
            now=L;for(int d=vlen-1;d>=0;d--){dl[d]=now%clen;now/=clen;}
            now=R-1;for(int d=vlen-1;d>=0;d--){dr[d]=now%clen;now/=clen;}
            int up=0;while(up<vlen&&dl[up]==dr[up])up++;
            now=L;for(int d=vlen-1;d>=0;d--){int ci=now%clen;ENC(buf+plen+d*scl,ci);now/=clen;}
            int epre=plen+up*scl,evar=vlen-up;
            consume_seq(buf,nlen,name,epre,evar,L,R);
        };

        // ---- mode 2/4: 随机额外字符 + 随机区间 + 顺序进位 ----
        auto consume_mode24=[&](char* buf,int nlen,Name& name,int plen,uint64_t& L,uint64_t& R,std::mt19937_64& rng){
            int extra=vlen-varlen_task;if(extra>0)for(int pos=plen;pos<plen+extra*scl;pos+=scl){int ci=rng()%clen;ENC(buf+pos,ci);}
            int epre=plen+extra*scl,evar=varlen_task;name.PRELEN=epre;name.load_prefix(buf,nlen);
            L=rng()%random_range_max;R=L+CHUNK_SIZE;
            consume_seq(buf,nlen,name,epre,evar,L,R);
        };

        // ---- mode 3: 随机额外字符 + 随机逐位编码 ----
        auto consume_mode3=[&](char* buf,int nlen,Name& name,int plen,uint64_t L,uint64_t R,std::mt19937_64& rng){
            int extra=vlen-varlen_task;if(extra>0)for(int pos=plen;pos<plen+extra*scl;pos+=scl){int ci=rng()%clen;ENC(buf+pos,ci);}
            int epre=plen+extra*scl,evar=varlen_task;name.PRELEN=epre;name.load_prefix(buf,nlen);
            consume_rand(buf,nlen,name,epre,evar,L,R,rng);
        };

        while(q_get(q,t)){
            // 每个 chunk 用自己的确定性种子重建 rng (与执行环境无关)
            std::mt19937_64 rng(t.chunk_seed);
            // 构建名字缓冲区: prefix + 占位 + suffix
            const std::string&p=prefixes[(t.prefix_id-1)%np];const std::string&s=suffixes[(t.suffix_id-1)%ns];
            int plen=(int)p.size(),slen=(int)s.size(),nlen=plen+vlen*scl+slen;
            char buf[512];memcpy(buf,p.data(),plen);memset(buf+plen,0,vlen*scl);
            if(slen)memcpy(buf+plen+vlen*scl,s.data(),slen);
            buf[nlen]=0;  // 修复: load_prefix/load_name 循环第一轮读 name[nlen], 与 pbb_all.cpp 全局零初始化对齐
            uint64_t L=t.L,R=t.R;

            // 模式分发: 各模式独立的编码逻辑
            if(mode==1)      consume_mode1(buf,nlen,name,plen,vlen,L,R);
            else if(mode==3) consume_mode3(buf,nlen,name,plen,L,R,rng);
            else             consume_mode24(buf,nlen,name,plen,L,R,rng);  // mode 2/4

            // ===== task 完成: 更新全局状态 + 进度显示 (对齐原版 pbb_all.cpp) =====
            {
                std::lock_guard lk(out_mtx);
                total_found+=local_found;
                if(local_max_sum>max_sum)max_sum=local_max_sum;
                if(local_max_xp>max_xp)max_xp=local_max_xp;
                if(local_max_xd>max_xd)max_xd=local_max_xd;
                // mex 追踪 (原版 mex_vis + mex_cur)
                if(t.task_id>=(uint64_t)mex_vis.size()){
                    uint64_t next_size=t.task_id+256;
                    if(next_size<t.task_id)next_size=std::numeric_limits<uint64_t>::max();
                    uint64_t max_size=(uint64_t)mex_vis.max_size();
                    if(next_size>max_size)next_size=max_size;
                    mex_vis.resize((size_t)next_size,false);
                }
                mex_vis[(size_t)t.task_id]=true;
                while(mex_cur<(uint64_t)mex_vis.size()&&mex_vis[(size_t)mex_cur])mex_cur++;
                // 进度显示: 每 100 task (对齐原版)
                uint64_t td=++tasks_done;
                if(td%100==0){
                    auto now=std::chrono::steady_clock::now();
                    double sec=std::chrono::duration<double>(now-t_start).count();
                    uint64_t done_num=saturating_mul_u64(td,CHUNK_SIZE);
                    long double tmlft_d=(ALL_totnum>done_num&&done_num>0)?((long double)(ALL_totnum-done_num)/(long double)done_num)*(long double)sec:0.0L;
                    uint64_t tmlft=tmlft_d>(long double)std::numeric_limits<uint64_t>::max()?std::numeric_limits<uint64_t>::max():(uint64_t)tmlft_d;
                    // 原版第一行: taskX finished, task_mex=Y, count:Z.ZZZT
                    fprintf(flog,"task%llu finished,task_mex=%llu,count:%.6lfT\n",
                        (unsigned long long)t.task_id,(unsigned long long)mex_cur,(double)done_num/1e12);
                    // 原版第二行: tot=N, (八围,XP,XD), time: Xs, speed: XT/d, time left:XhXmXs
                    fprintf(fspeed,"tot=%d, (%d,%d,%d),time: %.2fs, speed: %.6fT/d,time left:%lluh%llum%llus\n",
                        total_found.load(),max_sum,max_xp,max_xd,sec,
                        (double)td/sec*86400.0*(double)CHUNK_SIZE/1e12,
                        (unsigned long long)(tmlft/3600),(unsigned long long)((tmlft%3600)/60),(unsigned long long)(tmlft%60));
                    fflush(output_log?stderr:flog);
                    fflush(output_speed?stderr:fspeed);
                }
            }
            local_found=0;local_max_sum=local_max_xp=local_max_xd=0;
        }};

    // 启动线程
    DBG("creating 1 producer + %d consumers...",n_threads);
    std::thread pt(prod);std::vector<std::thread>cts;
    for(int i=0;i<n_threads;i++)cts.emplace_back(cons,i);
    DBG("all threads running");
    pt.join();for(auto&t:cts)t.join();
    DBG("all threads joined");

    // ===== 权威摘要 (问题4/5修复, 2026-06-01) =====
    // 引擎是唯一真相源: max_sum/max_xp/max_xd 追踪通过 V 值/技能检查的名字
    // (不止达标的——包含检查通过但未达输出阈值的名字), speed 是纯算力吞吐。
    // Python 直接采信此行, 不再从文件重算 max、不再用墙钟反算 speed。
    // 格式: SUMMARY max_sum=.. max_xp=.. max_xd=.. found=.. count=.. elapsed=.. speed=..
    //   count=已处理名字总数, speed=名字/秒 (纯计算)
    {
        auto t_end=std::chrono::steady_clock::now();
        double calc_sec=std::chrono::duration<double>(t_end-t_start).count();
        unsigned long long processed=ALL_totnum;  // 总名字数 (= (rR-rL)*np)
        double speed=calc_sec>0?(double)processed/calc_sec:0.0;
        fprintf(stderr,"SUMMARY max_sum=%d max_xp=%d max_xd=%d found=%d count=%llu elapsed=%.6f speed=%.6f\n",
            max_sum,max_xp,max_xd,total_found.load(),processed,calc_sec,speed);
        fflush(stderr);
    }

    // 关闭文件
    fclose(fp);if(fp_blue)fclose(fp_blue);
    if(!output_log&&flog!=stderr)fclose(flog);
    if(!output_speed&&fspeed!=stderr)fclose(fspeed);
    return 0;
}
