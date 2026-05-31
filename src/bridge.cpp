// ============================================================================
// bridge.cpp — pybind11 模块入口
//
// 暴露 C++ 核心给 Python: Name 状态机、字符集加载、批量编码+特征提取。
// 模型评分 (hanxu_Poly + MODEL) 由 Python 的 data.py 负责。
// ============================================================================
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/numpy.h>
#include "charset_data.hpp"
#include "charset.hpp"
#include "name.hpp"
#include "scoring.hpp"
#include "batch.hpp"

namespace py = pybind11;

PYBIND11_MODULE(pbb_core, m) {
    m.doc() = "PBB Name Scoring Core — C++ RC4 engine + batch encoding";

    // ===== Character set raw data (exposed for Python charset building) =====
    m.def("get_hanzi_bytes", []() {
        return py::bytes(reinterpret_cast<const char*>(hanzi_), sizeof(hanzi_));
    }, "Common Chinese characters (3500 chars, 3 bytes each)");

    m.def("get_xila_bytes", []() {
        return py::bytes(reinterpret_cast<const char*>(xila), sizeof(xila));
    }, "Lowercase Greek (24 chars, 2 bytes each)");

    m.def("get_XILA_bytes", []() {
        return py::bytes(reinterpret_cast<const char*>(XILA), sizeof(XILA));
    }, "Uppercase Greek (24 chars, 2 bytes each)");

    m.def("get_ewen_bytes", []() {
        return py::bytes(reinterpret_cast<const char*>(ewen), sizeof(ewen));
    }, "Lowercase Russian (33 chars, 2 bytes each)");

    m.def("get_EWEN_bytes", []() {
        return py::bytes(reinterpret_cast<const char*>(EWEN), sizeof(EWEN));
    }, "Uppercase Russian (33 chars, 2 bytes each)");

    m.def("get_lading_bytes", []() {
        return py::bytes(reinterpret_cast<const char*>(lading), sizeof(lading));
    }, "Lowercase Latin (31 chars, 2 bytes each)");

    m.def("get_LADING_bytes", []() {
        return py::bytes(reinterpret_cast<const char*>(LADING), sizeof(LADING));
    }, "Uppercase Latin (31 chars, 2 bytes each)");

    m.def("get_pingjia_bytes", []() {
        return py::bytes(reinterpret_cast<const char*>(pingjia), sizeof(pingjia));
    }, "Japanese Hiragana (74 chars, 3 bytes each)");

    m.def("get_pianjia_bytes", []() {
        return py::bytes(reinterpret_cast<const char*>(pianjia), sizeof(pianjia));
    }, "Japanese Katakana (74 chars, 3 bytes each)");

    m.def("get_mangwen_bytes", []() {
        return py::bytes(reinterpret_cast<const char*>(mangwen), sizeof(mangwen));
    }, "Braille (255 chars, 3 bytes each)");

    m.def("get_extended_hanzi_3_bytes", []() {
        return py::bytes(reinterpret_cast<const char*>(hanzi_3), sizeof(hanzi_3));
    }, "Extended Chinese (3-byte, 6400 chars)");

    m.def("get_extended_hanzi_4_bytes", []() {
        return py::bytes(reinterpret_cast<const char*>(hanzi_4), sizeof(hanzi_4));
    }, "Extended Chinese (4-byte, 64287 chars)");

    // ===== Character set initialization =====
    m.def("init_exhanzi", &init_exhanzi,
          "Initialize extended Chinese character data");

    m.def("load_hanzi", &load_hanzi,
          "Load CJK Unified Ideographs (U+4E00..U+9FFF) into charset buffer",
          py::arg("start"), py::arg("end"));

    m.def("load_unicode_range", &load_unicode_range,
          "Load Unicode codepoint range into charset buffer",
          py::arg("start"), py::arg("end"));

    m.def("encode_unicode", [](int codepoint) -> py::bytes {
        char buf[4];
        int len = load_unicode_codepoint(codepoint, buf);
        return py::bytes(buf, len);
    }, "Encode Unicode codepoint to UTF-8 bytes", py::arg("codepoint"));

    m.def("get_charset_bytes", []() { return py::bytes(charset, charset_cnt + 1); });
    m.def("get_charset_len", []() { return charset_len; });
    m.def("reset_charset", &reset_charset);

    // ===== Name class (RC4 state machine) =====
    py::class_<Name>(m, "Name", py::module_local(),
                    "RC4-based name state machine")
        .def(py::init<>())
        .def("load_team", [](Name& self, const py::bytes& team) {
            std::string s = team; self.load_team(s.c_str());
        }, py::arg("team"))
        .def("load_prefix", [](Name& self, const py::bytes& name, int name_len) {
            std::string s = name; self.load_prefix(s.c_str(), name_len);
        }, py::arg("name"), py::arg("name_len"))
        .def("load_name", [](Name& self, const py::bytes& name) {
            std::string s = name; self.load_name(s.c_str(), s.size());
        }, py::arg("name"))
        .def("loading_name", [](Name& self, const py::bytes& name) {
            std::string s = name; self.loading_name(s.c_str());
        }, py::arg("name"))
        .def("calc_skills", [](Name& self, const py::bytes& name) {
            std::string s = name; self.calc_skills(s.c_str());
        }, py::arg("name"))
        .def_property_readonly("V", [](const Name& self) { return self.V; })
        .def_readwrite("PRELEN", &Name::PRELEN)
        .def_property_readonly("NAMELEN", [](const Name& self) { return self.NAMELEN; });

    // ===== Batch processing (C++ hot path, GIL released) =====
    m.def("process_batch", &process_batch,
          "Encode + RC4 + feature extraction (GIL released). "
          "Returns candidates with raw features for Python scoring.",
          py::arg("template_name"), py::arg("charset"),
          py::arg("charset_len"), py::arg("scl"),
          py::arg("effective_prelen"), py::arg("effective_varlen"),
          py::arg("start"), py::arg("count"),
          py::arg("name_obj"));

    // ===== Score full (kept for individual name scoring / debugging) =====
    m.def("score_full", [](const py::bytes& name, Name& name_obj) -> py::dict {
        std::string s = name;
        ScoreResult r = score_full(s.c_str(), s.size(), name_obj);
        py::dict result;
        result["xp"] = r.xp; result["xd"] = r.xd; result["sum"] = r.sum;
        result["flag"] = r.flag; result["flag3"] = r.flag3; result["shadow"] = r.shadow;
        py::list props, skills, freqs;
        for (int i = 0; i < 8; i++) props.append(r.props[i]);
        for (int i = 0; i < 16; i++) { skills.append(r.skills[i]); freqs.append(r.freqs[i]); }
        result["props"] = props; result["skills"] = skills; result["freqs"] = freqs;
        return result;
    }, "Full scoring (debug): load_name → calc_skills → model scoring");

    // ===== Constants =====
    m.attr("N") = N; m.attr("M") = M; m.attr("K") = K;
    m.attr("SKILL_CNT") = skill_cnt;
    m.attr("HAS_AVX2") = (bool)PBB_HAS_AVX2;
    m.attr("__version__") = "2.0.0";
}
