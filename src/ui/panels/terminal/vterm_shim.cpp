#include "vterm_shim.h"

// Include the real libvterm header in this TU only. Avoid bringing in
// Windows headers here to reduce macro collisions.
#include "vterm.h"
#include <stdlib.h>
#include <string.h>

void* vt_new_shim(int rows, int cols)
{
    return (void*)vterm_new(rows, cols);
}

void vt_free_shim(void* vt)
{
    if (!vt) return;
    vterm_free((VTerm*)vt);
}

void vt_set_utf8_shim(void* vt, int utf8)
{
    if (!vt) return;
    vterm_set_utf8((VTerm*)vt, utf8);
}

void* vt_obtain_screen_shim(void* vt)
{
    if (!vt) return NULL;
    return (void*)vterm_obtain_screen((VTerm*)vt);
}

void vt_screen_reset_shim(void* screen, int val)
{
    if (!screen) return;
    vterm_screen_reset((VTermScreen*)screen, val);
}

void vt_screen_enable_altscreen_shim(void* screen, int val)
{
    if (!screen) return;
    vterm_screen_enable_altscreen((VTermScreen*)screen, val);
}

void vt_screen_set_damage_merge_shim(void* screen, int merge)
{
    if (!screen) return;
    vterm_screen_set_damage_merge((VTermScreen*)screen, (VTermDamageSize)merge);
}

void vt_set_size_shim(void* vt, int rows, int cols)
{
    if (!vt) return;
    vterm_set_size((VTerm*)vt, rows, cols);
}

size_t vt_input_write_shim(void* vt, const char* bytes, size_t len)
{
    if (!vt) return 0;
    return vterm_input_write((VTerm*)vt, bytes, len);
}

void vt_screen_flush_damage_shim(void* screen)
{
    if (!screen) return;
    vterm_screen_flush_damage((VTermScreen*)screen);
}

int vt_get_cell_codepoint_shim(void* screen, int row, int col, uint32_t* out_cp)
{
    if (!screen || !out_cp) return 0;
    VTermPos pos; pos.row = row; pos.col = col;
    VTermScreenCell cell;
    int ok = vterm_screen_get_cell((VTermScreen*)screen, pos, &cell);
    if (!ok) return 0;
    *out_cp = cell.chars[0];
    return 1;
}
