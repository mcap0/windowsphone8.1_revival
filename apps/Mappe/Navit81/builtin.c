/* Static plugin registration for the Windows Phone 8.1 build (USE_PLUGINS is off): the same job as the
 * builtin.c that Navit's CMake generates from builtin.c.in. Keep in sync with gen_vcxproj.py. */
void module_map_binfile_init(void);
void module_map_textfile_init(void);
void module_map_csv_init(void);
void module_vehicle_null_init(void);
void module_vehicle_demo_init(void);
void module_graphics_null_init(void);
void module_event_wp81_init(void);
void module_graphics_d2d_init(void);
void module_vehicle_wp81_init(void);
void module_speech_wp81_init(void);

extern void builtin_init(void);
void builtin_init(void) {
    module_map_binfile_init();
    module_map_textfile_init();
    module_map_csv_init();
    module_vehicle_null_init();
    module_vehicle_demo_init();
    module_graphics_null_init();
    module_event_wp81_init();
    module_graphics_d2d_init();
    module_vehicle_wp81_init();
    module_speech_wp81_init();
}
