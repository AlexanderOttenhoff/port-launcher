/* Port-launcher module ABI (docs/MODULE_SPEC.md §4). Plain C, shared by hosts and modules. */
#ifndef PLM_MODULE_ABI_H
#define PLM_MODULE_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PLM_ABI_VERSION 0u
#define PLM_HOST_API_VERSION 1u

/* Services the host offers a module, as C function pointers (no JNI names, so Linux hosts can offer the same).
   Fields are only ever appended; check version before using later ones. */
typedef struct plm_host_api {
    uint32_t version; /* PLM_HOST_API_VERSION the host implements */
    void* context;    /* pass back to every call */

    /* The module has drawn its first game frame; the host removes its loading screen. Any thread, once. */
    void (*first_frame)(void* context);
} plm_host_api;

/* Module exports. plm_set_host is optional: called before plm_main when present; api outlives the module. */
uint32_t plm_abi_version(void);
int plm_main(int argc, char** argv);
void plm_request_quit(void);
void plm_set_host(const plm_host_api* api);

/* Optional (host menu, MODULE_SPEC §4.2): plm_pause holds the game at its next frame boundary and silences it;
   plm_resume lets it continue. Called from the host's UI thread; must not block it. plm_request_quit must work
   while paused. */
void plm_pause(void);
void plm_resume(void);

/* Optional: the host menu changed settings. json is an object of canonical key -> value (e.g.
   {"game.cutscene_skip":"start"}); apply what the module supports, live. Host UI thread; must not block. */
void plm_apply_settings(const char* json);

#ifdef __cplusplus
}
#endif

#endif /* PLM_MODULE_ABI_H */
