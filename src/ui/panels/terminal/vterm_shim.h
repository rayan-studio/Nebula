#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

void* vt_new_shim(int rows, int cols);
void vt_free_shim(void* vt);
void vt_set_utf8_shim(void* vt, int utf8);
void* vt_obtain_screen_shim(void* vt);
void vt_screen_reset_shim(void* screen, int val);
void vt_screen_enable_altscreen_shim(void* screen, int val);
void vt_screen_set_damage_merge_shim(void* screen, int merge);
void vt_set_size_shim(void* vt, int rows, int cols);
size_t vt_input_write_shim(void* vt, const char* bytes, size_t len);
void vt_screen_flush_damage_shim(void* screen);
int vt_get_cell_codepoint_shim(void* screen, int row, int col, uint32_t* out_cp);

#ifdef __cplusplus
}
#endif
