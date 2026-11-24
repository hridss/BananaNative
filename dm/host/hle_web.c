// nn::web offline HTML sessions (OfflineWebSession): the pages a game ships in its HtmlDocument NCA, which the web applet shows over the game while the game drives it.
// This host has no web view (upstream's hle_web.c shows them in WKWebView), so the stand-in answers: the page loads at once, sends one empty content message and closes
// when it appears (exit reason 0, no last URL). The SDK's own Start creates a library applet through am's self controller, which the host never made: a null read in
// applet::StartLibraryAppletImmediately (Super Mario 3D All-Stars' Sunshine starts its launcher pause menu, launcher_pause_menu.htdocs, hidden at boot, right after its save
// is mounted; Super Mario 64 imports the same). The synchronous form, web::ShowOfflineHtmlPage, stays cut in hle_offline.c.
// OfflineHtmlPageReturnValue: u64 exit reason at +0, the last URL at +8 (0x1000 bytes), its size at +0x1008; its getters stay in the SDK
#include "hle.h"
#include <pthread.h>

enum { URL_SIZE = 0x1000 };

// One page at a time, as the applet. state: 0 none, 1 started (loaded, hidden), 2 shown, 3 ended (the exit event is signalled, WaitForExit takes the result)
static _Alignas(16) uint8_t g_ev_web[0x100];
static pthread_mutex_t g_web_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_web_cv = PTHREAD_COND_INITIALIZER;
static int g_web_state, g_web_msg;   // g_web_msg: the stand-in's ready message not yet taken
static uint32_t g_web_reason;

static void web_end_locked(uint32_t reason) {   // the page ends: the exit event fires
    if (g_web_state == 0 || g_web_state == 3) return;
    g_web_reason = reason; g_web_state = 3;
    hle_signal_system_event(g_ev_web);
    pthread_cond_broadcast(&g_web_cv);
}
static void web_return(void *rv) {   // the ended page's result into the return value; the session is over
    pthread_mutex_lock(&g_web_mu);
    if (rv) { *(uint64_t *)rv = g_web_reason; ((char *)rv)[8] = 0; *(uint64_t *)((char *)rv + 8 + URL_SIZE) = 0; }
    g_web_state = 0; g_web_msg = 0; hle_clear_system_event(g_ev_web);
    pthread_mutex_unlock(&g_web_mu);
}

HLE_FORCE("_ZN2nn3web17OfflineWebSessionC1Ev", "offline HTML session (no applet): nothing to construct", void, h_webs_Ctor, void *s) { (void)s; }
HLE_FORCE("_ZN2nn3web17OfflineWebSessionD1Ev", "offline HTML session (no applet)", void, h_webs_Dtor, void *s) { (void)s; }
HLE_FORCE("_ZN2nn3web17OfflineWebSession17GetWorkBufferSizeEv", "offline HTML session work buffer: the SDK's size (0xd0), unused by the host", size_t, h_webs_WorkSize, void) { return 0xd0; }
HLE_FORCE("_ZN2nn3web17OfflineWebSession10InitializeEPvm", "offline HTML session: the work buffer is not used", void, h_webs_Init, void *s, void *buf, size_t n) { (void)s; (void)buf; (void)n; }
HLE_FORCE("_ZN2nn3web17OfflineWebSession8FinalizeEv", "offline HTML session: a page still open is closed", void, h_webs_Fini, void *s) {
    (void)s; pthread_mutex_lock(&g_web_mu); int open = g_web_state == 1 || g_web_state == 2; if (open) web_end_locked(2); pthread_mutex_unlock(&g_web_mu);
    if (open) web_return(NULL);
}
HLE_FORCE("_ZN2nn3web17OfflineWebSession5StartEPPNS_2os15SystemEventTypeERKNS0_22ShowOfflineHtmlPageArgE", "start an offline HTML page: the stand-in (no web view)",
          Result, h_webs_Start, void *s, void **ev, const uint8_t *arg) {
    (void)s; (void)arg;
    hle_bind_system_event(g_ev_web, 0); hle_clear_system_event(g_ev_web);
    if (ev) *ev = g_ev_web;
    pthread_mutex_lock(&g_web_mu);
    g_web_state = 1; g_web_reason = 0;
    g_web_msg = 1;   // the stand-in's ready message: the game waits for any message before Appear (the pause menu sends end_sound_load)
    pthread_mutex_unlock(&g_web_mu);
    static int n; if (n++ < 8) LOG("web: offline page started: no web view, the stand-in answers (it loads at once and closes when it appears)");
    return RES_OK;
}
HLE_FORCE("_ZN2nn3web17OfflineWebSession6AppearEv", "show the offline HTML page (the stand-in closes at once)", unsigned, h_webs_Appear, void *s) {
    (void)s;
    pthread_mutex_lock(&g_web_mu);
    if (g_web_state == 1 || g_web_state == 2) {
        static int n; if (n++ < 8) LOG("web::OfflineWebSession::Appear: the stand-in closes at once (no page shown)");
        g_web_state = 2; web_end_locked(0);
    }
    pthread_mutex_unlock(&g_web_mu);
    return 1;
}
HLE_FORCE("_ZN2nn3web17OfflineWebSession21TrySendContentMessageEPKcm", "content message to the offline HTML page (no page: dropped)", unsigned, h_webs_Send, void *s, const char *msg, size_t n) {
    (void)s; (void)msg; (void)n; return 1;
}
HLE_FORCE("_ZN2nn3web17OfflineWebSession24TryReceiveContentMessageEPmPcm", "content message from the offline HTML page: the stand-in's one empty message",
          unsigned, h_webs_Recv, void *s, size_t *out, char *buf, size_t n) {
    (void)s;
    pthread_mutex_lock(&g_web_mu);
    int got = g_web_msg && (g_web_state == 1 || g_web_state == 2);
    if (got) { g_web_msg = 0; if (buf && n) buf[0] = 0; if (out) *out = 0; }
    pthread_mutex_unlock(&g_web_mu);
    return got;
}
HLE_FORCE("_ZNK2nn3web17OfflineWebSession16NeedsWaitForExitEv", "whether the offline HTML page was started and not waited for", unsigned, h_webs_Needs, const void *s) {
    (void)s; pthread_mutex_lock(&g_web_mu); int r = g_web_state != 0; pthread_mutex_unlock(&g_web_mu); return r; }
HLE_FORCE("_ZN2nn3web17OfflineWebSession11RequestExitEv", "close the offline HTML page at the game's request (exit reason 2; the exit event fires)", void, h_webs_ReqExit, void *s) {
    (void)s; LOG("web::OfflineWebSession::RequestExit"); pthread_mutex_lock(&g_web_mu); web_end_locked(2); pthread_mutex_unlock(&g_web_mu); }
HLE_FORCE("_ZN2nn3web17OfflineWebSession11WaitForExitEPNS0_26OfflineHtmlPageReturnValueE", "wait for the offline HTML page's end: its exit reason", Result, h_webs_Wait, void *s, void *rv) {
    (void)s;
    pthread_mutex_lock(&g_web_mu);
    if (g_web_state == 1 || g_web_state == 2) { dm_thread_native_wait(1); while (g_web_state == 1 || g_web_state == 2) pthread_cond_wait(&g_web_cv, &g_web_mu); dm_thread_native_wait(0); }   // (the guest core is given up meanwhile)
    pthread_mutex_unlock(&g_web_mu);
    web_return(rv); return RES_OK;
}
