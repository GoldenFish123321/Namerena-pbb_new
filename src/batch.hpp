#pragma once
// ============================================================================
// batch.hpp — C++ 热路径: 编码 + 完整评分 (释放 GIL)
//
// process_batch() 在 C++ 中完成编码、RC4 状态机、完整评分流水线。
// 返回通过阈值的 (name, xp, xd) 列表。
// Python 负责: 阈值判断 + 文件输出。
// ============================================================================
#include "common.hpp"
#include "scoring.hpp"
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

namespace py = pybind11;

struct BatchHit { std::string name; int xp; int xd; int flag3; };

inline py::list process_batch(
    const std::string& template_name,
    const std::string& charset,
    int charset_len,
    int scl,
    int effective_prelen,
    int effective_varlen,
    uint64_t start,
    uint64_t count,
    Name& name_obj)
{
    int name_len = (int)template_name.size();
    std::vector<char> name_buf(name_len);
    memcpy(name_buf.data(), template_name.data(), name_len);

    std::vector<BatchHit> hits;
    hits.reserve(256);

    {
        py::gil_scoped_release release;
        for (uint64_t i = start; i < start + count; i++) {
            uint64_t now = i;
            int pos = effective_prelen + effective_varlen * scl - scl;
            int end_pos = effective_prelen - scl;
            for (; pos > end_pos; pos -= scl) {
                int ci = (int)(now % charset_len);
                memcpy(name_buf.data() + pos, charset.data() + ci * scl, scl);
                now /= charset_len;
            }
            ScoreResult r = score_full(name_buf.data(), name_len, name_obj);
            if (r.flag) {
                hits.push_back({std::string(name_buf.data(), name_len), r.xp, r.xd, r.flag3});
            }
        }
    }

    py::list results;
    for (const auto& h : hits) {
        py::dict entry;
        entry["name"] = py::bytes(h.name);
        entry["xp"] = h.xp;
        entry["xd"] = h.xd;
        entry["flag3"] = h.flag3;
        results.append(entry);
    }
    return results;
}
