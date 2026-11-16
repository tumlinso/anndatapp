#pragma once
#include <cstdint>
extern "C" void phase_enter(int); extern "C" void phase_leave(int); extern "C" void phase_piece(unsigned long, int);
struct Phase { int id; explicit Phase(int n):id(n){phase_enter(id);} ~Phase(){phase_leave(id);} };
