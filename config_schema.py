"""
PBB 配置字段定义 — config → engine 键名映射的唯一真相源.

新增字段只在 CONFIG_MAP 加一行，所有 Python 层自动同步：
  - main.py: _validate_config() + task_config 组装
  - engine.py: _build_params() 默认值 + engine.hpp 键名文档

engine.hpp 键名 = CONFIG_MAP 的 engine_key 列，也在此处核验。
"""
from __future__ import annotations
from dataclasses import dataclass
from typing import Any

# ══════════════════════════════════════════════════════════════════════════════
# 单字段定义
# ══════════════════════════════════════════════════════════════════════════════


@dataclass(frozen=True)
class FieldDef:
    """一个配置字段的全程定义.

    config_path  →  engine_key  →  engine.hpp kv["engine_key"]
    """

    config_path: str  # dot-separated config 路径，如 "threads.worker_threads"
    engine_key: str  # engine 层键名 (task_config / stdin / engine.hpp 统一)
    default: Any  # 默认值
    required: bool = False  # True=缺少时 fatal error

    @property
    def config_section(self) -> str:
        """顶级 section key (第一段)."""
        return self.config_path.split(".")[0]

    @property
    def config_leaf(self) -> str:
        """叶子 key (最后一段)."""
        return self.config_path.rsplit(".", 1)[-1]

    @property
    def config_parent(self) -> str:
        """中间路径 (无叶子)，"" = 顶级."""
        parts = self.config_path.split(".")
        return ".".join(parts[:-1]) if len(parts) > 1 else ""


# ══════════════════════════════════════════════════════════════════════════════
# 所有 config → engine 标量映射 — 唯一定义处
# ============================================================================
#  新增字段只需在此列表加一行:
#    FieldDef("section.config_key", "engine_key", default)
#  main.py 和 engine.py 自动同步，编译器和 engine.hpp 键名一致。
# ══════════════════════════════════════════════════════════════════════════════

CONFIG_MAP: list[FieldDef] = [
    # ── top-level ──
    FieldDef("debug_mode", "debug_mode", 0),
    FieldDef("team_name", "team_name", "", required=True),

    # ── threads ──
    FieldDef("threads.worker_threads", "n_threads", -1),

    # ── enumeration ──
    FieldDef("enumeration.mode", "mode", None, required=True),
    FieldDef("enumeration.variable_length", "variable_len", None, required=True),
    # 注: ranges[0].start / ranges[0].end → range_L / range_R 在 main.py 手工处理
    # (不是简单的标量映射: 需要从列表取 [0] + CLI 优先级)

    # ── collection ──
    FieldDef("collection.xp_min", "xp_min", 0),
    FieldDef("collection.xd_min", "xd_min", 0),
    FieldDef("collection.collect_mode", "collect_mode", 0),

    # ── collection.special_thresholds (collect_mode=2) ──
    FieldDef("collection.special_thresholds.eight_v_min", "c_eight_v_min", 0),
    FieldDef("collection.special_thresholds.seven_v_min", "c_seven_v_min", 0),
    FieldDef("collection.special_thresholds.hl_min", "c_hl_min", 0),
    FieldDef("collection.special_thresholds.hp398_eight_v_min", "c_hp398_min", 0),

    # ── output ──
    FieldDef("output.output_xp", "output_xp", 1),
    FieldDef("output.log_output", "output_log", 1),
    FieldDef("output.speed_output", "output_speed", 1),
    FieldDef("output.result_file", "result_file", "result.txt"),
]

# ══════════════════════════════════════════════════════════════════════════════
# 引擎 stdin 所有键名 — 与 engine.hpp kv[...] 一一对应
# ============================================================================
#  用于 engine.py _build_params() 自检: 生成的每个 key 必须在此集合中.
#  此集合也作为 engine.hpp 键名的可审计清单.
# ══════════════════════════════════════════════════════════════════════════════

# 来自 CONFIG_MAP 的 engine_key
_ENGINE_KEYS_FROM_CONFIG = {fd.engine_key for fd in CONFIG_MAP}

# Python 层计算的 engine 键 (不由 CONFIG_MAP 覆盖)
_ENGINE_KEYS_COMPUTED = {
    # character_set → 引擎
    "scl",
    "charset_len",
    "charset_bytes",
    # prefix/suffix CSV
    "prefixes",
    "suffixes",
    # range 映射 (由 Python 从 range_L/range_R 写)
    "range_L",
    "range_R",
    # per-prefix ranges (CSV, 与 prefixes 顺序一一对应; 不存在时回退到 range_L/R)
    "prefix_range_L",
    "prefix_range_R",
    # 可选 seed
    "seed",
}

# 所有合法 engine 键名 = CONFIG_MAP 内 + Python 计算
ALL_ENGINE_KEYS: set[str] = _ENGINE_KEYS_FROM_CONFIG | _ENGINE_KEYS_COMPUTED


# ── 便捷工具 ──────────────────────────────────────────────────────────────────


def read_config(cfg: dict, fd: FieldDef) -> Any:
    """从配置 dict 按 config_path 读取值，不存在时返回 default."""
    parts = fd.config_path.split(".")
    cur = cfg
    for p in parts[:-1]:
        if not isinstance(cur, dict):
            return fd.default
        cur = cur.get(p, {})
    if not isinstance(cur, dict):
        return fd.default
    return cur.get(parts[-1], fd.default)


def engine_defaults() -> dict[str, Any]:
    """返回所有 engine_key → default 的字典."""
    return {fd.engine_key: fd.default for fd in CONFIG_MAP}


def engine_default(engine_key: str) -> Any:
    """单个 engine_key 的默认值."""
    for fd in CONFIG_MAP:
        if fd.engine_key == engine_key:
            return fd.default
    raise KeyError(f"unknown engine_key: {engine_key}")


# ── 自检 (导入时执行) ──────────────────────────────────────────────────────


def _selfcheck() -> None:
    """启动时自检: 确保 CONFIG_MAP 内部一致."""
    seen_engine = set()
    seen_config = set()
    for fd in CONFIG_MAP:
        # engine_key 唯一
        if fd.engine_key in seen_engine:
            raise AssertionError(
                f"CONFIG_MAP: 重复 engine_key '{fd.engine_key}'"
            )
        seen_engine.add(fd.engine_key)
        # config_path 唯一
        if fd.config_path in seen_config:
            raise AssertionError(
                f"CONFIG_MAP: 重复 config_path '{fd.config_path}'"
            )
        seen_config.add(fd.config_path)


_selfcheck()
