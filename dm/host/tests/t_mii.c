// hle_mii.c unit test: the database of Miis made in the host's stand-in for the Mii editor. AppendMii adds the default Mii with a create ID of its own, Get lists the database's Miis
// (source 0) before the six default ones (source 1), GetIndex and UpdateLatest find it by its create ID, IsUpdated is true once, the Mii is kept in <DM_SYSTEM_DIR>/host/MiiDatabase.dat
// and a later start lists it again; a damaged host file is refused, a full database cancels
#include "../hle_mii.c"
#include <unistd.h>
static uint64_t g_entropy = 0x0123456789abcdefull;
uint64_t dm_entropy64(void) { return g_entropy += 0x1111; }
const char *dm_system_dir(char *buf, size_t n) { (void)buf; (void)n; const char *v = getenv("DM_SYSTEM_DIR"); return v ? v : "build/t_mii.none"; }
void dm_applet_enter(const char *name) { (void)name; }
void dm_applet_leave(void) {}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL " __VA_ARGS__); puts(""); fails++; } } while (0)

int main(int argc, char **argv) {
    const char *base = argc > 1 ? argv[1] : "build";   // a writable folder (the phone runs it from /data/local/tmp)
    char root[512], hp[600];
    snprintf(root, sizeof root, "%s/t_mii.XXXXXX", base);
    CHECK(mkdtemp(root) != NULL, "temporary folder in %s", base);
    setenv("DM_SYSTEM_DIR", root, 1);

    // A first start: no host file, an empty database beside the six default Miis
    CHECK(h_mii_Init(NULL) == RES_OK && g_mii_n == 0, "Initialize without a host file: %d", g_mii_n);
    CHECK(h_mii_Count(NULL, MII_SRC_DATABASE) == 0 && h_mii_Count(NULL, MII_SRC_DEFAULT) == 6 && h_mii_Count(NULL, 3) == 6, "counts of an empty database");
    CHECK(h_mii_Updated(NULL, MII_SRC_DATABASE) == 0, "nothing updated yet");
    struct __attribute__((packed)) { mii_charinfo c; uint32_t source; } el[10]; int n = -1, idx = -1;
    CHECK(h_mii_GetE(NULL, &n, el, 10, 3) == RES_OK && n == 6 && el[0].source == 1, "Get of both: the six defaults: %d", n);
    mii_charinfo def0 = el[0].c;
    CHECK(h_mii_Index(NULL, &idx, &def0) == MII_RES_NOT_FOUND, "a default Mii is not in the database");

    // AppendMii: the default Mii as entry 0, kept in the host's file
    int ai = -1;
    CHECK(h_mii_Append(&ai) == RES_OK && ai == 0 && g_mii_n == 1, "AppendMii adds entry 0: %d, %d Miis", ai, g_mii_n);
    CHECK(h_mii_Updated(NULL, MII_SRC_DEFAULT) == 0 && h_mii_Updated(NULL, MII_SRC_DATABASE) == 1 && h_mii_Updated(NULL, MII_SRC_DATABASE) == 0, "IsUpdated once, for the database");
    CHECK(h_mii_Count(NULL, MII_SRC_DATABASE) == 1 && h_mii_GetE(NULL, &n, el, 10, 3) == RES_OK && n == 7 && el[0].source == 0 && el[1].source == 1, "Get lists the new Mii first");
    CHECK(h_mii_GetE(NULL, &n, el, 1, 3) == RES_OK && n == 1, "Get stops at the room given");
    mii_charinfo made = el[0].c;
    CHECK(made.nickname[0] == 'n' && made.height == 64 && memcmp(made.create_id, def0.create_id, 16), "the default Mii, with a create ID of its own");
    CHECK((made.create_id[6] & 0xf0) == 0x40 && (made.create_id[8] & 0xc0) == 0x80, "a version 4 create ID");
    CHECK(h_mii_Index(NULL, &idx, &made) == RES_OK && idx == 0, "GetIndex finds the new Mii");
    mii_charinfo latest = made; latest.height = 7;
    CHECK(h_mii_Latest(NULL, &latest, MII_SRC_DATABASE) == RES_OK && latest.height == 64, "UpdateLatest gives the database's copy");
    CHECK(h_mii_Latest(NULL, &latest, MII_SRC_DEFAULT) == MII_RES_NOT_FOUND, "UpdateLatest without the database source finds nothing");
    struct stat st;
    snprintf(hp, sizeof hp, "%s/host/MiiDatabase.dat", root);
    CHECK(stat(hp, &st) == 0 && st.st_size == MII_HOST_HEAD + (off_t)sizeof(mii_charinfo), "the host's file holds one Mii: %lld bytes", (long long)st.st_size);
    char tmp[620]; snprintf(tmp, sizeof tmp, "%s.new", hp);
    CHECK(stat(tmp, &st) != 0, "no .new file is left behind");
    CHECK(h_mii_Append(&ai) == RES_OK && ai == 1 && g_mii_n == 2, "a second AppendMii adds entry 1: %d", ai);
    CHECK(stat(hp, &st) == 0 && st.st_size == MII_HOST_HEAD + 2 * (off_t)sizeof(mii_charinfo), "the host's file holds two Miis");

    // The next start lists them again, with the same create IDs
    memset(g_mii_db, 0, sizeof g_mii_db); mii_db_load();
    CHECK(g_mii_n == 2 && !memcmp(g_mii_db[0].create_id, made.create_id, 16), "a later start reads the host's Miis back: %d", g_mii_n);
    // A host file that is not the host's database is refused and lists nothing
    FILE *hf = fopen(hp, "r+b"); if (hf) { fputc('X', hf); fclose(hf); }
    mii_db_load(); CHECK(g_mii_n == 0, "a damaged host file is refused: %d", g_mii_n);
    // A full database cancels
    g_mii_n = MII_DB_MAX; CHECK(h_mii_Full(NULL) == 1 && h_mii_Append(&ai) == MII_RES_CANCELED, "a full database cancels"); g_mii_n = 0;

    unlink(hp); snprintf(hp, sizeof hp, "%s/host", root); rmdir(hp); rmdir(root);
    printf("t_mii: %s\n", fails ? "FAILED" : "all checks pass");
    return fails != 0;
}
