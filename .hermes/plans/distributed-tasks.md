# 分布式任务系统 — 设计文档

## 核心设计原则

1. **客户端无状态** — 拿到 task → 跑完 → 交结果 → 取下一个。不存进度、不管断点。
2. **服务端是唯一真相源** — 所有状态、进度、结果在服务端。客户端只做运算。
3. **task 自包含** — 服务端下发完整配置，客户端不读本地文件。
4. **数据不冗余** — Job 存一次配置，Task 只存范围和结果，取任务时动态拼装。
5. **认证极简** — api_key 即身份，无 JWT/过期/刷新。

## 文件结构

```
namerena-pbb_new/
├── server/
│   ├── main.py           # FastAPI 入口 + uvicorn
│   ├── models.py         # SQLite 表 + CRUD
│   ├── routes_api.py     # /api/* (客户端)
│   ├── routes_web.py     # WebUI (Jinja2)
│   ├── task_queue.py     # 拆分 / 分配 / 超时回收
│   ├── templates/        # HTML 模板
│   └── requirements.txt  # fastapi, uvicorn, aiosqlite, jinja2
│
├── client/
│   └── client.py         # CLI: 取任务 → 跑 → 交结果 (循环)
│
├── engine.py             # run_task(config) → results (server/client/单机 共用)
├── main.py               # 单机模式 (不变)
├── build.py              # 编译 (不变)
├── src/                  # C++ (不变)
└── engine_main.cpp       # (不变)
```

## 数据模型

### Job — 配置模板
```
id           TEXT PK    UUID
name         TEXT       管理员命名
team_name    TEXT
prefixes     TEXT       逗号分隔
suffixes     TEXT
charset_hex  TEXT       预构建字符集 hex (服务端计算一次)
scl          INT
vlen         INT
mode         INT
xp_min       INT
xd_min       INT
collect_mode INT        0/1/2
output_xp    INT        0/1
c_8v / c_7v / c_hl / c_hp  INT   collect_mode=2 时
range_start  INT        总范围起始
range_end    INT        总范围结束
task_count   INT        拆分后 task 数
created_at   TEXT
status       TEXT       active / done
```

### Task — 工作单元 (50M 名字/task)
```
id           TEXT PK    UUID
job_id       TEXT FK
range_start  INT        本 task 范围
range_end    INT
status       TEXT       pending / done
client_id    TEXT       执行者 (assigned 时)
assigned_at  TEXT
completed_at TEXT
results      TEXT       JSON [{name, xp, xd}, ...]
max_xp       INT
max_xd       INT
speed        REAL       T/d
```

### Client
```
id           TEXT PK    服务端分配
api_key      TEXT UNIQUE
name         TEXT
last_seen    TEXT
completed    INT        累计完成数
```

## API — 客户端协议

```
POST /api/register
  header: Authorization: Bearer <api_key>
  body:   {name: "worker1"}
  →:      {client_id: "uuid"}

GET  /api/task/next
  header: Authorization: Bearer <api_key>
  →:      {task: {id, 从 Job 拼装的完整配置+range} }  或  null

POST /api/task/{id}/result
  header: Authorization: Bearer <api_key>
  body:   {results: [{name, xp, xd}], max_xp, max_xd, speed}
  →:      {ok: true}
```

> task 配置由服务端从 `Job + Task.range` 动态拼装，不冗余存储。

## 服务端逻辑

### 创建 Job (WebUI)
```
1. 管理员填写: 上传配置文件 + 指定 range [start, end)
2. 服务端解析配置, 计算 charset_hex (调用 pbb_core)
3. 写入 Job 表 (存一份完整配置)
4. 按 TASK_SIZE (50,000,000) 拆分:
   task_count = ceil((end - start) / TASK_SIZE)
   生成 task_count 个 Task, 全部 status=pending
```

### 分发 Task (GET /task/next)
```
1. 验证 api_key → 查找 client
2. 取 age(assigned_at) 最老的 status=pending task
3. → status=assigned, assigned_at=now(), client_id=client
4. 从 Job 表读取配置, 拼装 Task.range → 返回完整 task
```

### 超时回收 (每分钟定时任务)
```
status=assigned 且 now() - assigned_at > 30 分钟
→ status=pending (释放给其他客户端)
```

## WebUI 页面

```
GET  /               Dashboard — Job 卡片 + 客户端列表
GET  /job/{id}       Job 详情 — 进度条 / task 表格 / 结果分页
GET  /create         创建表单 (上传配置 + range)
POST /create         提交 → 自动拆分
```

## 客户端工作流

```
1. 从环境变量或 CLI 参数获取: SERVER_URL, API_KEY, NAME
2. POST /register (一次)
3. 循环:
   GET /task/next
   → null: sleep(10), 回到 3
   → task: engine.run_task(task) → results
            POST /task/{id}/result
            回到 3
```

## engine.py (项目根, 三方共用)

```python
def run_task(config: dict) -> dict:
    """执行单个 task, 返回 {results, max_xp, max_xd, speed}.

    config: {team_name, charset_hex, prefixes, suffixes, scl, vlen,
             mode, range_start, range_end, xp_min, xd_min,
             collect_mode, output_xp, ...}
    """
    # 1. 构建引擎 stdin 参数
    # 2. Popen 启动 pbb_engine
    # 3. 实时转发 stderr (进度日志)
    # 4. proc.wait(), 读输出文件
    # 5. 返回结果
```

## 单机模式兼容

main.py 不变, 调用 engine.run_task():
```
python3 main.py -c config.yaml    # 和之前完全一样
```

## 部署

```bash
# 服务端
pip install fastapi uvicorn aiosqlite jinja2
uvicorn server.main:app --host 0.0.0.0 --port 8080

# 客户端
export SERVER_URL=http://server:8080 API_KEY=xxx NAME=worker1
python3 client/client.py
```
