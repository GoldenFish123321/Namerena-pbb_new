// bridge.cpp — pybind11 module: exposes character set data to Python.
#include <pybind11/pybind11.h>
#include "charset_data.hpp"
#include "charset.hpp"

namespace py = pybind11;

PYBIND11_MODULE(pbb_core, m) {
    m.doc() = "PBB charset data provider";

    m.def("get_hanzi_bytes", []() {
        return py::bytes(reinterpret_cast<const char*>(hanzi_), sizeof(hanzi_)); });
    m.def("get_xila_bytes", []() {
        return py::bytes(reinterpret_cast<const char*>(xila), sizeof(xila)); });
    m.def("get_XILA_bytes", []() {
        return py::bytes(reinterpret_cast<const char*>(XILA), sizeof(XILA)); });
    m.def("get_ewen_bytes", []() {
        return py::bytes(reinterpret_cast<const char*>(ewen), sizeof(ewen)); });
    m.def("get_EWEN_bytes", []() {
        return py::bytes(reinterpret_cast<const char*>(EWEN), sizeof(EWEN)); });
    m.def("get_lading_bytes", []() {
        return py::bytes(reinterpret_cast<const char*>(lading), sizeof(lading)); });
    m.def("get_LADING_bytes", []() {
        return py::bytes(reinterpret_cast<const char*>(LADING), sizeof(LADING)); });
    m.def("get_pingjia_bytes", []() {
        return py::bytes(reinterpret_cast<const char*>(pingjia), sizeof(pingjia)); });
    m.def("get_pianjia_bytes", []() {
        return py::bytes(reinterpret_cast<const char*>(pianjia), sizeof(pianjia)); });
    m.def("get_mangwen_bytes", []() {
        return py::bytes(reinterpret_cast<const char*>(mangwen), sizeof(mangwen)); });
    m.def("get_extended_hanzi_3_bytes", []() {
        return py::bytes(reinterpret_cast<const char*>(hanzi_3), sizeof(hanzi_3)); });
    m.def("get_extended_hanzi_4_bytes", []() {
        return py::bytes(reinterpret_cast<const char*>(hanzi_4), sizeof(hanzi_4)); });

    m.def("init_exhanzi", &init_exhanzi);
    m.def("load_hanzi", &load_hanzi, py::arg("start"), py::arg("end"));
    m.def("encode_unicode", [](int cp) -> py::bytes {
        char buf[4]; int len = load_unicode_codepoint(cp, buf);
        return py::bytes(buf, len); }, py::arg("codepoint"));

    m.def("get_charset_bytes", []() { return py::bytes(charset, charset_cnt + 1); });
    m.def("get_charset_len", []() { return charset_len; });
    m.def("reset_charset", &reset_charset);
    m.def("load_unicode_range", &load_unicode_range, py::arg("start"), py::arg("end"));
}
