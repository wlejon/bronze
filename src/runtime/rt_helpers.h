#pragma once

#include <cstdint>

namespace bronze::runtime {

extern "C" {

uint64_t bronze_box_f64(double v);
uint64_t bronze_box_i32(int32_t v);
uint64_t bronze_box_bool(bool v);
uint64_t bronze_box_str(const char* s);
uint64_t bronze_box_str_key(uint32_t keyIndex);

double bronze_unbox_f64(uint64_t bits);
int32_t bronze_unbox_i32(uint64_t bits);
bool bronze_unbox_bool(uint64_t bits);

uint64_t bronze_create_object();
uint64_t bronze_create_array(uint32_t length);
uint64_t bronze_create_function(void* code, uint32_t arity);

uint64_t bronze_prop_get(uint64_t objBits, uint32_t keyIndex, uint32_t icIndex);
void bronze_prop_set(uint64_t objBits, uint32_t keyIndex, uint64_t valBits, uint32_t icIndex);
uint64_t bronze_dynamic_call(uint64_t calleeBits, uint64_t thisBits, uint32_t argc, const uint64_t* argvBits);

uint64_t bronze_string_concat(uint64_t aBits, uint64_t bBits);
uint64_t bronze_dynamic_add(uint64_t aBits, uint64_t bBits);
void bronze_print_value(uint64_t valBits);
void bronze_print_string(const char* s);

void bronze_register_key_string(uint32_t index, const char* str);

}

}  // namespace bronze::runtime
