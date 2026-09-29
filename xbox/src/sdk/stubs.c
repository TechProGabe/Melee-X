/* stubs.c - generated from melee-pc's src/pc headers: the netplay, ranked,
 * LAN, Slippi replay, file cache and custom-music hooks the game calls. None
 * of those are built for the Xbox, so each reports "off" / "not active".
 * Regenerate when a melee-pc sync adds hooks (the link fails otherwise). */
#include "pc/net.h"
#include "pc/net_lan.h"
#include "pc/net_chat.h"
#include "pc/net_match.h"
#include "pc/net_rank.h"
#include "pc/net_rank_session.h"
#include "pc/net_rank_store.h"
#include "pc/net_sfx.h"
#include "pc/file_cache.h"
#include "pc/music_stream.h"
#include "pc/slp.h"
#include "pc/launcher.h"

bool net_sfx_is_handle(int32_t handle) { (void)handle; return false; }
bool net_sfx_is_private(int32_t handle) { (void)handle; return false; }
int32_t net_sfx_keyoff(int32_t handle) { (void)handle; return -1; }
void net_sfx_keyoff_track(int32_t track, bool all) { (void)track; (void)all; }
void net_sfx_private(bool on) { (void)on; }
int32_t net_sfx_resolve(int32_t handle) { (void)handle; return -1; }
int32_t net_sfx_set(int32_t handle, int what, int32_t value) { (void)handle; (void)what; (void)value; return -1; }
bool net_sfx_shielded(int32_t voice) { (void)voice; return false; }
int32_t net_sfx_start(int32_t sound, uint8_t volume, uint8_t pan, int32_t track, int32_t channel) { (void)sound; (void)volume; (void)pan; (void)track; (void)channel; return 0; }
bool pc_file_cache_get(const char* filename, void* dst, size_t* size) { (void)filename; (void)dst; (void)size; return false; }
bool pc_file_cache_get_size(const char* filename, size_t* size) { (void)filename; (void)size; return false; }
void pc_file_cache_put(const char* filename, const void* data, size_t size) { (void)filename; (void)data; (void)size; }
bool pc_file_cache_require(const char* filename) { (void)filename; return false; }
void pc_file_cache_start_prewarm(void) {}
bool pc_lan_discovery_unavailable(void) { return false; }
bool pc_lan_full(void) { return false; }
bool pc_lan_is_host(void) { return false; }
const char* pc_lan_local_name(void) { return 0; }
int pc_lan_peers(PcLanPeer* out, int max) { (void)out; (void)max; return 0; }
void pc_lan_poll(void) {}
uint32_t pc_lan_seed(void) { return 0; }
void pc_lan_start(void) {}
int32_t pc_lan_start_frame(void) { return 0; }
bool pc_lan_start_match(void) { return false; }
int pc_lan_state(const char** why) { (void)why; return 0; }
void pc_lan_stop(void) {}
bool pc_music_stream_is_playing(void) { return false; }
bool pc_music_stream_open(const char* track_stem) { (void)track_stem; return false; }
void pc_music_stream_set_volume(float vol) { (void)vol; }
void pc_music_stream_stop(void) {}
bool pc_net_active(void) { return false; }
bool pc_net_after_tick(bool scene_ending) { (void)scene_ending; return false; }
bool pc_net_audio_deaf(bool* answer) { (void)answer; return false; }
void pc_net_audio_deaf_note(bool live) { (void)live; }
int32_t pc_net_audio_record(int32_t v) { return v; }
bool pc_net_audio_replay(int32_t* out) { (void)out; return false; }
const char* pc_net_chat_line(void) { return 0; }
const char* pc_net_chat_prompt(void) { return 0; }
bool pc_net_desync(void) { return false; }
bool pc_net_deterministic(void) { return false; }
void pc_net_disconnect(void) {}
int32_t pc_net_frame(void) { return 0; }
int pc_net_local_player(void) { return 0; }
bool pc_net_match_clipboard_code(char out[18]) { (void)out; return false; }
int pc_net_match_contacts(PcNetContact* out, int max) { (void)out; (void)max; return 0; }
bool pc_net_match_copy_code(void) { return false; }
const PcNetIdentity* pc_net_match_identity(void) { return 0; }
const char* pc_net_match_local_code(void) { return 0; }
const char* pc_net_match_opponent_code(void) { return 0; }
void pc_net_match_poll(void) {}
void pc_net_match_poll_publication(void) {}
const char* pc_net_match_profile_directory(void) { return 0; }
void pc_net_match_progress(PcNetMatchProgress* progress) { (void)progress; }
int pc_net_match_publication(const char** reason) { (void)reason; return 0; }
bool pc_net_match_publish_rank(void) { return false; }
uint32_t pc_net_match_seed(void) { return 0; }
bool pc_net_match_start(enum PcNetMatchMode mode, const char* target_code) { (void)mode; (void)target_code; return false; }
int32_t pc_net_match_start_frame(void) { return 0; }
int pc_net_match_state(const char** why) { (void)why; return 0; }
void pc_net_match_stop(void) {}
void pc_net_note_io(void) {}
int pc_net_peer_status(void) { return 0; }
void pc_net_peer_status_clear(void) {}
void pc_net_poll(void) {}
bool pc_net_pure_load(const char* filename) { (void)filename; return false; }
int pc_net_quality(void) { return 0; }
int pc_net_recv_reliable(uint8_t* type, void* payload, int max) { (void)type; (void)payload; (void)max; return 0; }
void pc_net_render_audit(bool after) { (void)after; }
bool pc_net_resim(void) { return false; }
bool pc_net_scene_hold(void) { return false; }
uint32_t pc_net_seed(void) { return 0; }
bool pc_net_send_reliable(uint8_t type, const void* payload, int len) { (void)type; (void)payload; (void)len; return false; }
bool pc_net_sfx_on(void) { return false; }
bool pc_net_stats(int* ping_ms, int* delay_frames, unsigned* rollbacks) { (void)ping_ms; (void)delay_frames; (void)rollbacks; return false; }
void pc_net_sync(void) {}
double pc_rank_display(const PcNetRating* rating) { (void)rating; return 0.0; }
void pc_rank_session_abort(const char* reason) { (void)reason; }
bool pc_rank_session_active(void) { return false; }
bool pc_rank_session_choose_stage(unsigned player, unsigned stage) { (void)player; (void)stage; return false; }
bool pc_rank_session_game(unsigned winner, unsigned stocks0, unsigned stocks1, unsigned stage, uint32_t frames) { (void)winner; (void)stocks0; (void)stocks1; (void)stage; (void)frames; return false; }
void pc_rank_session_poll(void) {}
unsigned pc_rank_session_seconds(void) { return 0; }
bool pc_rank_session_set_complete(void) { return false; }
unsigned pc_rank_session_stage(void) { return 0; }
bool pc_rank_session_stage_available(unsigned stage) { (void)stage; return false; }
void pc_rank_session_stage_begin(void) {}
int pc_rank_session_stage_port(void) { return 0; }
const char* pc_rank_session_stage_prompt(void) { return 0; }
int pc_rank_session_state(const char** reason) { (void)reason; return 0; }
unsigned pc_rank_session_stocks(void) { return 0; }
void pc_rank_store_close(PcNetRankStore* store) { (void)store; }
bool pc_rank_store_current(const PcNetRankStore* store, PcNetRating* rating, uint8_t head[32], uint32_t* count) { (void)store; (void)rating; (void)head; (void)count; return false; }
PcNetRankStore* pc_rank_store_open(const char* directory, const uint8_t key[32], PcNetRankStoreResult* result) { (void)directory; (void)key; (void)result; return 0; }
void pc_set_net_target(const char* code) { (void)code; }
void pc_slp_match_end(void) {}
void pc_slp_match_start(const struct StartMeleeData* data) { (void)data; }
void pc_slp_pre_frame(struct HSD_GObj* gobj) { (void)gobj; }
void pc_slp_tick_begin(void) {}
void pc_slp_tick_end(uint64_t proc_mask) { (void)proc_mask; }
