#include "Feather.hpp"

Feather::Feather(now_ms_func) {
    clock = FSTime(now_ms_func);
}