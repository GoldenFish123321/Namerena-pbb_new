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

// ===== 候选交错宽度 (Issue #17 扩展实验) =====
// 2=双路(默认) 3=三路交错 4=四路交错
// ARM Cortex-A55 (in-order): 四路寄存器压力过大 → spill → 退化, 二路最优 (+12.7%)
// x86-64 (out-of-order): 四路可充分利用 ROB 深度
#ifdef __aarch64__
#define PAIR_WIDTH 2
#else
#define PAIR_WIDTH 4
#endif
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
static bool q_done(TaskQueue& q){std::unique_lock lk(q.mtx);return q.closed&&q.h>=q.t;}

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

    // Per-prefix ranges (可选: 给每个前缀分配独立的枚举区间)
    // prefix_range_L / prefix_range_R 是 CSV 字符串, 与 prefixes 顺序一一对应
    // 不存在时回退到全局 range_L / range_R
    std::vector<uint64_t> prefix_L, prefix_R;
    bool has_prefix_ranges = false;
    if(kv.count("prefix_range_L") && kv.count("prefix_range_R")){
        auto parts_L = split_csv(kv["prefix_range_L"]);
        auto parts_R = split_csv(kv["prefix_range_R"]);
        for(size_t i=0;i<parts_L.size()&&i<parts_R.size();i++){
            prefix_L.push_back(std::stoull(parts_L[i]));
            prefix_R.push_back(std::stoull(parts_R[i]));
        }
        has_prefix_ranges = !prefix_L.empty();
        if(has_prefix_ranges && (prefix_L.size()!=(size_t)np || prefix_R.size()!=(size_t)np)){
            fprintf(stderr,"[engine] ERROR: prefix ranges count must match prefixes count\n");
            return 1;
        }
        for(size_t i=0;i<prefix_L.size();i++){
            if(prefix_R[i]<prefix_L[i]){
                fprintf(stderr,"[engine] ERROR: prefix range end must be >= start\n");
                return 1;
            }
        }
    }

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
    uint64_t ALL_totnum;
    if(has_prefix_ranges){
        ALL_totnum = 0;
        uint64_t total_chunks = 0;
        if(mode==1){
            for(int j=0;j<np;j++){
                uint64_t add=prefix_R[j]-prefix_L[j];
                uint64_t next=ALL_totnum+add;
                ALL_totnum = next<ALL_totnum ? std::numeric_limits<uint64_t>::max() : next;
            }
        } else {
            for(int j=0;j<np;j++){
                uint64_t add=ceil_div_u64(prefix_R[j]-prefix_L[j],CHUNK_SIZE);
                uint64_t next=total_chunks+add;
                total_chunks = next<total_chunks ? std::numeric_limits<uint64_t>::max() : next;
            }
            ALL_totnum = saturating_mul_u64(total_chunks,CHUNK_SIZE);
        }
    } else {
        ALL_totnum = saturating_mul_u64(range_len,(uint64_t)np);
        if(mode>=2) ALL_totnum = range_len;
        if(mode==2||mode==4) ALL_totnum = round_up_to_chunk_u64(range_len);
    }
    auto t_start=std::chrono::steady_clock::now();
    // cur_speed: 定期速度采样 (每 1000 task ≈ 1B 名字), 对齐原版 pbb_all.cpp
    auto t_cur_last = t_start;
    int td_cur_last = 0;
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
    uint64_t est_tasks_u;
    if(has_prefix_ranges){
        est_tasks_u = 0;
        for(int j=0;j<np;j++){
            uint64_t add=ceil_div_u64(prefix_R[j]-prefix_L[j],CHUNK_SIZE);
            uint64_t next=est_tasks_u+add;
            est_tasks_u = next<est_tasks_u ? std::numeric_limits<uint64_t>::max() : next;
        }
    } else {
        est_tasks_u = (mode == 1 ? saturating_mul_u64(range_chunks,(uint64_t)np) : range_chunks);
    }
    // 安全上限: 10M 条已覆盖任何实际运行场景, 防止超大 range 或 uint64 溢出 (mex_vis 支持动态扩容)
    if (est_tasks_u > 10000000ULL) est_tasks_u = 10000000ULL;
    int est_tasks = (int)(est_tasks_u * 1.2) + 10;
    mex_vis.resize(est_tasks,false);

    // ---- task_id 序号生成器 ----
    uint64_t task_id_counter=0;

    // ---- Producer: 生成任务 ----
    // 每个 chunk 的随机决策 (prefix/suffix 选择) 由其 chunk_seed 派生,
    // 不再共享 rng_global, 确保与执行顺序/线程数无关。
    // 有 prefix_ranges: 每个前缀在自己的 [prefix_L[j], prefix_R[j]) 区间内生成 chunk.
    // mode 1: 前缀固定 (j), suffix 按 index 轮转
    // mode 2/3/4: 前缀固定 (j, 由 per-prefix range 决定), suffix 随机 (mode 4 配对)
    auto prod=[&]{
        auto add_task=[&](uint64_t L,uint64_t R,int type,int prefix_id,int suffix_id){
            TaskData t;t.L=L;t.R=R;t.type=type;t.prefix_id=prefix_id;t.suffix_id=suffix_id;
            t.task_id=task_id_counter++;t.chunk_seed=derive_chunk_seed(g_seed,t.task_id);q_add(q,t);
        };
        auto add_mode_task=[&](uint64_t L,uint64_t R,int prefix_id){
            TaskData t;t.L=L;t.R=R;t.type=mode;t.prefix_id=prefix_id;
            t.task_id=task_id_counter++;t.chunk_seed=derive_chunk_seed(g_seed,t.task_id);
            if(mode==4) t.suffix_id=prefix_id;
            else { std::mt19937_64 prng(t.chunk_seed); t.suffix_id=prng()%ns+1; }
            q_add(q,t);
        };
        if(has_prefix_ranges){
            for(int j=0;j<np;j++)for(uint64_t i=prefix_L[j];i<prefix_R[j];){
                uint64_t tr=chunk_end_u64(i,prefix_R[j]);
                if(mode==1) add_task(i,tr,1,j+1,(j%ns)+1);
                else add_mode_task(i,tr,j+1);
                if(prefix_R[j]-i<=CHUNK_SIZE)break;
                i+=CHUNK_SIZE;
            }
        } else if(mode==1)for(int j=0;j<np;j++)for(uint64_t i=rL;i<rR;){
            uint64_t tr=chunk_end_u64(i,rR);
            add_task(i,tr,1,j+1,(j%ns)+1);
            if(rR-i<=CHUNK_SIZE)break;
            i+=CHUNK_SIZE;
        } else for(uint64_t i=rL;i<rR;){
            uint64_t tr=chunk_end_u64(i,rR);
            TaskData t;t.L=i;t.R=tr;t.type=mode;
            t.task_id=task_id_counter++;t.chunk_seed=derive_chunk_seed(g_seed,t.task_id);
            std::mt19937_64 prng(t.chunk_seed);
            t.prefix_id=prng()%np+1;t.suffix_id=mode==4?t.prefix_id:prng()%ns+1;
            q_add(q,t);
            if(rR-i<=CHUNK_SIZE)break;
            i+=CHUNK_SIZE;
        }
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

    // 动态线程数: worker id >= target_threads 时该线程睡眠, 不取任务
    std::atomic<int> target_threads{n_threads};

    auto cons=[&](int cid){(void)cid;
        Name name_a,name_b;
#if PAIR_WIDTH >= 3
        Name name_c;
#endif
#if PAIR_WIDTH >= 4
        Name name_d;
#endif
        memcpy(name_a.val_base,name_init.val_base,sizeof(name_a.val_base));
        memcpy(name_b.val_base,name_init.val_base,sizeof(name_b.val_base));
#if PAIR_WIDTH >= 3
        memcpy(name_c.val_base,name_init.val_base,sizeof(name_c.val_base));
#endif
#if PAIR_WIDTH >= 4
        memcpy(name_d.val_base,name_init.val_base,sizeof(name_d.val_base));
#endif
        char buf_b[512];
#if PAIR_WIDTH >= 3
        char buf_c[512];
#endif
#if PAIR_WIDTH >= 4
        char buf_d[512];
#endif
        TaskData t;
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

        // ---- 编码 helper: 顺序进位 — 多候选交错 KSA (Issue #17 扩展) ----
#if PAIR_WIDTH == 2
        auto consume_seq=[&](char* buf_a,int nlen,Name& na,char* buf_b,Name& nb,
                              int epre,int evar,uint64_t L,uint64_t R){
            for(uint64_t i=L;i+1<R;i+=2){
                uint64_t now=i;
                for(int pos=epre+evar*scl-scl;pos>=epre;pos-=scl){int ci=now%clen;ENC(buf_a+pos,ci);now/=clen;}
                now=i+1;
                for(int pos=epre+evar*scl-scl;pos>=epre;pos-=scl){int ci=now%clen;ENC(buf_b+pos,ci);now/=clen;}
                na.load_name_pair(buf_a,buf_b,nlen,nb);
                process_one(buf_a,nlen,score_full(buf_a,nlen,na));
                process_one(buf_b,nlen,score_full(buf_b,nlen,nb));
            }
            if((R-L)%2==1){
                uint64_t i=R-1,now=i;
                for(int pos=epre+evar*scl-scl;pos>=epre;pos-=scl){int ci=now%clen;ENC(buf_a+pos,ci);now/=clen;}
                process_one(buf_a,nlen,score_full(buf_a,nlen,na));
            }
        };
#elif PAIR_WIDTH == 3
        auto consume_seq=[&](char* a,int nlen,Name& na,char* b,Name& nb,char* c,Name& nc,
                              int epre,int evar,uint64_t L,uint64_t R){
            for(uint64_t i=L;i+2<R;i+=3){
                uint64_t now;
                now=i;for(int p=epre+evar*scl-scl;p>=epre;p-=scl){int ci=now%clen;ENC(a+p,ci);now/=clen;}
                now=i+1;for(int p=epre+evar*scl-scl;p>=epre;p-=scl){int ci=now%clen;ENC(b+p,ci);now/=clen;}
                now=i+2;for(int p=epre+evar*scl-scl;p>=epre;p-=scl){int ci=now%clen;ENC(c+p,ci);now/=clen;}
                na.load_name_triple(a,b,c,nlen,nb,nc);
                process_one(a,nlen,score_full(a,nlen,na));
                process_one(b,nlen,score_full(b,nlen,nb));
                process_one(c,nlen,score_full(c,nlen,nc));
            }
            // 尾块: fallback 到单路 load_name — 重置 _ksa_done 强制全 KSA
            for(uint64_t i=L+((R-L)/3)*3;i<R;i++){
                uint64_t now=i;
                for(int p=epre+evar*scl-scl;p>=epre;p-=scl){int ci=now%clen;ENC(a+p,ci);now/=clen;}
                na._ksa_done = false; na.load_name(a,nlen);
                process_one(a,nlen,score_full(a,nlen,na));
            }
        };
#elif PAIR_WIDTH == 4
        auto consume_seq=[&](char* a,int nlen,Name& na,char* b,Name& nb,char* c,Name& nc,char* d,Name& nd,
                              int epre,int evar,uint64_t L,uint64_t R){
            for(uint64_t i=L;i+3<R;i+=4){
                uint64_t now;
                now=i;for(int p=epre+evar*scl-scl;p>=epre;p-=scl){int ci=now%clen;ENC(a+p,ci);now/=clen;}
                now=i+1;for(int p=epre+evar*scl-scl;p>=epre;p-=scl){int ci=now%clen;ENC(b+p,ci);now/=clen;}
                now=i+2;for(int p=epre+evar*scl-scl;p>=epre;p-=scl){int ci=now%clen;ENC(c+p,ci);now/=clen;}
                now=i+3;for(int p=epre+evar*scl-scl;p>=epre;p-=scl){int ci=now%clen;ENC(d+p,ci);now/=clen;}
                na.load_name_quad(a,b,c,d,nlen,nb,nc,nd);
                process_one(a,nlen,score_full(a,nlen,na));
                process_one(b,nlen,score_full(b,nlen,nb));
                process_one(c,nlen,score_full(c,nlen,nc));
                process_one(d,nlen,score_full(d,nlen,nd));
            }
            for(uint64_t i=L+((R-L)/4)*4;i<R;i++){
                uint64_t now=i;
                for(int p=epre+evar*scl-scl;p>=epre;p-=scl){int ci=now%clen;ENC(a+p,ci);now/=clen;}
                na._ksa_done = false; na.load_name(a,nlen);
                process_one(a,nlen,score_full(a,nlen,na));
            }
        };
#endif

        // ---- 编码 helper: 随机逐位 — 多候选交错 KSA (Issue #17 扩展) ----
#if PAIR_WIDTH == 2
        auto consume_rand=[&](char* buf_a,int nlen,Name& na,char* buf_b,Name& nb,
                               int epre,int evar,uint64_t L,uint64_t R,std::mt19937_64& rng){
            for(uint64_t i=L;i+1<R;i+=2){
                for(int pos=epre+evar*scl-scl;pos>=epre;pos-=scl){int ci=rng()%clen;ENC(buf_a+pos,ci);}
                for(int pos=epre+evar*scl-scl;pos>=epre;pos-=scl){int ci=rng()%clen;ENC(buf_b+pos,ci);}
                na.load_name_pair(buf_a,buf_b,nlen,nb);
                process_one(buf_a,nlen,score_full(buf_a,nlen,na));
                process_one(buf_b,nlen,score_full(buf_b,nlen,nb));
            }
            if((R-L)%2==1){
                uint64_t i=R-1;
                for(int pos=epre+evar*scl-scl;pos>=epre;pos-=scl){int ci=rng()%clen;ENC(buf_a+pos,ci);}
                process_one(buf_a,nlen,score_full(buf_a,nlen,na));
            }
        };
#elif PAIR_WIDTH == 3
        auto consume_rand=[&](char* a,int nlen,Name& na,char* b,Name& nb,char* c,Name& nc,
                               int epre,int evar,uint64_t L,uint64_t R,std::mt19937_64& rng){
            for(uint64_t i=L;i+2<R;i+=3){
                for(int p=epre+evar*scl-scl;p>=epre;p-=scl){int ci=rng()%clen;ENC(a+p,ci);}
                for(int p=epre+evar*scl-scl;p>=epre;p-=scl){int ci=rng()%clen;ENC(b+p,ci);}
                for(int p=epre+evar*scl-scl;p>=epre;p-=scl){int ci=rng()%clen;ENC(c+p,ci);}
                na.load_name_triple(a,b,c,nlen,nb,nc);
                process_one(a,nlen,score_full(a,nlen,na));
                process_one(b,nlen,score_full(b,nlen,nb));
                process_one(c,nlen,score_full(c,nlen,nc));
            }
            for(uint64_t i=L+((R-L)/3)*3;i<R;i++){
                for(int p=epre+evar*scl-scl;p>=epre;p-=scl){int ci=rng()%clen;ENC(a+p,ci);}
                na._ksa_done = false; na.load_name(a,nlen);
                process_one(a,nlen,score_full(a,nlen,na));
            }
        };
#elif PAIR_WIDTH == 4
        auto consume_rand=[&](char* a,int nlen,Name& na,char* b,Name& nb,char* c,Name& nc,char* d,Name& nd,
                               int epre,int evar,uint64_t L,uint64_t R,std::mt19937_64& rng){
            for(uint64_t i=L;i+3<R;i+=4){
                for(int p=epre+evar*scl-scl;p>=epre;p-=scl){int ci=rng()%clen;ENC(a+p,ci);}
                for(int p=epre+evar*scl-scl;p>=epre;p-=scl){int ci=rng()%clen;ENC(b+p,ci);}
                for(int p=epre+evar*scl-scl;p>=epre;p-=scl){int ci=rng()%clen;ENC(c+p,ci);}
                for(int p=epre+evar*scl-scl;p>=epre;p-=scl){int ci=rng()%clen;ENC(d+p,ci);}
                na.load_name_quad(a,b,c,d,nlen,nb,nc,nd);
                process_one(a,nlen,score_full(a,nlen,na));
                process_one(b,nlen,score_full(b,nlen,nb));
                process_one(c,nlen,score_full(c,nlen,nc));
                process_one(d,nlen,score_full(d,nlen,nd));
            }
            for(uint64_t i=L+((R-L)/4)*4;i<R;i++){
                for(int p=epre+evar*scl-scl;p>=epre;p-=scl){int ci=rng()%clen;ENC(a+p,ci);}
                na._ksa_done = false; na.load_name(a,nlen);
                process_one(a,nlen,score_full(a,nlen,na));
            }
        };
#endif

        // ---- mode 1: 顺序区间 — 多候选交错 KSA (Issue #17 扩展) ----
#if PAIR_WIDTH == 2
        auto consume_mode1=[&](char* buf_a,int nlen,Name& na,char* buf_b,Name& nb,int plen,int vlen,uint64_t L,uint64_t R){
            na.PRELEN=plen;na.load_prefix(buf_a,nlen);
            nb.PRELEN=plen;nb.load_prefix(buf_a,nlen);
            uint8_t dl[16],dr[16];uint64_t now;
            now=L;for(int d=vlen-1;d>=0;d--){dl[d]=now%clen;now/=clen;}
            now=R-1;for(int d=vlen-1;d>=0;d--){dr[d]=now%clen;now/=clen;}
            int up=0;while(up<vlen&&dl[up]==dr[up])up++;
            now=L;for(int d=vlen-1;d>=0;d--){int ci=now%clen;ENC(buf_a+plen+d*scl,ci);now/=clen;}
            now=L+1;for(int d=vlen-1;d>=0;d--){int ci=now%clen;ENC(buf_b+plen+d*scl,ci);now/=clen;}
            int epre=plen+up*scl,evar=vlen-up;
            consume_seq(buf_a,nlen,na,buf_b,nb,epre,evar,L,R);
        };
#elif PAIR_WIDTH == 3
        auto consume_mode1=[&](char* a,int nlen,Name& na,char* b,Name& nb,char* c,Name& nc,int plen,int vlen,uint64_t L,uint64_t R){
            na.PRELEN=plen;na.load_prefix(a,nlen);
            nb.PRELEN=plen;nb.load_prefix(a,nlen);
            nc.PRELEN=plen;nc.load_prefix(a,nlen);
            uint8_t dl[16],dr[16];uint64_t now;
            now=L;for(int d=vlen-1;d>=0;d--){dl[d]=now%clen;now/=clen;}
            now=R-1;for(int d=vlen-1;d>=0;d--){dr[d]=now%clen;now/=clen;}
            int up=0;while(up<vlen&&dl[up]==dr[up])up++;
            now=L;for(int d=vlen-1;d>=0;d--){int ci=now%clen;ENC(a+plen+d*scl,ci);now/=clen;}
            now=L+1;for(int d=vlen-1;d>=0;d--){int ci=now%clen;ENC(b+plen+d*scl,ci);now/=clen;}
            now=L+2;for(int d=vlen-1;d>=0;d--){int ci=now%clen;ENC(c+plen+d*scl,ci);now/=clen;}
            int epre=plen+up*scl,evar=vlen-up;
            consume_seq(a,nlen,na,b,nb,c,nc,epre,evar,L,R);
        };
#elif PAIR_WIDTH == 4
        auto consume_mode1=[&](char* a,int nlen,Name& na,char* b,Name& nb,char* c,Name& nc,char* buf_d,Name& nd,int plen,int vlen,uint64_t L,uint64_t R){
            na.PRELEN=plen;na.load_prefix(a,nlen);
            nb.PRELEN=plen;nb.load_prefix(a,nlen);
            nc.PRELEN=plen;nc.load_prefix(a,nlen);
            nd.PRELEN=plen;nd.load_prefix(a,nlen);
            uint8_t dl[16],dr[16];uint64_t now;
            now=L;for(int d=vlen-1;d>=0;d--){dl[d]=now%clen;now/=clen;}
            now=R-1;for(int d=vlen-1;d>=0;d--){dr[d]=now%clen;now/=clen;}
            int up=0;while(up<vlen&&dl[up]==dr[up])up++;
            now=L;for(int d=vlen-1;d>=0;d--){int ci=now%clen;ENC(a+plen+d*scl,ci);now/=clen;}
            now=L+1;for(int d=vlen-1;d>=0;d--){int ci=now%clen;ENC(b+plen+d*scl,ci);now/=clen;}
            now=L+2;for(int d=vlen-1;d>=0;d--){int ci=now%clen;ENC(c+plen+d*scl,ci);now/=clen;}
            now=L+3;for(int d=vlen-1;d>=0;d--){int ci=now%clen;ENC(buf_d+plen+d*scl,ci);now/=clen;}
            int epre=plen+up*scl,evar=vlen-up;
            consume_seq(a,nlen,na,b,nb,c,nc,buf_d,nd,epre,evar,L,R);
        };
#endif

        // ---- mode 2/4: 随机额外字符 + 随机区间 — 多候选交错 KSA (Issue #17 扩展) ----
#if PAIR_WIDTH == 2
        auto consume_mode24=[&](char* buf_a,int nlen,Name& na,char* buf_b,Name& nb,
                                 int plen,uint64_t& L,uint64_t& R,std::mt19937_64& rng){
            int extra=vlen-varlen_task;
            if(extra>0)for(int pos=plen;pos<plen+extra*scl;pos+=scl){int ci=rng()%clen;ENC(buf_a+pos,ci);}
            memcpy(buf_b,buf_a,plen+extra*scl);
            int epre=plen+extra*scl,evar=varlen_task;
            na.PRELEN=epre;na.load_prefix(buf_a,nlen);
            nb.PRELEN=epre;nb.load_prefix(buf_b,nlen);
            L=rng()%random_range_max;R=L+CHUNK_SIZE;
            consume_seq(buf_a,nlen,na,buf_b,nb,epre,evar,L,R);
        };
#elif PAIR_WIDTH == 3
        auto consume_mode24=[&](char* a,int nlen,Name& na,char* b,Name& nb,char* c,Name& nc,
                                 int plen,uint64_t& L,uint64_t& R,std::mt19937_64& rng){
            int extra=vlen-varlen_task;
            if(extra>0)for(int pos=plen;pos<plen+extra*scl;pos+=scl){int ci=rng()%clen;ENC(a+pos,ci);}
            memcpy(b,a,plen+extra*scl);memcpy(c,a,plen+extra*scl);
            int epre=plen+extra*scl,evar=varlen_task;
            na.PRELEN=epre;na.load_prefix(a,nlen);
            nb.PRELEN=epre;nb.load_prefix(b,nlen);
            nc.PRELEN=epre;nc.load_prefix(c,nlen);
            L=rng()%random_range_max;R=L+CHUNK_SIZE;
            consume_seq(a,nlen,na,b,nb,c,nc,epre,evar,L,R);
        };
#elif PAIR_WIDTH == 4
        auto consume_mode24=[&](char* a,int nlen,Name& na,char* b,Name& nb,char* c,Name& nc,char* d,Name& nd,
                                 int plen,uint64_t& L,uint64_t& R,std::mt19937_64& rng){
            int extra=vlen-varlen_task;
            if(extra>0)for(int pos=plen;pos<plen+extra*scl;pos+=scl){int ci=rng()%clen;ENC(a+pos,ci);}
            memcpy(b,a,plen+extra*scl);memcpy(c,a,plen+extra*scl);memcpy(d,a,plen+extra*scl);
            int epre=plen+extra*scl,evar=varlen_task;
            na.PRELEN=epre;na.load_prefix(a,nlen);
            nb.PRELEN=epre;nb.load_prefix(b,nlen);
            nc.PRELEN=epre;nc.load_prefix(c,nlen);
            nd.PRELEN=epre;nd.load_prefix(d,nlen);
            L=rng()%random_range_max;R=L+CHUNK_SIZE;
            consume_seq(a,nlen,na,b,nb,c,nc,d,nd,epre,evar,L,R);
        };
#endif

        // ---- mode 3: 随机额外字符 + 随机逐位 — 多候选交错 KSA (Issue #17 扩展) ----
#if PAIR_WIDTH == 2
        auto consume_mode3=[&](char* buf_a,int nlen,Name& na,char* buf_b,Name& nb,
                                int plen,uint64_t L,uint64_t R,std::mt19937_64& rng){
            int extra=vlen-varlen_task;
            if(extra>0)for(int pos=plen;pos<plen+extra*scl;pos+=scl){int ci=rng()%clen;ENC(buf_a+pos,ci);}
            memcpy(buf_b,buf_a,plen+extra*scl);
            int epre=plen+extra*scl,evar=varlen_task;
            na.PRELEN=epre;na.load_prefix(buf_a,nlen);
            nb.PRELEN=epre;nb.load_prefix(buf_b,nlen);
            consume_rand(buf_a,nlen,na,buf_b,nb,epre,evar,L,R,rng);
        };
#elif PAIR_WIDTH == 3
        auto consume_mode3=[&](char* a,int nlen,Name& na,char* b,Name& nb,char* c,Name& nc,
                                int plen,uint64_t L,uint64_t R,std::mt19937_64& rng){
            int extra=vlen-varlen_task;
            if(extra>0)for(int pos=plen;pos<plen+extra*scl;pos+=scl){int ci=rng()%clen;ENC(a+pos,ci);}
            memcpy(b,a,plen+extra*scl);memcpy(c,a,plen+extra*scl);
            int epre=plen+extra*scl,evar=varlen_task;
            na.PRELEN=epre;na.load_prefix(a,nlen);
            nb.PRELEN=epre;nb.load_prefix(b,nlen);
            nc.PRELEN=epre;nc.load_prefix(c,nlen);
            consume_rand(a,nlen,na,b,nb,c,nc,epre,evar,L,R,rng);
        };
#elif PAIR_WIDTH == 4
        auto consume_mode3=[&](char* a,int nlen,Name& na,char* b,Name& nb,char* c,Name& nc,char* d,Name& nd,
                                int plen,uint64_t L,uint64_t R,std::mt19937_64& rng){
            int extra=vlen-varlen_task;
            if(extra>0)for(int pos=plen;pos<plen+extra*scl;pos+=scl){int ci=rng()%clen;ENC(a+pos,ci);}
            memcpy(b,a,plen+extra*scl);memcpy(c,a,plen+extra*scl);memcpy(d,a,plen+extra*scl);
            int epre=plen+extra*scl,evar=varlen_task;
            na.PRELEN=epre;na.load_prefix(a,nlen);
            nb.PRELEN=epre;nb.load_prefix(b,nlen);
            nc.PRELEN=epre;nc.load_prefix(c,nlen);
            nd.PRELEN=epre;nd.load_prefix(d,nlen);
            consume_rand(a,nlen,na,b,nb,c,nc,d,nd,epre,evar,L,R,rng);
        };
#endif

        while(true){
            // 动态线程数: 如果当前线程 id >= target_threads, 睡眠等待
            if(cid >= target_threads.load(std::memory_order_relaxed)){
                if(q_done(q)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            if(!q_get(q,t)) break;
            // 每个 chunk 用自己的确定性种子重建 rng (与执行环境无关)
            std::mt19937_64 rng(t.chunk_seed);
            // 构建名字缓冲区: prefix + 占位 + suffix
            const std::string&p=prefixes[(t.prefix_id-1)%np];const std::string&s=suffixes[(t.suffix_id-1)%ns];
            int plen=(int)p.size(),slen=(int)s.size(),nlen=plen+vlen*scl+slen;
            char buf[512];memcpy(buf,p.data(),plen);memset(buf+plen,0,vlen*scl);
            if(slen)memcpy(buf+plen+vlen*scl,s.data(),slen);
            buf[nlen]=0;  // 修复: load_prefix/load_name 循环第一轮读 name[nlen], 与 pbb_all.cpp 全局零初始化对齐
            uint64_t L=t.L,R=t.R;

            // 模式分发: 各模式独立的编码逻辑 (多候选交错 KSA, Issue #17 扩展)
#if PAIR_WIDTH == 2
            memcpy(buf_b,buf,nlen+1);
            if(mode==1)      consume_mode1(buf,nlen,name_a,buf_b,name_b,plen,vlen,L,R);
            else if(mode==3) consume_mode3(buf,nlen,name_a,buf_b,name_b,plen,L,R,rng);
            else             consume_mode24(buf,nlen,name_a,buf_b,name_b,plen,L,R,rng);  // mode 2/4
#elif PAIR_WIDTH == 3
            memcpy(buf_b,buf,nlen+1);memcpy(buf_c,buf,nlen+1);
            if(mode==1)      consume_mode1(buf,nlen,name_a,buf_b,name_b,buf_c,name_c,plen,vlen,L,R);
            else if(mode==3) consume_mode3(buf,nlen,name_a,buf_b,name_b,buf_c,name_c,plen,L,R,rng);
            else             consume_mode24(buf,nlen,name_a,buf_b,name_b,buf_c,name_c,plen,L,R,rng);
#elif PAIR_WIDTH == 4
            memcpy(buf_b,buf,nlen+1);memcpy(buf_c,buf,nlen+1);memcpy(buf_d,buf,nlen+1);
            if(mode==1)      consume_mode1(buf,nlen,name_a,buf_b,name_b,buf_c,name_c,buf_d,name_d,plen,vlen,L,R);
            else if(mode==3) consume_mode3(buf,nlen,name_a,buf_b,name_b,buf_c,name_c,buf_d,name_d,plen,L,R,rng);
            else             consume_mode24(buf,nlen,name_a,buf_b,name_b,buf_c,name_c,buf_d,name_d,plen,L,R,rng);
#endif

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
                    // cur_speed: 每 1000 task 输出近期速度 (≈1B 名字), 对齐原版 pbb_all.cpp
                    if(td%1000==0){
                        auto t_cur=std::chrono::steady_clock::now();
                        double cur_sec=std::chrono::duration<double>(t_cur-t_cur_last).count();
                        long long cur_done=(td-td_cur_last)*CHUNK_SIZE;
                        long long cur_tmlft=ALL_totnum>done_num?((ALL_totnum-done_num)*1.0/cur_done)*cur_sec:0;
                        if(cur_sec>0)
                            fprintf(fspeed,"cur_speed:%.6fT/d,time left(?):%lldh%lldm%llds\n\n",
                                86400.0*(cur_done/1e12)/cur_sec,
                                cur_tmlft/3600,(cur_tmlft%3600)/60,cur_tmlft%60);
                        fflush(output_speed?stderr:fspeed);
                        t_cur_last=t_cur;td_cur_last=td;
                    }
                }
            }
            local_found=0;local_max_sum=local_max_xp=local_max_xd=0;
        }};

    // ---- 动态线程数: 通过控制文件 out/.threads 运行时调整 ----
    //   写入整数到 out/.threads 即可实时增减 Worker 线程 (1 ~ n_threads)
    //   excess 线程在每次 chunk 前 sleep 500ms 等待唤醒
    std::atomic<bool> reader_stop{false};
    std::thread reader_thread([&]{
        // Poll control file every second (轻量, 不影响性能)
        std::string ctl_path = "out/.threads";
        int last_val = n_threads;
        while(!reader_stop.load(std::memory_order_relaxed)){
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            FILE* f = fopen(ctl_path.c_str(), "r");
            if(!f) continue;
            int val = -1;
            if(fscanf(f, "%d", &val) == 1 && val >= 1 && val <= n_threads && val != last_val){
                target_threads.store(val);
                fprintf(stderr, "\n[engine] threads → %d\n", val);
                fflush(stderr);
                last_val = val;
            }
            fclose(f);
        }
    });

    // 启动线程
    DBG("creating 1 producer + %d consumers...",n_threads);
    std::thread pt(prod);std::vector<std::thread>cts;
    for(int i=0;i<n_threads;i++)cts.emplace_back(cons,i);
    DBG("all threads running");
    pt.join();for(auto&t:cts)t.join();
    reader_stop.store(true);
    if(reader_thread.joinable())reader_thread.join();
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
