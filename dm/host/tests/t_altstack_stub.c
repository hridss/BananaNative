// the loader's module table (loader.c), which crash.c's report walks: the unit test has no modules
#include "../dm.h"
module_t *g_mod;
int g_nmod;
module_t *module_of(const void *p) { (void)p; return 0; }
void dm_fs_recent_dump(FILE *f) { (void)f; }   // the recent file opens of hle_fs.c, which the unit test does not link
