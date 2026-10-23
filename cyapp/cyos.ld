/* CyOS kernel exports the browser uses, by their numbers in the Cybiko
 * SDK's headers (__cyos N). A call of (or a reference to) 0xf00000 + N
 * becomes an import of export N when tools/cybiko_mkapp.py builds the app. */
_cy_current_thread = 0xf00000 + 124;
_cy_cool_bold_font = 0xf00000 + 468;
_cy_cool_normal_font = 0xf00000 + 469;
_cy_mini_bold_font = 0xf00000 + 891;
_cy_mini_normal_font = 0xf00000 + 892;
_cy_malloc = 0xf00000 + 876;
_cy_free = 0xf00000 + 610;
_cy_Message_new = 0xf00000 + 270;
_cy_Message_delete = 0xf00000 + 265;
_cy_Message_post = 0xf00000 + 943;
_cy_Message_deliver = 0xf00000 + 501;
_cy_Message_check_delivery = 0xf00000 + 413;
_cy_Message_get_buffer = 0xf00000 + 640;
_cy_Message_has_buffer = 0xf00000 + 757;
_cy_Message_attach_buffer = 0xf00000 + 373;
_cy_Queue_get_message = 0xf00000 + 687;
_cy_Buffer_ctor = 0xf00000 + 222;
_cy_Buffer_store = 0xf00000 + 1196;
_cy_Buffer_load = 0xf00000 + 851;
_cy_Buffer_get_size = 0xf00000 + 728;
_cy_Buffer_dtor = 0xf00000 + 107;
_cy_Process_get_display = 0xf00000 + 666;
_cy_Process_request_focus = 0xf00000 + 1034;
_cy_TGraph_set_color = 0xf00000 + 1108;
_cy_TGraph_fill_screen = 0xf00000 + 587;
_cy_Graphics_set_font = 0xf00000 + 1126;
_cy_Graphics_draw_text = 0xf00000 + 532;
_cy_Graphics_draw_text_Ex = 0xf00000 + 533;
_cy_Graphics_string_width_Ex = 0xf00000 + 739;
_cy_Font_get_char_width = 0xf00000 + 651;
_cy_Font_get_char_height = 0xf00000 + 649;
_cy_DisplayGraphics_show = 0xf00000 + 1163;
_cy_get_gate_cyid = 0xf00000 + 1340;
_cy_get_own_id = 0xf00000 + 701;
_cy_sprintf = 0xf00000 + 1170;
_cy_strlen = 0xf00000 + 1212;
_cy_Process_ctor = 0xf00000 + 237;      /* (process, name, stack size) */
_cy_Process_dtor = 0xf00000 + 116;
_cy_Thread_start = 0xf00000 + 1175;     /* (thread, 0): runs its vtable[4] */
_cy_Process_has_focus = 0xf00000 + 758;
/* CyWin.dl (library 1) */
_cy_cWinApp_ctor = 0xf10000 + 324;      /* (app, name, stack size) */
