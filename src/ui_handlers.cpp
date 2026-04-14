/**
 * UI Event Handlers and Utilities
 * All event callbacks, WiFi, OTA, brightness control, and UI update functions
 */

#include "ui_common.h"
#include "ui_icons.h"
#include <vector>
#include "config.h"
#include "lyrics.h"
#include "clock_screen.h"
#include <esp_task_wdt.h>

// ============================================================================
// Brightness Control
// ============================================================================
void setBrightness(int level) {
    brightness_level = constrain(level, MIN_BRIGHTNESS, MAX_BRIGHTNESS);
    display_set_brightness(brightness_level);
    wifiPrefs.putInt(NVS_KEY_BRIGHTNESS, brightness_level);
}

void resetScreenTimeout() {
    last_touch_time = millis();
    if (screen_dimmed) {
        // Instant wake-up - no animation
        display_set_brightness(brightness_level);
        screen_dimmed = false;
    }
}

// Brightness animation callback for smooth dimming
static void brightness_anim_cb(void* var, int32_t v) {
    display_set_brightness(v);
}

void checkAutoDim() {
    if (autodim_timeout == 0) return;  // Auto-dim disabled
    if (screen_dimmed) return;  // Already dimmed

    if ((millis() - last_touch_time) > (autodim_timeout * 1000)) {
        int dimmed = constrain(brightness_dimmed, 5, 100);

        // Smooth fade to dimmed brightness (1 second fade)
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, NULL);
        lv_anim_set_values(&anim, brightness_level, dimmed);
        lv_anim_set_duration(&anim, 1000);  // 1 second smooth fade
        lv_anim_set_exec_cb(&anim, brightness_anim_cb);
        lv_anim_set_path_cb(&anim, lv_anim_path_ease_in);
        lv_anim_start(&anim);

        screen_dimmed = true;
    }
}

// ============================================================================
// YTMD (pear-desktop) API Helpers
// ============================================================================

// Returns true when operating in YTMD-only mode (no real Sonos device active).
static bool isYtmdMode() {
    SonosDevice* d = sonos.getCurrentDevice();
    return !d || (d->rinconID == "YTMD_VIRTUAL");
}
static bool isYtmdVirtualDevice(const SonosDevice* d);

// Shared interpolation state — written by poll, read by updateYtmdProgressInterp()
static float         s_ytmd_elapsed_sec  = 0.0f;
static int           s_ytmd_dur_sec      = 0;
static bool          s_ytmd_is_playing   = false;
static unsigned long s_ytmd_pos_ts       = 0;   // millis() when elapsed was last polled
// Local volume-set guard: prevents stale GET /volume from immediately snapping
// the slider back after a successful user drag+release POST.
static unsigned long s_ytmd_local_volume_set_ms = 0;
static int           s_ytmd_local_volume_target = -1;
// Some pear-desktop builds can report GET /volume.state as fixed 100 even after
// successful POST /volume. Detect that pattern and stop forcing slider to max.
static bool          s_ytmd_volume_sync_suspect = false;
static uint8_t       s_ytmd_stuck100_hits = 0;
// This backend currently does not expose /api/v1/queue/next.
// Keep disabled to avoid periodic 404s; parse /api/v1/queue directly.
static bool          s_ytmd_queue_next_supported = false;

#ifndef YTMD_FB_VERBOSE_LOGS
#define YTMD_FB_VERBOSE_LOGS 0
#endif

#ifndef YTMD_VOL_DEBUG
#define YTMD_VOL_DEBUG 0
#endif

#ifndef YTMD_CMD_DEBUG
#define YTMD_CMD_DEBUG 0
#endif

#ifndef YTMD_QUEUE_DEBUG
#define YTMD_QUEUE_DEBUG 1
#endif

#if YTMD_FB_VERBOSE_LOGS
#define YTMD_FB_LOG(...) Serial.printf(__VA_ARGS__)
#define YTMD_FB_LOG_LN(msg) Serial.println(msg)
#else
#define YTMD_FB_LOG(...) do {} while (0)
#define YTMD_FB_LOG_LN(msg) do {} while (0)
#endif

#if YTMD_VOL_DEBUG
#define YTMD_VOL_LOG(...) Serial.printf(__VA_ARGS__)
#else
#define YTMD_VOL_LOG(...) do {} while (0)
#endif

#if YTMD_CMD_DEBUG
#define YTMD_CMD_LOG(...) Serial.printf(__VA_ARGS__)
#else
#define YTMD_CMD_LOG(...) do {} while (0)
#endif

#if YTMD_QUEUE_DEBUG
#define YTMD_QUEUE_LOG(...) Serial.printf(__VA_ARGS__)
#define YTMD_QUEUE_LOG_LN(msg) Serial.println(msg)
#else
#define YTMD_QUEUE_LOG(...) do {} while (0)
#define YTMD_QUEUE_LOG_LN(msg) do {} while (0)
#endif

// Parse "M:SS" or "H:MM:SS" into seconds.
static bool parseTimeStringToSeconds(const char* text, float* outSeconds) {
    if (!text || !outSeconds) return false;
    String s(text);
    s.trim();
    if (s.length() == 0) return false;

    bool neg = false;
    if (s[0] == '-') {
        neg = true;
        s.remove(0, 1);
        s.trim();
    }
    if (s.length() == 0 || s.indexOf(':') < 0) return false;

    int p2 = s.lastIndexOf(':');
    int p1 = s.lastIndexOf(':', p2 - 1);
    long h = 0, m = 0, sec = 0;

    if (p1 >= 0) {
        h = s.substring(0, p1).toInt();
        m = s.substring(p1 + 1, p2).toInt();
        sec = s.substring(p2 + 1).toInt();
    } else {
        m = s.substring(0, p2).toInt();
        sec = s.substring(p2 + 1).toInt();
    }

    if (m < 0 || sec < 0) return false;
    float total = (float)(h * 3600 + m * 60 + sec);
    *outSeconds = neg ? -total : total;
    return true;
}

// Parse seconds from JSON value that may be numeric or a time string.
static bool parseJsonSeconds(JsonVariantConst v, float* outSeconds) {
    if (!outSeconds || v.isNull()) return false;
    // Guard against accidental bool->number coercion (true=>1, false=>0).
    // For volume parsing this can incorrectly map "true" to 100%.
    if (v.is<bool>()) return false;

    if (v.is<const char*>()) {
        const char* s = v.as<const char*>();
        if (!s || !s[0]) return false;
        float sec = 0.0f;
        if (parseTimeStringToSeconds(s, &sec)) {
            *outSeconds = sec;
            return true;
        }
        *outSeconds = (float)atof(s);
        return true;
    }

    if (v.is<float>() || v.is<double>() ||
        v.is<int>() || v.is<long>() ||
        v.is<unsigned int>() || v.is<unsigned long>()) {
        *outSeconds = v.as<float>();
        return true;
    }
    return false;
}

// Parse bool from JSON value that may be bool/int/string.
static bool parseJsonBoolFlexible(JsonVariantConst v, bool defaultVal) {
    if (v.isNull()) return defaultVal;
    if (v.is<bool>()) return v.as<bool>();
    if (v.is<int>() || v.is<long>() || v.is<unsigned int>() || v.is<unsigned long>()) {
        return v.as<long>() != 0;
    }
    if (v.is<const char*>()) {
        String s(v.as<const char*>());
        s.trim();
        s.toLowerCase();
        if (s == "true" || s == "1" || s == "yes" || s == "on") return true;
        if (s == "false" || s == "0" || s == "no" || s == "off") return false;
    }
    return defaultVal;
}

static void formatTimeLabel(int totalSec, char* out, size_t outSize, bool withMinus) {
    if (!out || outSize == 0) return;
    if (totalSec < 0) totalSec = 0;
    int h = totalSec / 3600;
    int m = (totalSec % 3600) / 60;
    int s = totalSec % 60;
    const char* sign = withMinus ? "-" : "";
    if (h > 0) snprintf(out, outSize, "%s%d:%02d:%02d", sign, h, m, s);
    else snprintf(out, outSize, "%s%d:%02d", sign, m, s);
}

// Parse /api/v1/volume response.
// pear-desktop commonly returns: { "state": <0..100>, "isMuted": <bool> }
// but field names can vary by build, so we accept several shapes/keys.
static bool parseYtmdVolumeStateResponse(const String& resp, int* outVolume, bool* outMuted) {
    if (!outVolume || !outMuted) return false;

    DynamicJsonDocument doc(256);
    DeserializationError err = deserializeJson(doc, resp);
    if (err) return false;

    JsonVariantConst root = doc.as<JsonVariantConst>();
    float volumeNum = 0.0f;
    bool hasVolume = false;
    bool hasMuted = false;
    bool isMuted = ui_muted;

    auto tryVolume = [&](JsonVariantConst v) {
        if (hasVolume || v.isNull()) return;

        float tmp = 0.0f;
        if (parseJsonSeconds(v, &tmp)) {
            volumeNum = tmp;
            hasVolume = true;
            return;
        }

        if (v.is<JsonObjectConst>()) {
            JsonVariantConst o = v;
            if (parseJsonSeconds(o["state"], &tmp) ||
                parseJsonSeconds(o["volume"], &tmp) ||
                parseJsonSeconds(o["value"], &tmp) ||
                parseJsonSeconds(o["level"], &tmp) ||
                parseJsonSeconds(o["percent"], &tmp)) {
                volumeNum = tmp;
                hasVolume = true;
            }
        }
    };

    auto tryMuted = [&](JsonVariantConst v) {
        if (hasMuted || v.isNull()) return;

        if (v.is<JsonObjectConst>()) {
            JsonVariantConst o = v;
            if (!o["isMuted"].isNull()) {
                isMuted = parseJsonBoolFlexible(o["isMuted"], isMuted);
                hasMuted = true;
            } else if (!o["muted"].isNull()) {
                isMuted = parseJsonBoolFlexible(o["muted"], isMuted);
                hasMuted = true;
            } else if (!o["mute"].isNull()) {
                isMuted = parseJsonBoolFlexible(o["mute"], isMuted);
                hasMuted = true;
            } else if (!o["is_muted"].isNull()) {
                isMuted = parseJsonBoolFlexible(o["is_muted"], isMuted);
                hasMuted = true;
            }
            return;
        }

        isMuted = parseJsonBoolFlexible(v, isMuted);
        hasMuted = true;
    };

    // Prefer explicit volume keys first, then broader fallbacks.
    tryVolume(root);
    tryVolume(root["volume"]);
    tryVolume(root["value"]);
    tryVolume(root["level"]);
    tryVolume(root["percent"]);
    tryVolume(root["state"]);
    tryVolume(root["data"]);
    tryVolume(root["data"]["volume"]);
    tryVolume(root["data"]["value"]);
    tryVolume(root["data"]["state"]);
    tryVolume(root["data"]["player"]);
    tryVolume(root["data"]["player"]["volume"]);
    tryVolume(root["player"]);
    tryVolume(root["player"]["volume"]);
    tryVolume(root["player"]["value"]);
    tryVolume(root["player"]["state"]);

    tryMuted(root["isMuted"]);
    tryMuted(root["muted"]);
    tryMuted(root["mute"]);
    tryMuted(root["is_muted"]);
    tryMuted(root["data"]);
    tryMuted(root["player"]);

    if (!hasVolume && !hasMuted) return false;

    int volume = ui_vol >= 0 ? ui_vol : 0;
    if (hasVolume) {
        // Some endpoints return 0..1 instead of 0..100.
        // Accept 0..1 fractional values from some builds, but keep exact "1"
        // as 1% for strict 0..100 endpoints that report integer percentages.
        if (volumeNum > 0.0f && volumeNum < 1.0f) volumeNum *= 100.0f;
        int rounded = (int)(volumeNum + 0.5f);
        volume = constrain(rounded, 0, 100);
    }

    *outVolume = volume;
    *outMuted = isMuted;
    return true;
}

// Poll current volume from pear-desktop and mirror it into the UI state.
static void pollAndApplyYtmdVolumeState(const char* authHeader) {
    if (!authHeader || !authHeader[0]) return;

    char url[192];
    snprintf(url, sizeof(url), "http://%s:%d/api/v1/volume",
             ytmd_ip.c_str(),
             (ytmd_port > 0 ? ytmd_port : YTMD_DEFAULT_PORT));

    HTTPClient http;
    http.begin(url);
    http.addHeader("Authorization", authHeader);
    // Keep short to minimize UI-thread blocking when desktop app is unreachable.
    http.setTimeout(450);

    int code = http.GET();
    if (code != 200) {
        YTMD_VOL_LOG("[YTMD/VOL] GET /volume -> HTTP %d\n", code);
        YTMD_FB_LOG("[YTMD/FB] GET %s -> HTTP %d\n", url, code);
        http.end();
        return;
    }

    String resp = http.getString();
    http.end();
    YTMD_VOL_LOG("[YTMD/VOL] body: %.200s\n", resp.c_str());

    int serverVolume = ui_vol >= 0 ? ui_vol : 0;
    bool serverMuted = ui_muted;
    if (!parseYtmdVolumeStateResponse(resp, &serverVolume, &serverMuted)) {
        YTMD_VOL_LOG("[YTMD/VOL] parse failed\n");
        YTMD_FB_LOG("[YTMD/FB] /volume parse failed: %.180s\n", resp.c_str());
        return;
    }
    YTMD_VOL_LOG("[YTMD/VOL] parsed: vol=%d muted=%d ui_vol=%d dragging=%d suspect=%d target=%d\n",
                 serverVolume, (int)serverMuted, ui_vol, (int)dragging_vol,
                 (int)s_ytmd_volume_sync_suspect, s_ytmd_local_volume_target);

    // If user just set volume, tolerate a short server propagation delay.
    // This avoids snapping back to a stale value (commonly 100) right after drag.
    const unsigned long now = millis();
    if (s_ytmd_local_volume_target >= 0) {
        if (serverVolume == s_ytmd_local_volume_target) {
            s_ytmd_local_volume_target = -1;
        } else if ((now - s_ytmd_local_volume_set_ms) < 1600) {
            return;
        } else {
            s_ytmd_local_volume_target = -1;
        }
    }

    // Detect broken /volume state feedback (stuck at 100 while local ui_vol is not high).
    // This keeps UI stable instead of snapping back to max every poll.
    if (!dragging_vol && ui_vol >= 0 && ui_vol <= 95) {
        if (serverVolume == 100) {
            if (s_ytmd_stuck100_hits < 255) s_ytmd_stuck100_hits++;
            if (s_ytmd_stuck100_hits >= 3 && !s_ytmd_volume_sync_suspect) {
                s_ytmd_volume_sync_suspect = true;
                YTMD_VOL_LOG("[YTMD/VOL] state appears stuck at 100; suppressing forced slider sync\n");
            }
        } else {
            s_ytmd_stuck100_hits = 0;
            s_ytmd_volume_sync_suspect = false;
        }
    } else if (!dragging_vol && serverVolume != 100) {
        s_ytmd_stuck100_hits = 0;
        s_ytmd_volume_sync_suspect = false;
    }

    if (!s_ytmd_volume_sync_suspect) {
        if (!dragging_vol && slider_vol && serverVolume != ui_vol) {
            lv_slider_set_value(slider_vol, serverVolume, LV_ANIM_OFF);
            ui_vol = serverVolume;
            YTMD_VOL_LOG("[YTMD/VOL] apply slider/ui -> %d\n", serverVolume);
        } else if (!dragging_vol && ui_vol != serverVolume) {
            ui_vol = serverVolume;
            YTMD_VOL_LOG("[YTMD/VOL] apply ui only -> %d\n", serverVolume);
        }
    } else {
        YTMD_VOL_LOG("[YTMD/VOL] sync suppressed (stuck100 suspected)\n");
    }

    if (serverMuted != ui_muted && btn_mute) {
        lv_obj_t* lbl = lv_obj_get_child(btn_mute, 0);
        lv_label_set_text(lbl, serverMuted ? MDI_VOLUME_OFF : MDI_VOLUME_HIGH);
    }
    ui_muted = serverMuted;
}

// POST to pear-desktop API with bearer token authentication.
// Called from UI task for playback commands — short timeout to avoid touch freeze.
// Returns HTTP response code (200 = success, negative = connection error).
static int ytmdApiPost(const char* path, const char* body = nullptr) {
    if (ytmd_ip.length() == 0 || ytmd_token.length() == 0) {
        YTMD_CMD_LOG("[YTMD] POST %s skipped: missing ip/token (ip=%d token=%d)\n",
                     path ? path : "<null>",
                     (int)(ytmd_ip.length() > 0),
                     (int)(ytmd_token.length() > 0));
        return -1;
    }
    if (WiFi.status() != WL_CONNECTED) {
        YTMD_CMD_LOG("[YTMD] POST %s skipped: wifi disconnected (status=%d)\n",
                     path ? path : "<null>", (int)WiFi.status());
        return -1;
    }

    if (!network_mutex || xSemaphoreTake(network_mutex, pdMS_TO_TICKS(700)) != pdTRUE) {
        YTMD_CMD_LOG("[YTMD] POST %s skipped: network mutex busy\n", path ? path : "<null>");
        return -1;
    }

    char url[192];
    snprintf(url, sizeof(url), "http://%s:%d%s",
             ytmd_ip.c_str(),
             (ytmd_port > 0 ? ytmd_port : YTMD_DEFAULT_PORT),
             path);
    char auth[300];
    snprintf(auth, sizeof(auth), "Bearer %s", ytmd_token.c_str());

    HTTPClient http;
    http.begin(url);
    http.addHeader("Authorization", auth);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(1500);

    int code;
    if (body && strlen(body) > 0) {
        code = http.POST((uint8_t*)body, strlen(body));
    } else {
        code = http.POST("");
    }
    http.end();
    xSemaphoreGive(network_mutex);

    YTMD_CMD_LOG("[YTMD] POST %s -> HTTP %d\n", path, code);
    return code;
}

// POST helper with simple compatibility fallback between API variants.
// Returns final HTTP code (or negative error code from ytmdApiPost).
static int ytmdApiPostWithFallback(const char* primaryPath,
                                   const char* primaryBody,
                                   const char* fallbackPath,
                                   const char* fallbackBody = nullptr) {
    int code = ytmdApiPost(primaryPath, primaryBody);
    if ((code == 404 || code == 405) && fallbackPath && fallbackPath[0]) {
        YTMD_CMD_LOG("[YTMD] Fallback POST %s -> %s (prev HTTP %d)\n",
                     primaryPath ? primaryPath : "<null>",
                     fallbackPath,
                     code);
        return ytmdApiPost(fallbackPath, fallbackBody);
    }
    return code;
}

static void applyYtmdPlayState(bool isPlaying) {
    if (btn_play) {
        lv_obj_t* lbl = lv_obj_get_child(btn_play, 0);
        if (lbl) {
            lv_label_set_text(lbl, isPlaying ? MDI_PAUSE : MDI_PLAY);
            lv_obj_set_style_text_font(lbl, &lv_font_mdi_40, 0);
            lv_obj_center(lbl);
        }
    }
    ui_playing = isPlaying;
    s_ytmd_is_playing = isPlaying;
    if (isPlaying) s_ytmd_pos_ts = millis();
}

static void applyYtmdMuteState(bool muted) {
    if (btn_mute) {
        lv_obj_t* lbl = lv_obj_get_child(btn_mute, 0);
        if (lbl) lv_label_set_text(lbl, muted ? MDI_VOLUME_OFF : MDI_VOLUME_HIGH);
    }
    ui_muted = muted;
}

static void applyYtmdShuffleState(bool shuffle) {
    if (btn_shuffle) {
        lv_obj_t* lbl = lv_obj_get_child(btn_shuffle, 0);
        if (lbl) lv_obj_set_style_text_color(lbl, shuffle ? COL_ACCENT : COL_TEXT2, 0);
    }
    ui_shuffle = shuffle;
}

static void applyYtmdRepeatMode(const String& rm) {
    if (btn_repeat) {
        lv_obj_t* lbl = lv_obj_get_child(btn_repeat, 0);
        if (lbl) {
            if (rm == "ONE") {
                lv_label_set_text(lbl, MDI_REPEAT_ONCE);
                lv_obj_set_style_text_font(lbl, &lv_font_mdi_32, 0);
                lv_obj_set_style_text_color(lbl, COL_ACCENT, 0);
            } else if (rm == "ALL") {
                lv_label_set_text(lbl, MDI_REPEAT);
                lv_obj_set_style_text_font(lbl, &lv_font_mdi_32, 0);
                lv_obj_set_style_text_color(lbl, COL_ACCENT, 0);
            } else {
                lv_label_set_text(lbl, MDI_REPEAT);
                lv_obj_set_style_text_font(lbl, &lv_font_mdi_32, 0);
                lv_obj_set_style_text_color(lbl, COL_TEXT2, 0);
            }
        }
    }
    ui_repeat = rm;
}

static String ytmdExtractText(JsonVariantConst v) {
    if (v.isNull()) return "";
    if (v.is<const char*>()) {
        const char* s = v.as<const char*>();
        return (s && s[0]) ? String(s) : String("");
    }
    if (v.is<JsonObjectConst>()) {
        JsonVariantConst o = v;
        const char* s = o["text"] | (const char*)nullptr;
        if (s && s[0]) return String(s);
        const char* r0 = o["runs"][0]["text"] | (const char*)nullptr;
        if (r0 && r0[0]) return String(r0);
    }
    return "";
}

static void applyYtmdNextTrackLine(const String& line) {
    if (lbl_next_header) lv_obj_clear_flag(lbl_next_header, LV_OBJ_FLAG_HIDDEN);
    if (lbl_next_title) {
        lv_label_set_text(lbl_next_title, line.c_str());
        if (line.length() > 0) lv_obj_clear_flag(lbl_next_title, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(lbl_next_title, LV_OBJ_FLAG_HIDDEN);
    }
    // In YTMD mode show single-line "title - artist" only.
    if (lbl_next_artist) lv_obj_add_flag(lbl_next_artist, LV_OBJ_FLAG_HIDDEN);
}

// Called from YTMD polling path while network_mutex is already held by caller.
// Returns true when queue data was successfully fetched/parsing completed.
static bool pollAndApplyYtmdNextTrack(const char* authHeader) {
    if (!authHeader || !authHeader[0]) return false;

    auto httpGet = [&](const char* path, String* outResp, int* outCode) -> bool {
        if (!path || !outResp || !outCode) return false;
        char url[192];
        snprintf(url, sizeof(url), "http://%s:%d%s",
                 ytmd_ip.c_str(),
                 (ytmd_port > 0 ? ytmd_port : YTMD_DEFAULT_PORT),
                 path);
        HTTPClient http;
        http.begin(url);
        http.addHeader("Authorization", authHeader);
        // Queue endpoints can be heavier than /song; keep timeout moderate.
        // Keep short to avoid UI stutter on slow/no response.
        http.setTimeout(350);
        int code = http.GET();
        *outCode = code;
        if (code != 200) {
            http.end();
            return false;
        }
        *outResp = http.getString();
        http.end();
        return true;
    };

    // Fast path: if backend supports /queue/next, use it.
    if (s_ytmd_queue_next_supported) {
        String resp;
        int code = 0;
        if (httpGet("/api/v1/queue/next", &resp, &code)) {
            DynamicJsonDocument doc(1024);
            if (!deserializeJson(doc, resp)) {
                JsonVariantConst root = doc.as<JsonVariantConst>();
                String title = ytmdExtractText(root["title"]);
                String artist = ytmdExtractText(root["shortBylineText"]);
                if (artist.length() == 0) artist = ytmdExtractText(root["author"]);
                if (artist.length() == 0) artist = ytmdExtractText(root["artist"]);
                String line = title;
                if (artist.length() > 0) {
                    if (line.length() > 0) line += " - ";
                    line += artist;
                }
                applyYtmdNextTrackLine(line);
                return true;
            }
        } else if (code == 404 || code == 405) {
            s_ytmd_queue_next_supported = false;
            YTMD_QUEUE_LOG_LN("[YTMD/QUEUE] /queue/next not supported; fallback to /queue");
        } else if (code == 204) {
            applyYtmdNextTrackLine("");
            return true;
        }
    }

    // Fallback path: parse /queue items and find selected+1.
    String resp;
    int code = 0;
    if (!httpGet("/api/v1/queue", &resp, &code)) return false;

    StaticJsonDocument<768> filter;
    filter["items"][0]["playlistPanelVideoRenderer"]["selected"] = true;
    filter["items"][0]["playlistPanelVideoRenderer"]["title"]["runs"][0]["text"] = true;
    filter["items"][0]["playlistPanelVideoRenderer"]["shortBylineText"]["runs"][0]["text"] = true;
    filter["items"][0]["playlistPanelVideoWrapperRenderer"]["primaryRenderer"]["playlistPanelVideoRenderer"]["selected"] = true;
    filter["items"][0]["playlistPanelVideoWrapperRenderer"]["primaryRenderer"]["playlistPanelVideoRenderer"]["title"]["runs"][0]["text"] = true;
    filter["items"][0]["playlistPanelVideoWrapperRenderer"]["primaryRenderer"]["playlistPanelVideoRenderer"]["shortBylineText"]["runs"][0]["text"] = true;

    DynamicJsonDocument doc(12288);
    DeserializationError err = deserializeJson(
        doc,
        resp,
        DeserializationOption::Filter(filter),
        DeserializationOption::NestingLimit(64)
    );
    if (err) {
        YTMD_QUEUE_LOG("[YTMD/QUEUE] /queue parse error: %s\n", err.c_str());
        return false;
    }

    JsonArrayConst items = doc["items"].as<JsonArrayConst>();
    if (items.isNull() || items.size() == 0) {
        applyYtmdNextTrackLine("");
        return true;
    }

    auto getRenderer = [&](JsonObjectConst item) -> JsonVariantConst {
        JsonVariantConst r = item["playlistPanelVideoRenderer"];
        if (r.isNull()) r = item["playlistPanelVideoWrapperRenderer"]["primaryRenderer"]["playlistPanelVideoRenderer"];
        return r;
    };

    int selectedPos = -1;
    for (int i = 0; i < (int)items.size(); ++i) {
        JsonObjectConst item = items[i].as<JsonObjectConst>();
        if (item.isNull()) continue;
        JsonVariantConst r = getRenderer(item);
        if (r.isNull()) continue;
        if (parseJsonBoolFlexible(r["selected"], false)) {
            selectedPos = i;
            break;
        }
    }

    // Sync YTMD queue into the existing SonosDevice cache so Playlist UI can
    // render from memory (same pattern as native Sonos flow).
    SonosDevice* dev = sonos.getCurrentDevice();
    if (dev && isYtmdVirtualDevice(dev)) {
        const int maxItems = (int)min((size_t)QUEUE_ITEMS_MAX, items.size());
        dev->queueSize = 0;
        dev->totalTracks = (int)items.size();
        if (selectedPos >= 0) {
            dev->currentTrackNumber = selectedPos + 1;  // 1-based
        }
        for (int i = 0; i < maxItems; ++i) {
            JsonObjectConst item = items[i].as<JsonObjectConst>();
            if (item.isNull()) continue;
            JsonVariantConst r = getRenderer(item);
            if (r.isNull()) continue;

            String t = ytmdExtractText(r["title"]);
            String a = ytmdExtractText(r["shortBylineText"]);
            if (a.length() == 0) a = ytmdExtractText(r["author"]);
            if (a.length() == 0) a = ytmdExtractText(r["artist"]);

            dev->queue[dev->queueSize].title = t;
            dev->queue[dev->queueSize].artist = a;
            dev->queue[dev->queueSize].album = "";
            dev->queue[dev->queueSize].duration = "";
            dev->queue[dev->queueSize].albumArtURL = "";
            dev->queue[dev->queueSize].trackNumber = i + 1;  // 1-based
            dev->queueSize++;
        }

        // If playlist screen is open, repaint from the updated cache.
        if (lv_screen_active() == scr_queue) {
            refreshQueueList();
        }
    }

    if (selectedPos < 0 || selectedPos + 1 >= (int)items.size()) {
        YTMD_QUEUE_LOG("[YTMD/QUEUE] selected=%d items=%d (no next)\n", selectedPos, (int)items.size());
        applyYtmdNextTrackLine("");
        return true;
    }

    JsonObjectConst nextItem = items[selectedPos + 1].as<JsonObjectConst>();
    JsonVariantConst nr = getRenderer(nextItem);
    if (nr.isNull()) {
        applyYtmdNextTrackLine("");
        return true;
    }

    String title = ytmdExtractText(nr["title"]);
    String artist = ytmdExtractText(nr["shortBylineText"]);
    String line = title;
    if (artist.length() > 0) {
        if (line.length() > 0) line += " - ";
        line += artist;
    }
    static String s_last_queue_log_line = "";
    if (line != s_last_queue_log_line) {
        YTMD_QUEUE_LOG("[YTMD/QUEUE] next: %s\n", line.c_str());
        s_last_queue_log_line = line;
    }
    applyYtmdNextTrackLine(line);
    return true;
}

static String nextRepeatMode(const String& current) {
    if (current == "NONE") return "ALL";
    if (current == "ALL") return "ONE";
    return "NONE";
}

// Polls lightweight control-state endpoints so icons stay in sync even when
// /song payload omits queue/repeat fields.
static void pollAndApplyYtmdControlStates(const char* authHeader) {
    if (!authHeader || !authHeader[0]) return;

    auto getJson = [&](const char* path, String* outResp) -> bool {
        if (!path || !outResp) return false;
        char url[192];
        snprintf(url, sizeof(url), "http://%s:%d%s",
                 ytmd_ip.c_str(),
                 (ytmd_port > 0 ? ytmd_port : YTMD_DEFAULT_PORT),
                 path);
        HTTPClient http;
        http.begin(url);
        http.addHeader("Authorization", authHeader);
        http.setTimeout(450);
        int code = http.GET();
        if (code != 200) {
            http.end();
            return false;
        }
        *outResp = http.getString();
        http.end();
        return true;
    };

    String shuffleResp;
    if (getJson("/api/v1/shuffle", &shuffleResp)) {
        DynamicJsonDocument doc(128);
        if (!deserializeJson(doc, shuffleResp)) {
            JsonVariantConst root = doc.as<JsonVariantConst>();
            bool shuffle = parseJsonBoolFlexible(root["state"], ui_shuffle);
            if (shuffle != ui_shuffle) applyYtmdShuffleState(shuffle);
        }
    }

    String repeatResp;
    if (getJson("/api/v1/repeat-mode", &repeatResp)) {
        DynamicJsonDocument doc(128);
        if (!deserializeJson(doc, repeatResp)) {
            JsonVariantConst root = doc.as<JsonVariantConst>();
            const char* mode = root["mode"] | (const char*)nullptr;
            if (mode && mode[0]) {
                String rm(mode);
                if (rm != ui_repeat) applyYtmdRepeatMode(rm);
            }
        }
    }
}

// ============================================================================
// Playback Event Handlers
// ============================================================================
void ev_play(lv_event_t* e) {
    if (isYtmdMode()) {
        // Newer pear-desktop: /toggle-play
        // Older builds may expose /play-pause.
        int code = ytmdApiPostWithFallback("/api/v1/toggle-play", nullptr,
                                           "/api/v1/play-pause", nullptr);
        if (code >= 200 && code < 300) {
            applyYtmdPlayState(!ui_playing);  // optimistic
        }
        return;
    }
    SonosDevice* d = sonos.getCurrentDevice();
    if (d) d->isPlaying ? sonos.pause() : sonos.play();
}

void ev_prev(lv_event_t* e) {
    if (isYtmdMode()) {
        ytmdApiPost("/api/v1/previous");
        return;
    }
    sonos.previous();
}

void ev_next(lv_event_t* e) {
    if (isYtmdMode()) {
        ytmdApiPost("/api/v1/next");
        return;
    }
    sonos.next();
}

void ev_shuffle(lv_event_t* e) {
    if (isYtmdMode()) {
        int code = ytmdApiPost("/api/v1/shuffle", "{}");
        if (code >= 200 && code < 300) {
            applyYtmdShuffleState(!ui_shuffle);  // optimistic
        }
        return;
    }
    SonosDevice* d = sonos.getCurrentDevice();
    if (d) sonos.setShuffle(!d->shuffleMode);
}

void ev_repeat(lv_event_t* e) {
    if (isYtmdMode()) {
        // Newer pear-desktop: POST /switch-repeat with {"iteration":1}
        // Older builds may expose POST /repeat-mode.
        int code = ytmdApiPostWithFallback("/api/v1/switch-repeat", "{\"iteration\":1}",
                                           "/api/v1/repeat-mode", "{}");
        if (code >= 200 && code < 300) {
            applyYtmdRepeatMode(nextRepeatMode(ui_repeat));  // optimistic
        }
        return;
    }
    SonosDevice* d = sonos.getCurrentDevice();
    if (!d) return;
    if (d->repeatMode == "NONE") sonos.setRepeat("ALL");
    else if (d->repeatMode == "ALL") sonos.setRepeat("ONE");
    else sonos.setRepeat("NONE");
}

void ev_progress(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        dragging_prog = true;
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (isYtmdMode()) {
            if (ytmd_duration_seconds > 0) {
                int seekSec = (lv_slider_get_value(slider_progress) * ytmd_duration_seconds) / 100;
                YTMD_CMD_LOG("[YTMD/SEEK] slider=%d dur=%d seek=%d\n",
                             lv_slider_get_value(slider_progress),
                             ytmd_duration_seconds,
                             seekSec);
                char body[32];
                snprintf(body, sizeof(body), "{\"seconds\":%d}", seekSec);
                ytmdApiPost("/api/v1/seek-to", body);
                // Optimistic local update: avoid waiting up to next poll interval
                // before slider/time labels reflect the new seek target.
                s_ytmd_elapsed_sec = (float)seekSec;
                s_ytmd_pos_ts = millis();
            }
        } else {
            SonosDevice* d = sonos.getCurrentDevice();
            if (d && d->durationSeconds > 0)
                sonos.seek((lv_slider_get_value(slider_progress) * d->durationSeconds) / 100);
        }
        dragging_prog = false;
    } else if (code == LV_EVENT_CANCEL) {
        dragging_prog = false;
    }
}

void ev_vol_slider(lv_event_t* e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        dragging_vol = true;
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        // Apply once per drag gesture (PRESS_LOST can happen when finger exits widget).
        if (!dragging_vol) return;

        if (isYtmdMode()) {
            int vol = lv_slider_get_value(slider_vol);
            ui_vol = vol;  // optimistic UI sync; server poll will correct if needed
            s_ytmd_local_volume_target = vol;
            s_ytmd_local_volume_set_ms = millis();
            YTMD_CMD_LOG("[VOL] YTMD set request: vol=%d event=%d\n", vol, (int)code);
            char body[24];
            snprintf(body, sizeof(body), "{\"volume\":%d}", vol);
            ytmdApiPost("/api/v1/volume", body);
        } else {
            int vol = lv_slider_get_value(slider_vol);
            YTMD_CMD_LOG("[VOL] Sonos set request: vol=%d event=%d\n", vol, (int)code);
            sonos.setVolume(lv_slider_get_value(slider_vol));
        }
        dragging_vol = false;
    } else if (code == LV_EVENT_CANCEL) {
        dragging_vol = false;
    }
}

void ev_mute(lv_event_t* e) {
    if (isYtmdMode()) {
        // pear-desktop API: POST /toggle-mute
        int code = ytmdApiPostWithFallback("/api/v1/toggle-mute", nullptr,
                                           "/api/v1/mute-unmute", nullptr);
        if (code >= 200 && code < 300) {
            applyYtmdMuteState(!ui_muted);  // optimistic
        }
        return;
    }
    SonosDevice* d = sonos.getCurrentDevice();
    if (d) sonos.setMute(!d->isMuted);
}

void ev_queue_item(lv_event_t* e) {
    static uint32_t last_click_ms = 0;
    uint32_t now = millis();
    if (now - last_click_ms < 1500) return;  // debounce: ignore rapid repeat taps
    last_click_ms = now;
    int trackNum = (int)(intptr_t)lv_obj_get_user_data((lv_obj_t*)lv_event_get_target(e));
    sonos.playQueueItem(trackNum);
    lv_screen_load(scr_main);
}

// ============================================================================
// Navigation Event Handlers
// ============================================================================
void ev_devices(lv_event_t* e) {
    lv_screen_load(scr_devices);
}

void ev_queue(lv_event_t* e) {
    // Show cached data immediately.
    // Sonos: request a fresh windowed fetch in polling task (no SOAP on UI thread).
    // YTMD: request lightweight queue refresh via YTMD polling path.
    SonosDevice* d = sonos.getCurrentDevice();
    if (isYtmdMode()) {
        ytmd_queue_fetch_requested = true;
    } else {
        int start = 0;
        if (d && d->currentTrackNumber > 0) {
            start = d->currentTrackNumber - SONOS_QUEUE_BATCH_SIZE / 2;
            if (start < 0) start = 0;
            if (d->totalTracks > 0 && start + SONOS_QUEUE_BATCH_SIZE > d->totalTracks)
                start = d->totalTracks - SONOS_QUEUE_BATCH_SIZE;
            if (start < 0) start = 0;
        }
        queue_fetch_start_index = start;
        queue_fetch_requested   = true;
    }
    refreshQueueList();
    lv_screen_load(scr_queue);
}

void ev_settings(lv_event_t* e) {
    lv_screen_load(scr_settings);
}

void ev_back_main(lv_event_t* e) {
    lv_screen_load(scr_main);
}

void ev_back_settings(lv_event_t* e) {
    lv_screen_load(scr_settings);
}

void ev_groups(lv_event_t* e) {
    sonos.updateGroupInfo();
    refreshGroupsList();
    lv_screen_load(scr_groups);
}

// ============================================================================
// Speaker Discovery Event Handler
// ============================================================================
void ev_discover(lv_event_t* e) {
    Serial.println("[SCAN] Scan button pressed");

    // Disable scan button during discovery
    if (btn_sonos_scan) {
        lv_obj_add_state(btn_sonos_scan, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(btn_sonos_scan, lv_color_hex(0x555555), LV_STATE_DISABLED);
    }

    // Show spinner
    if (spinner_scan) {
        Serial.println("[SCAN] Showing spinner");
        lv_obj_remove_flag(spinner_scan, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(spinner_scan);  // Bring to front
    } else {
        Serial.println("[SCAN] ERROR: spinner_scan is NULL!");
    }

    lv_label_set_text(lbl_status, "Scanning for speakers...");
    lv_obj_set_style_text_color(lbl_status, COL_ACCENT, 0);
    lv_obj_clean(list_devices);
    lv_refr_now(NULL);  // Force immediate screen refresh

    int cnt = sonos.discoverDevices();

    // Hide spinner
    if (spinner_scan) {
        lv_obj_add_flag(spinner_scan, LV_OBJ_FLAG_HIDDEN);
    }

    // Re-enable scan button
    if (btn_sonos_scan) {
        lv_obj_clear_state(btn_sonos_scan, LV_STATE_DISABLED);
    }

    if (cnt == 0) {
        lv_label_set_text(lbl_status, MDI_ALERT " No Sonos devices found on network");
        lv_obj_set_style_text_color(lbl_status, lv_color_hex(0xFF6B6B), 0);
        return;
    }

    if (cnt < 0) {
        lv_label_set_text(lbl_status, MDI_ALERT " Discovery failed - check network");
        lv_obj_set_style_text_color(lbl_status, lv_color_hex(0xFF6B6B), 0);
        return;
    }

    lv_label_set_text_fmt(lbl_status, MDI_CHECK " Found %d Sonos device%s", cnt, cnt == 1 ? "" : "s");
    lv_obj_set_style_text_color(lbl_status, lv_color_hex(0x4ECB71), 0);
    refreshDeviceList();
}

// ============================================================================
// WiFi Event Handlers
// ============================================================================
void ev_wifi_scan(lv_event_t* e) {
    // Disable button and show loading state
    if (btn_wifi_scan) {
        lv_obj_add_state(btn_wifi_scan, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(btn_wifi_scan, lv_color_hex(0x555555), LV_STATE_DISABLED);
    }
    if (lbl_scan_text) {
        lv_label_set_text(lbl_scan_text, MDI_REFRESH "  Scanning...");
    }

    lv_label_set_text(lbl_wifi_status, "Scanning for networks...");
    lv_obj_set_style_text_color(lbl_wifi_status, COL_ACCENT, 0);
    lv_obj_clean(list_wifi);
    // Hide password strip if visible from a previous selection
    lv_obj_add_flag(pw_strip, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    // Show spinner
    if (spinner_wifi_scan) {
        lv_obj_remove_flag(spinner_wifi_scan, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(spinner_wifi_scan);
    }
    lv_timer_handler();  // Update UI immediately

    WiFi.disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));

    int n = WiFi.scanNetworks();
    wifiNetworkCount = min(n, 20);

    // Hide spinner, re-enable button
    if (spinner_wifi_scan) lv_obj_add_flag(spinner_wifi_scan, LV_OBJ_FLAG_HIDDEN);
    if (btn_wifi_scan)     lv_obj_clear_state(btn_wifi_scan, LV_STATE_DISABLED);
    if (lbl_scan_text)     lv_label_set_text(lbl_scan_text, MDI_REFRESH " Scan");

    if (n == 0) {
        lv_label_set_text(lbl_wifi_status, MDI_ALERT " No networks found");
        lv_obj_set_style_text_color(lbl_wifi_status, lv_color_hex(0xFF6B6B), 0);
        return;
    }

    if (n < 0) {
        lv_label_set_text(lbl_wifi_status, MDI_ALERT " Scan failed - try again");
        lv_obj_set_style_text_color(lbl_wifi_status, lv_color_hex(0xFF6B6B), 0);
        return;
    }

    lv_label_set_text_fmt(lbl_wifi_status, MDI_CHECK " Found %d network%s", n, n == 1 ? "" : "s");
    lv_obj_set_style_text_color(lbl_wifi_status, lv_color_hex(0x4ECB71), 0);

    // Deduplicate: for mesh networks (same SSID, multiple APs) keep best RSSI only
    std::vector<int> unique_indices;
    for (int i = 0; i < wifiNetworkCount; i++) {
        String ssid = WiFi.SSID(i);
        int32_t rssi = WiFi.RSSI(i);
        bool found = false;
        for (int& j : unique_indices) {
            if (WiFi.SSID(j) == ssid) {
                if (rssi > WiFi.RSSI(j)) j = i;  // keep stronger signal
                found = true;
                break;
            }
        }
        if (!found) unique_indices.push_back(i);
    }
    wifiNetworkCount = min((int)unique_indices.size(), 20);

    for (int ui = 0; ui < wifiNetworkCount; ui++) {
        int i = unique_indices[ui];
        wifiNetworks[ui] = WiFi.SSID(i);
        int32_t rssi = WiFi.RSSI(i);

        // Icon color only: green=strong, accent=medium, red=weak
        lv_color_t icon_color;
        if      (rssi > -60) icon_color = lv_color_hex(0x4ECB71);
        else if (rssi > -75) icon_color = COL_ACCENT;
        else                 icon_color = lv_color_hex(0xFF6B6B);

        lv_obj_t* btn = lv_btn_create(list_wifi);
        lv_obj_set_size(btn, lv_pct(100), 50);
        lv_obj_set_user_data(btn, (void*)(intptr_t)ui);
        lv_obj_set_style_bg_color(btn, COL_CARD, 0);
        lv_obj_set_style_bg_color(btn, COL_BTN, LV_STATE_PRESSED);
        lv_obj_set_style_radius(btn, 10, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, [](lv_event_t* e) {
            int idx = (int)(intptr_t)lv_obj_get_user_data((lv_obj_t*)lv_event_get_target(e));
            selectedSSID = wifiNetworks[idx];
            // Show password strip + update SSID label
            lv_label_set_text(lbl_pw_ssid, selectedSSID.c_str());
            lv_obj_clear_flag(pw_strip, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text_fmt(lbl_wifi_status, MDI_WIFI " %s", selectedSSID.c_str());
            lv_obj_set_style_text_color(lbl_wifi_status, COL_TEXT, 0);
            lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
        }, LV_EVENT_CLICKED, NULL);

        lv_obj_t* icon = lv_label_create(btn);
        lv_label_set_text(icon, MDI_WIFI);
        lv_obj_set_style_text_font(icon, &lv_font_mdi_16, 0);
        lv_obj_set_style_text_color(icon, icon_color, 0);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 10, 0);

        lv_obj_t* ssid_lbl = lv_label_create(btn);
        lv_label_set_text(ssid_lbl, wifiNetworks[ui].c_str());
        lv_obj_set_style_text_color(ssid_lbl, COL_TEXT, 0);
        lv_obj_set_style_text_font(ssid_lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_width(ssid_lbl, lv_pct(80));
        lv_label_set_long_mode(ssid_lbl, LV_LABEL_LONG_DOT);
        lv_obj_align(ssid_lbl, LV_ALIGN_LEFT_MID, 36, 0);
    }
    WiFi.scanDelete();
}

void ev_wifi_connect(lv_event_t* e) {
    if (selectedSSID.length() == 0) {
        lv_label_set_text(lbl_wifi_status, MDI_ALERT " Please select a network first");
        lv_obj_set_style_text_color(lbl_wifi_status, lv_color_hex(0xFF6B6B), 0);
        return;
    }

    const char* pwd = lv_textarea_get_text(ta_password);

    // Disable connect button during connection
    if (btn_wifi_connect) {
        lv_obj_add_state(btn_wifi_connect, LV_STATE_DISABLED);
    }

    lv_label_set_text_fmt(lbl_wifi_status, MDI_REFRESH " Connecting to %s...", selectedSSID.c_str());
    lv_obj_set_style_text_color(lbl_wifi_status, COL_ACCENT, 0);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_timer_handler();  // Update UI

    WiFi.disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));
    WiFi.begin(selectedSSID.c_str(), pwd);

    // Non-blocking connection with visual feedback (max 30 seconds — mesh/Orbi can be slow).
    // MUST reset the hardware WDT each iteration: this function runs on mainAppTask which is
    // registered with the 30s WDT. The loop itself takes up to 30s → WDT fires at loop end.
    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries++ < 60) {
        esp_task_wdt_reset();  // Feed WDT — loop runs up to 30s, WDT timeout = 30s
        vTaskDelay(pdMS_TO_TICKS(500));
        lv_timer_handler();  // Keep UI responsive
        lv_label_set_text_fmt(lbl_wifi_status, MDI_REFRESH " Connecting to %s%s",
            selectedSSID.c_str(),
            tries % 4 == 0 ? "..." : tries % 4 == 1 ? ".  " : tries % 4 == 2 ? ".. " : " ..");
    }
    esp_task_wdt_reset();  // Reset after loop exits (NVS write below can take ~100ms)

    // Re-enable button
    if (btn_wifi_connect) {
        lv_obj_clear_state(btn_wifi_connect, LV_STATE_DISABLED);
    }

    if (WiFi.status() == WL_CONNECTED) {
        // Save credentials to NVS
        Serial.printf("[WIFI] Saving credentials to NVS: SSID='%s'\n", selectedSSID.c_str());
        wifiPrefs.putString("ssid", selectedSSID);
        wifiPrefs.putString("pass", pwd);

        // Verify write succeeded
        String verifySSID = wifiPrefs.getString("ssid", "");
        String verifyPass = wifiPrefs.getString("pass", "");

        if (verifySSID == selectedSSID && verifyPass == pwd) {
            Serial.println("[WIFI] Credentials successfully saved and verified in NVS");
        } else {
            Serial.println("[WIFI] WARNING: NVS verification failed! Credentials may not persist.");
        }

        String ip = WiFi.localIP().toString();
        lv_label_set_text_fmt(lbl_wifi_status,
            MDI_WIFI " Connected to %s  (%s)",
            selectedSSID.c_str(), ip.c_str());
        lv_obj_set_style_text_color(lbl_wifi_status, lv_color_hex(0x4ECB71), 0);

        // Hide strip + keyboard, clear password field
        lv_obj_add_flag(pw_strip, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
        lv_textarea_set_text(ta_password, "");
    } else {
        // Determine failure reason
        wl_status_t status = WiFi.status();
        const char* reason = "Unknown error";

        if (status == WL_CONNECT_FAILED) {
            reason = "Authentication failed - check password";
        } else if (status == WL_NO_SSID_AVAIL) {
            reason = "Network not found";
        } else if (status == WL_CONNECTION_LOST) {
            reason = "Connection lost";
        } else if (status == WL_DISCONNECTED) {
            reason = "Connection timeout — check password and try again";
        }

        lv_label_set_text_fmt(lbl_wifi_status, MDI_ALERT " Failed: %s", reason);
        lv_obj_set_style_text_color(lbl_wifi_status, lv_color_hex(0xFF6B6B), 0);
    }
}

// ============================================================================
// OTA Update Functions
// ============================================================================
static void checkForUpdates() {
    if (WiFi.status() != WL_CONNECTED) {
        if (lbl_ota_status) {
            lv_label_set_text(lbl_ota_status, MDI_ALERT " No WiFi connection");
            lv_obj_set_style_text_color(lbl_ota_status, lv_color_hex(0xFF6B6B), 0);
        }
        return;
    }

    // CRITICAL: Prevent rapid clicking - minimum 5 seconds between checks
    // Rapid HTTPS checks exhaust SDIO buffer pool even with cooldowns
    static unsigned long last_check_time = 0;
    unsigned long now = millis();
    if (last_check_time > 0 && (now - last_check_time) < OTA_CHECK_DEBOUNCE_MS) {
        unsigned long wait_sec = (OTA_CHECK_DEBOUNCE_MS - (now - last_check_time)) / 1000 + 1;
        if (lbl_ota_status) {
            lv_label_set_text_fmt(lbl_ota_status, MDI_ALERT " Please wait %lu seconds", wait_sec);
            lv_obj_set_style_text_color(lbl_ota_status, lv_color_hex(0xFFA500), 0);
        }
        return;
    }
    last_check_time = now;

    // Disable check button during check
    if (btn_check_update) lv_obj_add_state(btn_check_update, LV_STATE_DISABLED);

    if (lbl_ota_status) {
        lv_label_set_text(lbl_ota_status, MDI_REFRESH " Checking for updates...");
        lv_obj_set_style_text_color(lbl_ota_status, COL_ACCENT, 0);
    }
    lv_timer_handler();

    WiFiClientSecure client;
    client.setInsecure();  // Skip certificate validation

    HTTPClient http;

    // Choose API endpoint based on channel
    const char* apiUrl;
    if (ota_channel == 0) {
        // Stable: Get only latest non-prerelease
        apiUrl = "https://api.github.com/repos/" GITHUB_REPO "/releases/latest";
        Serial.println("[OTA] Checking Stable channel (latest stable release)");
    } else {
        // Nightly: Get recent releases (GitHub API doesn't sort prereleases first)
        // We'll fetch multiple and filter for the most recent nightly
        apiUrl = "https://api.github.com/repos/" GITHUB_REPO "/releases?per_page=5";
        Serial.println("[OTA] Checking Nightly channel (fetching recent releases)");
    }

    // CRITICAL: Acquire network_mutex BEFORE http.begin() to prevent SDIO overlap
    if (!xSemaphoreTake(network_mutex, pdMS_TO_TICKS(NETWORK_MUTEX_TIMEOUT_MS))) {
        Serial.println("[OTA] Failed to acquire network mutex - check aborted");
        if (lbl_ota_status) {
            lv_label_set_text(lbl_ota_status, MDI_ALERT " Network busy, try again");
            lv_obj_set_style_text_color(lbl_ota_status, lv_color_hex(0xFF6B6B), 0);
        }
        if (btn_check_update) lv_obj_clear_state(btn_check_update, LV_STATE_DISABLED);
        return;
    }

    // CRITICAL: Wait for general SDIO cooldown (200ms since last network op)
    now = millis();
    unsigned long elapsed = now - last_network_end_ms;
    if (last_network_end_ms > 0 && elapsed < 200) {
        vTaskDelay(pdMS_TO_TICKS(200 - elapsed));
    }

    // CRITICAL: Wait for HTTPS-specific cooldown (2000ms since last HTTPS)
    now = millis();
    elapsed = now - last_https_end_ms;
    if (last_https_end_ms > 0 && elapsed < OTA_HTTPS_COOLDOWN_MS) {
        vTaskDelay(pdMS_TO_TICKS(OTA_HTTPS_COOLDOWN_MS - elapsed));
    }

    int httpCode = -1;
    String payload = "";

    // Retry once on connection failure — TLS to api.github.com needs ~114KB DMA;
    // a brief wait lets previous sessions fully release their DMA buffers.
    for (int attempt = 1; attempt <= 2 && httpCode < 0; attempt++) {
        if (attempt > 1) {
            Serial.printf("[OTA] Retry (attempt %d) — free DMA: %d bytes\n",
                          attempt, heap_caps_get_free_size(MALLOC_CAP_DMA));
            vTaskDelay(pdMS_TO_TICKS(3000));
        } else {
            Serial.printf("[OTA] Free DMA before check: %d bytes\n",
                          heap_caps_get_free_size(MALLOC_CAP_DMA));
        }

        http.begin(client, apiUrl);
        http.addHeader("Accept", "application/vnd.github.v3+json");
        http.addHeader("User-Agent", "SonosESP/" FIRMWARE_VERSION);
        http.setTimeout(OTA_CHECK_TIMEOUT_MS);

        httpCode = http.GET();

        if (httpCode == 200) {
            payload = http.getString();
        }

        http.end();
        client.stop();
        vTaskDelay(pdMS_TO_TICKS(OTA_CHECK_CLEANUP_MS));
    }

    // Update timestamps before releasing mutex
    last_network_end_ms = millis();
    last_https_end_ms = millis();

    // Release mutex after ALL network activity including TLS cleanup
    xSemaphoreGive(network_mutex);

    if (btn_check_update) lv_obj_clear_state(btn_check_update, LV_STATE_DISABLED);

    if (httpCode == 200) {
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, payload);

        if (!error) {
            // For nightly channel, search array for first nightly release
            JsonVariant releaseObj;
            if (ota_channel == 1) {
                // Nightly: response is an array, find LATEST nightly release by published_at
                if (doc.is<JsonArray>() && doc.size() > 0) {
                    bool found = false;
                    String latest_published = "";

                    for (JsonVariant release : doc.as<JsonArray>()) {
                        String tag = release["tag_name"].as<String>();
                        // Check if this is a nightly release
                        if (tag.indexOf("-nightly") >= 0) {
                            String published = release["published_at"].as<String>();

                            // Compare published timestamps to find the latest
                            if (!found || published > latest_published) {
                                releaseObj = release;
                                latest_published = published;
                                found = true;
                                Serial.printf("[OTA] Found nightly release: %s (published: %s)\n",
                                            tag.c_str(), published.c_str());
                            }
                        }
                    }
                    if (!found) {
                        Serial.println("[OTA] No nightly releases found in recent releases");
                        if (lbl_ota_status) {
                            lv_label_set_text(lbl_ota_status, MDI_ALERT " No nightly releases found");
                            lv_obj_set_style_text_color(lbl_ota_status, lv_color_hex(0xFF6B6B), 0);
                        }
                        if (lbl_latest_version) {
                            lv_label_set_text(lbl_latest_version, "Latest (Nightly): None");
                        }
                        return;
                    }
                } else {
                    Serial.println("[OTA] Error: Expected array response for nightly channel");
                    if (lbl_ota_status) {
                        lv_label_set_text(lbl_ota_status, MDI_ALERT " No nightly releases found");
                        lv_obj_set_style_text_color(lbl_ota_status, lv_color_hex(0xFF6B6B), 0);
                    }
                    return;
                }
            } else {
                // Stable: response is a single object
                releaseObj = doc.as<JsonVariant>();
            }

            latest_version = releaseObj["tag_name"].as<String>();
            latest_version.replace("v", "");  // Remove 'v' prefix

            bool isPrerelease = releaseObj["prerelease"].as<bool>();
            const char* channelName = ota_channel == 0 ? "Stable" : "Nightly";

            // CRITICAL: Filter out nightly versions from Stable channel
            // A nightly version may have been incorrectly marked as stable (prerelease=false)
            // Always check the tag name to ensure Stable channel only shows stable versions
            if (ota_channel == 0 && latest_version.indexOf("-nightly") >= 0) {
                Serial.printf("[OTA] Skipping nightly version in Stable channel: v%s\n", latest_version.c_str());
                if (lbl_ota_status) {
                    lv_label_set_text(lbl_ota_status, MDI_ALERT " No stable releases found");
                    lv_obj_set_style_text_color(lbl_ota_status, lv_color_hex(0xFF6B6B), 0);
                }
                if (lbl_latest_version) {
                    lv_label_set_text(lbl_latest_version, "Latest (Stable): None");
                }
                return;
            }

            // CRITICAL: Filter out stable versions from Nightly channel
            // Nightly channel should only show prerelease versions with "-nightly" in tag
            if (ota_channel == 1 && latest_version.indexOf("-nightly") < 0) {
                Serial.printf("[OTA] Skipping stable version in Nightly channel: v%s\n", latest_version.c_str());

                // Check if user is already on a nightly version
                String current_version = FIRMWARE_VERSION;
                if (current_version.indexOf("-nightly") >= 0) {
                    // User is on a nightly, and latest release is stable = user is on latest nightly
                    if (lbl_ota_status) {
                        lv_label_set_text(lbl_ota_status, MDI_CHECK " You're on the latest nightly version!");
                        lv_obj_set_style_text_color(lbl_ota_status, lv_color_hex(0x4ECB71), 0);
                    }
                    if (lbl_latest_version) {
                        lv_label_set_text_fmt(lbl_latest_version, "Latest (Nightly): v%s", current_version.c_str());
                    }
                    if (btn_install_update) {
                        lv_obj_add_flag(btn_install_update, LV_OBJ_FLAG_HIDDEN);
                    }
                } else {
                    // User is on stable, no nightlies available
                    if (lbl_ota_status) {
                        lv_label_set_text(lbl_ota_status, MDI_ALERT " No nightly releases found");
                        lv_obj_set_style_text_color(lbl_ota_status, lv_color_hex(0xFF6B6B), 0);
                    }
                    if (lbl_latest_version) {
                        lv_label_set_text(lbl_latest_version, "Latest (Nightly): None");
                    }
                }
                return;
            }

            if (lbl_latest_version) {
                if (isPrerelease && ota_channel == 1) {
                    lv_label_set_text_fmt(lbl_latest_version, "Latest (%s): v%s (prerelease)", channelName, latest_version.c_str());
                } else {
                    lv_label_set_text_fmt(lbl_latest_version, "Latest (%s): v%s", channelName, latest_version.c_str());
                }
            }

            Serial.printf("[OTA] Latest %s version: v%s (prerelease: %s)\n",
                          channelName, latest_version.c_str(), isPrerelease ? "yes" : "no");

            // Find firmware.bin asset
            JsonArray assets = releaseObj["assets"];
            for (JsonObject asset : assets) {
                String name = asset["name"].as<String>();
                if (name.indexOf("firmware.bin") >= 0) {
                    download_url = asset["browser_download_url"].as<String>();
                    // Use HTTPS directly - ESP32-P4 supports it with WiFiClientSecure
                    break;
                }
            }

            // Compare versions
            if (latest_version != FIRMWARE_VERSION) {
                if (lbl_ota_status) {
                    lv_label_set_text_fmt(lbl_ota_status, MDI_DOWNLOAD " Update available: v%s", latest_version.c_str());
                    lv_obj_set_style_text_color(lbl_ota_status, lv_color_hex(0x4ECB71), 0);
                }
                if (btn_install_update) {
                    lv_obj_clear_flag(btn_install_update, LV_OBJ_FLAG_HIDDEN);
                }
            } else {
                if (lbl_ota_status) {
                    lv_label_set_text(lbl_ota_status, MDI_CHECK " You're on the latest version!");
                    lv_obj_set_style_text_color(lbl_ota_status, lv_color_hex(0x4ECB71), 0);
                }
                if (btn_install_update) {
                    lv_obj_add_flag(btn_install_update, LV_OBJ_FLAG_HIDDEN);
                }
            }
        } else {
            if (lbl_ota_status) {
                lv_label_set_text(lbl_ota_status, MDI_ALERT " Failed to parse response");
                lv_obj_set_style_text_color(lbl_ota_status, lv_color_hex(0xFF6B6B), 0);
            }
        }
    } else {
        if (lbl_ota_status) {
            lv_label_set_text_fmt(lbl_ota_status, MDI_ALERT " Check failed (HTTP %d)", httpCode);
            lv_obj_set_style_text_color(lbl_ota_status, lv_color_hex(0xFF6B6B), 0);
        }
    }
}

// Helper: Restore all tasks and state after OTA failure
static void otaRecovery() {
    // Close HTTP/TLS cleanup
    Serial.println("[OTA] === RECOVERY: Restoring normal operation ===");

    // Update HTTPS timestamps so art/lyrics tasks use proper cooldown after restarting.
    // performOTAUpdate() always does HTTPS before calling otaRecovery() on failure,
    // so art/lyrics must not fire HTTPS immediately after.
    last_network_end_ms = millis();
    last_https_end_ms = millis();

    // Hide progress bar and re-enable buttons
    if (bar_ota_progress) {
        lv_obj_add_flag(bar_ota_progress, LV_OBJ_FLAG_HIDDEN);
    }
    if (btn_check_update) lv_obj_clear_state(btn_check_update, LV_STATE_DISABLED);
    if (btn_install_update) lv_obj_clear_state(btn_install_update, LV_STATE_DISABLED);

    // Re-enable WiFi features
    WiFi.setAutoReconnect(true);

    // Clear OTA flag
    if (xSemaphoreTake(ota_progress_mutex, pdMS_TO_TICKS(1000))) {
        ota_in_progress = false;
        xSemaphoreGive(ota_progress_mutex);
    }

    // Resume Sonos background tasks
    sonos.resumeTasks();

    // ALWAYS clear ALL shutdown/abort flags before restarting tasks.
    // These must be cleared unconditionally — tasks can't start cleanly if any
    // flag is still set. Add any new task's flags here when adding new features.
    art_shutdown_requested         = false;
    art_abort_download             = false;
    lyrics_shutdown_requested      = false;
    lyrics_abort_requested         = false;
    clock_bg_shutdown_requested    = false;
    sonos_tasks_shutdown_requested = false;  // resumeTasks() also resets this, belt-and-suspenders

    // Restart album art task if it isn't already running
    if (albumArtTaskHandle == NULL) {
        Serial.println("[OTA] Restarting album art task");
        createArtTask();  // PSRAM stack — frees 20KB internal SRAM for SDIO/WiFi DMA
    }

    Serial.println("[OTA] === Recovery complete ===");
}

// Signals all background tasks to stop, waits up to 12s for clean exit,
// force-kills stragglers, recreates the network mutex, sets ota_in_progress.
// Returns true if any task was force-killed (indicates possible DMA leak).
static bool otaStopTasks() {
    if (lbl_ota_status) {
        lv_label_set_text(lbl_ota_status, MDI_REFRESH " Stopping background tasks...");
    }
    lv_tick_inc(10);
    lv_refr_now(NULL);

    // Signal ALL tasks to stop SIMULTANEOUSLY — do this before any waiting
    // so all tasks start their shutdown paths in parallel at t=0.
    art_abort_download            = true;
    art_shutdown_requested        = true;
    lyrics_shutdown_requested     = true;
    lyrics_abort_requested        = true;
    clock_bg_shutdown_requested   = true;
    sonos_tasks_shutdown_requested = true;

    bool force_killed = false;
    {
        const uint32_t SHUTDOWN_BUDGET_MS = 12000;
        uint32_t shutdown_start = millis();
        Serial.println("[OTA] Waiting for all tasks to exit (parallel)...");

        while (millis() - shutdown_start < SHUTDOWN_BUDGET_MS) {
            if (albumArtTaskHandle == nullptr &&
                lyricsTaskHandle   == nullptr &&
                clockBgTaskHandle  == nullptr) break;
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_task_wdt_reset();
        }

        if (albumArtTaskHandle) {
            Serial.println("[OTA] WARNING: Force-killing art task (possible DMA leak)");
            vTaskDelete(albumArtTaskHandle);
            albumArtTaskHandle = nullptr;
            force_killed = true;
        }
        if (lyricsTaskHandle) {
            Serial.println("[OTA] WARNING: Force-killing lyrics task (possible DMA leak)");
            vTaskDelete(lyricsTaskHandle);
            lyricsTaskHandle = nullptr;
            force_killed = true;
        }
        if (clockBgTaskHandle) {
            Serial.println("[OTA] WARNING: Force-killing clock bg task (possible DMA leak)");
            vTaskDelete(clockBgTaskHandle);
            clockBgTaskHandle = nullptr;
            force_killed = true;
        }
        if (force_killed) {
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_task_wdt_reset();
        }
        Serial.printf("[OTA] Task shutdown in %lums — DMA: %d bytes\n",
                      (unsigned long)(millis() - shutdown_start),
                      heap_caps_get_free_size(MALLOC_CAP_DMA));
    }

    Serial.println("[OTA] Suspending Sonos tasks...");
    sonos.suspendTasks();

    // Recreate network mutex — a force-killed task may have left it poisoned.
    if (network_mutex) {
        vSemaphoreDelete(network_mutex);
        network_mutex = xSemaphoreCreateMutex();
        Serial.println("[OTA] Network mutex recreated (clean state)");
    }

    if (xSemaphoreTake(ota_progress_mutex, pdMS_TO_TICKS(1000))) {
        ota_in_progress = true;
        xSemaphoreGive(ota_progress_mutex);
    }

    return force_killed;
}

// Waits for TIME_WAIT TCP sockets to release DMA after task shutdown.
// If DMA remains below OTA_TARGET_FREE_DMA after OTA_DMA_POLL_MS, saves URL
// to NVS and calls ESP.restart() — does NOT return in that case.
// On success, configures WiFi for the download and returns normally.
static void otaCheckDMA() {
    // ================================================================
    // PHASE 4: CLEAR NETWORK STATE AND VERIFY DMA
    // ================================================================
    // DMA may be low because recent HTTPS sessions (OTA check, lyrics, weather)
    // leave their TCP sockets in TIME_WAIT for ~12s (lwIP: 2×MSL = 2×6s).
    // Each TIME_WAIT socket holds ~5-6KB DMA. With 3 sessions: ~15KB held.
    // Wait up to OTA_DMA_POLL_MS (15s) for them to expire naturally — no WiFi
    // disruption needed. WiFi.disconnect/reconnect is NEVER done here: it
    // destabilises the ESP32-C6 SDIO transport driver and causes download crashes.
    //
    // Only reboot if DMA is STILL insufficient after 15s. That would indicate an
    // mbedTLS DMA leak from a force-killed task — extremely rare with clean shutdown.
    uint32_t free_dma = heap_caps_get_free_size(MALLOC_CAP_DMA);
    Serial.printf("[OTA] DMA after task cleanup: %d bytes (need %d)\n", free_dma, OTA_TARGET_FREE_DMA);

    if (free_dma < OTA_TARGET_FREE_DMA) {
        // TIME_WAIT sockets are still alive — poll until they expire (up to 15s).
        // Exit early if DMA plateaus for 3 consecutive seconds (no more recovery
        // possible — mbedTLS state is permanent until full restart).
        Serial.println("[OTA] Waiting for TIME_WAIT sockets to expire...");
        uint32_t poll_start = millis();
        size_t prev_dma = 0;
        int plateau_count = 0;
        while (millis() - poll_start < OTA_DMA_POLL_MS) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_task_wdt_reset();
            free_dma = heap_caps_get_free_size(MALLOC_CAP_DMA);
            uint32_t elapsed = (millis() - poll_start) / 1000;
            Serial.printf("[OTA] DMA: %d bytes (need %d) — %lus elapsed\n",
                free_dma, OTA_TARGET_FREE_DMA, (unsigned long)elapsed);
            if (lbl_ota_status) {
                lv_label_set_text_fmt(lbl_ota_status,
                    MDI_REFRESH " Freeing memory... (%d/%d KB)",
                    (int)(free_dma / 1024), (int)(OTA_TARGET_FREE_DMA / 1024));
                lv_obj_set_style_text_color(lbl_ota_status, lv_color_hex(0xAAAAAA), 0);
            }
            lv_tick_inc(1000);
            lv_refr_now(NULL);
            if (free_dma >= OTA_TARGET_FREE_DMA) break;
            // Plateau detection: if DMA hasn't changed in 3s, reboot now
            if (free_dma == prev_dma) {
                if (++plateau_count >= OTA_DMA_PLATEAU_COUNT) {
                    Serial.printf("[OTA] DMA plateaued at %d bytes — restarting early\n", free_dma);
                    break;
                }
            } else {
                plateau_count = 0;
            }
            prev_dma = free_dma;
        }
    }

    if (free_dma < OTA_TARGET_FREE_DMA) {
        // Still insufficient after 15s — likely an mbedTLS DMA leak from a
        // force-killed task. Only a full restart can reclaim it.
        // Save URL to NVS; auto-trigger on next boot skips checkForUpdates() HTTPS.
        Serial.printf("[OTA] DMA still insufficient (%d / %d) — restarting\n",
            free_dma, OTA_TARGET_FREE_DMA);
        Preferences prefs;
        prefs.begin(NVS_NAMESPACE, false);
        prefs.putBool(NVS_KEY_OTA_PENDING, true);
        prefs.putString(NVS_KEY_OTA_URL, download_url.c_str());
        prefs.end();
        if (lbl_ota_status) {
            lv_label_set_text(lbl_ota_status, MDI_REFRESH " Restarting to apply update...");
            lv_obj_set_style_text_color(lbl_ota_status, lv_color_hex(0xFFFFFF), 0);
        }
        lv_tick_inc(10);
        lv_refr_now(NULL);
        vTaskDelay(pdMS_TO_TICKS(2000));
        display_set_brightness(0);
        vTaskDelay(pdMS_TO_TICKS(100));
        ESP.restart();
        return;  // unreachable
    }

    // WiFi already connected; disable auto-reconnect and power-save for download
    WiFi.setAutoReconnect(false);
    WiFi.setSleep(false);
}

static void performOTAUpdate() {
    if (download_url.length() == 0) {
        if (lbl_ota_status) {
            lv_label_set_text(lbl_ota_status, MDI_ALERT " No update URL found");
            lv_obj_set_style_text_color(lbl_ota_status, lv_color_hex(0xFF6B6B), 0);
        }
        return;
    }

    // ================================================================
    // PHASE 1: IMMEDIATE UI FEEDBACK
    // ================================================================
    // Show "Preparing..." IMMEDIATELY so user knows button press registered
    if (btn_install_update) lv_obj_add_state(btn_install_update, LV_STATE_DISABLED);
    if (btn_check_update) lv_obj_add_state(btn_check_update, LV_STATE_DISABLED);
    if (lbl_ota_status) {
        lv_label_set_text(lbl_ota_status, MDI_REFRESH " Preparing update...");
        lv_obj_set_style_text_color(lbl_ota_status, COL_ACCENT, 0);
    }
    if (bar_ota_progress) {
        lv_obj_clear_flag(bar_ota_progress, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(bar_ota_progress, 0, LV_ANIM_OFF);
    }
    lv_tick_inc(10);
    lv_refr_now(NULL);  // Force immediate refresh so user sees feedback

    Serial.println("[OTA] ========================================");
    Serial.println("[OTA] PREPARING FOR FIRMWARE UPDATE");
    Serial.println("[OTA] ========================================");

    // ================================================================
    // PHASE 2: WAIT FOR PREVIOUS HTTPS CLEANUP
    // ================================================================
    unsigned long now = millis();
    unsigned long elapsed = now - last_https_end_ms;
    if (last_https_end_ms > 0 && elapsed < OTA_HTTPS_COOLDOWN_MS) {
        unsigned long wait_ms = OTA_HTTPS_COOLDOWN_MS - elapsed;
        Serial.printf("[OTA] Waiting for previous HTTPS cleanup: %lums\n", wait_ms);
        if (lbl_ota_status) {
            lv_label_set_text(lbl_ota_status, MDI_REFRESH " Waiting for network cleanup...");
        }
        lv_tick_inc(10);
        lv_refr_now(NULL);
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }

    // ================================================================
    // PHASE 3: STOP ALL BACKGROUND TASKS
    // ================================================================
    bool force_killed = otaStopTasks();

    // ================================================================
    // PHASE 4: CLEAR NETWORK STATE AND VERIFY DMA
    // ================================================================
    otaCheckDMA();  // polls TIME_WAIT sockets; restarts if DMA still low; sets WiFi mode

    // ================================================================
    // PHASE 5+6: CONNECT AND DOWNLOAD (retry on connection failure)
    // ================================================================
    // TLS handshake allocates ~71KB DMA (mbedTLS context + certificate chain +
    // crypto operation buffers). These remain allocated for the entire download
    // — TLS must stay active to decrypt the incoming firmware stream.
    // Called from setup() boot OTA path: ~125KB pre-TLS → ~54KB post-TLS (safe).
    // The retry loop retries on connection-level failures (stream drops,
    // 0 bytes received). Stall and timeout are fatal — do not retry.
    if (lbl_ota_status) {
        lv_label_set_text(lbl_ota_status, MDI_DOWNLOAD " Connecting to server...");
    }
    if (lbl_ota_progress) {
        lv_label_set_text(lbl_ota_progress, "0%");
    }
    lv_tick_inc(10);
    lv_refr_now(NULL);
    vTaskDelay(pdMS_TO_TICKS(100));

    WiFiClientSecure* clientPtr = nullptr;
    HTTPClient* httpPtr = nullptr;
    int contentLength = 0;
    size_t written = 0;
    uint32_t download_start = 0;
    static uint8_t buff[OTA_BUFFER_SIZE];

    for (int attempt = 1; attempt <= OTA_TLS_MAX_RETRIES; attempt++) {
        written = 0;

        if (attempt > 1) {
            uint32_t wait_sec = (uint32_t)(attempt - 1) * (OTA_TLS_RETRY_DELAY_MS / 1000);
            Serial.printf("[OTA] Connection failed - waiting %lus before retry %d/%d\n",
                (unsigned long)wait_sec, attempt, OTA_TLS_MAX_RETRIES);

            for (uint32_t s = wait_sec; s > 0; s--) {
                if (lbl_ota_status) {
                    lv_label_set_text_fmt(lbl_ota_status,
                        MDI_REFRESH " Retrying in %lus... (%d/%d)",
                        (unsigned long)s, attempt, OTA_TLS_MAX_RETRIES);
                    lv_obj_set_style_text_color(lbl_ota_status, lv_color_hex(0xFFA500), 0);
                }
                lv_tick_inc(1000);
                lv_refr_now(NULL);
                esp_task_wdt_reset();
                vTaskDelay(pdMS_TO_TICKS(1000));
            }

            if (lbl_ota_status) {
                lv_label_set_text_fmt(lbl_ota_status,
                    MDI_DOWNLOAD " Connecting (attempt %d/%d)...", attempt, OTA_TLS_MAX_RETRIES);
                lv_obj_set_style_text_color(lbl_ota_status, COL_ACCENT, 0);
            }
            lv_tick_inc(10);
            lv_refr_now(NULL);
        }

        // --- CONNECT ---
        clientPtr = new WiFiClientSecure();
        clientPtr->setInsecure();
        httpPtr = new HTTPClient();

        Serial.println("[OTA] ========================================");
        Serial.printf("[OTA] DOWNLOAD ATTEMPT %d/%d\n", attempt, OTA_TLS_MAX_RETRIES);
        Serial.printf("[OTA] Free DMA: %d bytes | Free heap: %d bytes\n",
            heap_caps_get_free_size(MALLOC_CAP_DMA), ESP.getFreeHeap());
        Serial.println("[OTA] ========================================");

        // Update UI BEFORE GET() — lv_refr_now() after GET() causes a ~50ms delay during
        // which lwIP buffers 40–50 KB of firmware into DMA-backed TCP receive buffers,
        // consuming DMA needed for AES encryption and the SDIO RX pool (→ AES failure /
        // sdio_push_data_to_queue assert crash). Rendering before TLS connect is safe:
        // no firmware is flowing yet so no unexpected lwIP DMA consumption occurs.
        if (lbl_ota_status) {
            lv_label_set_text(lbl_ota_status, MDI_DOWNLOAD " Downloading firmware...");
            lv_obj_set_style_text_color(lbl_ota_status, COL_ACCENT, 0);
        }
        lv_tick_inc(10);
        lv_refr_now(NULL);

        httpPtr->begin(*clientPtr, download_url);
        httpPtr->setTimeout(60000);
        httpPtr->setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

        int httpCode = httpPtr->GET();
        size_t post_tls_total = heap_caps_get_free_size(MALLOC_CAP_DMA);
        Serial.printf("[OTA] HTTP %d - Post-TLS DMA: %d bytes\n", httpCode, post_tls_total);

        if (httpCode != 200) {
            if (lbl_ota_status) {
                lv_label_set_text_fmt(lbl_ota_status, MDI_ALERT " Download failed (HTTP %d)", httpCode);
                lv_obj_set_style_text_color(lbl_ota_status, lv_color_hex(0xFF6B6B), 0);
            }
            httpPtr->end(); clientPtr->stop();
            delete httpPtr; delete clientPtr;
            httpPtr = nullptr; clientPtr = nullptr;
            otaRecovery();
            return;
        }

        contentLength = httpPtr->getSize();
        if (contentLength <= 0 || contentLength > OTA_MAX_FIRMWARE_SIZE) {
            Serial.printf("[OTA] Invalid firmware size: %d bytes\n", contentLength);
            if (lbl_ota_status) {
                lv_label_set_text(lbl_ota_status, MDI_ALERT " Invalid firmware file");
                lv_obj_set_style_text_color(lbl_ota_status, lv_color_hex(0xFF6B6B), 0);
            }
            httpPtr->end(); clientPtr->stop();
            delete httpPtr; delete clientPtr;
            httpPtr = nullptr; clientPtr = nullptr;
            otaRecovery();
            return;
        }

        Serial.printf("[OTA] Firmware: %d bytes (%.1f KB)\n", contentLength, contentLength / 1024.0);

        // Post-TLS DMA check: if total free DMA is below the threshold, SDIO RX buffers
        // will be starved during download causing an assert crash. Retry the full TLS
        // handshake — the previous session's DMA will be returned to the pool first.
        // Note: heap_caps_get_largest_free_block(MALLOC_CAP_DMA) always returns 0 on
        // ESP32-P4 (DMA heap is managed outside the standard allocator), so we check
        // total free only. If TLS session resumption leaves a fragmented heap and causes
        // an mbedTLS AES failure downstream, the natural retry (written=0 → continue)
        // will start a fresh full handshake from clean ~126KB DMA state.
        if (post_tls_total < OTA_MIN_DMA_AFTER_TLS) {
            Serial.printf("[OTA] Post-TLS DMA too low (%d bytes, need %d) — retrying\n",
                post_tls_total, OTA_MIN_DMA_AFTER_TLS);
            if (lbl_ota_status) {
                lv_label_set_text_fmt(lbl_ota_status,
                    MDI_REFRESH " Low memory after TLS (%d KB) - retrying...",
                    post_tls_total / 1024);
                lv_obj_set_style_text_color(lbl_ota_status, lv_color_hex(0xFFA500), 0);
            }
            lv_tick_inc(10);
            lv_refr_now(NULL);
            httpPtr->end(); clientPtr->stop();
            delete httpPtr; delete clientPtr;
            httpPtr = nullptr; clientPtr = nullptr;
            continue;  // Retry — do not call otaRecovery()
        }

        // --- DOWNLOAD ---
        // Update.begin() is deferred (lazy) — called only on first received byte.
        // This avoids allocating the DMA-backed flash write buffer (~11KB) before we
        // know data actually flows; an early connection drop would otherwise leak that
        // allocation across retry attempts, starving DMA for the next TLS handshake.
        WiFiClient* stream = httpPtr->getStreamPtr();
        int lastPercent = -1;
        uint32_t lastUIUpdate = millis();
        int chunk_count = 0;
        download_start = millis();
        uint32_t last_data_time = millis();
        bool fatal_abort = false;
        bool update_begun = false;

        Serial.printf("[OTA] Downloading... DMA: %d bytes\n", heap_caps_get_free_size(MALLOC_CAP_DMA));

        while (httpPtr->connected() && (written < (size_t)contentLength)) {
            if ((millis() - last_data_time) > OTA_STALL_TIMEOUT_MS) {
                Serial.printf("[OTA] STALL: No data for %ds at %d%% - aborting\n",
                    OTA_STALL_TIMEOUT_MS / 1000, (int)(written * 100 / contentLength));
                if (lbl_ota_status) {
                    lv_label_set_text(lbl_ota_status, MDI_ALERT " Download stalled - network timeout");
                    lv_obj_set_style_text_color(lbl_ota_status, lv_color_hex(0xFF6B6B), 0);
                }
                lv_tick_inc(10);
                lv_refr_now(NULL);
                fatal_abort = true;
                break;
            }

            if ((millis() - download_start) > OTA_DOWNLOAD_TIMEOUT_MS) {
                Serial.printf("[OTA] TIMEOUT: >%ds at %d%% - aborting\n",
                    OTA_DOWNLOAD_TIMEOUT_MS / 1000, (int)(written * 100 / contentLength));
                if (lbl_ota_status) {
                    lv_label_set_text(lbl_ota_status, MDI_ALERT " Download timeout - try again");
                    lv_obj_set_style_text_color(lbl_ota_status, lv_color_hex(0xFF6B6B), 0);
                }
                lv_tick_inc(10);
                lv_refr_now(NULL);
                fatal_abort = true;
                break;
            }

            size_t available = stream->available();
            if (available) {
                last_data_time = millis();

                // Lazy Update.begin() — only allocate the DMA-backed flash write buffer
                // once actual data starts flowing. This prevents ~11KB DMA leaks when the
                // connection drops before any bytes arrive (e.g. AES failure path).
                if (!update_begun) {
                    if (!Update.begin(contentLength)) {
                        Serial.println("[OTA] Update.begin() failed — not enough flash space");
                        if (lbl_ota_status) {
                            lv_label_set_text(lbl_ota_status, MDI_ALERT " Not enough space for OTA");
                            lv_obj_set_style_text_color(lbl_ota_status, lv_color_hex(0xFF6B6B), 0);
                        }
                        lv_tick_inc(10);
                        lv_refr_now(NULL);
                        fatal_abort = true;
                        break;
                    }
                    update_begun = true;
                    Serial.printf("[OTA] Update.begin() OK — DMA: %d bytes\n",
                        heap_caps_get_free_size(MALLOC_CAP_DMA));
                }

                size_t toRead = (available < OTA_READ_SIZE) ? available : OTA_READ_SIZE;
                if (toRead > sizeof(buff)) toRead = sizeof(buff);
                int bytesRead = stream->readBytes(buff, toRead);
                written += Update.write(buff, bytesRead);

                chunk_count++;
                if (chunk_count % OTA_DMA_CHECK_INTERVAL == 0) {
                    size_t cur_free_dma = heap_caps_get_free_size(MALLOC_CAP_DMA);
                    if (cur_free_dma < OTA_DMA_CRITICAL) {
                        vTaskDelay(pdMS_TO_TICKS(80));
                    } else if (cur_free_dma < OTA_DMA_LOW) {
                        vTaskDelay(pdMS_TO_TICKS(30));
                    } else {
                        vTaskDelay(pdMS_TO_TICKS(OTA_BASE_DELAY_MS));
                    }
                } else {
                    vTaskDelay(pdMS_TO_TICKS(OTA_BASE_DELAY_MS));
                }

                esp_task_wdt_reset();

                int percent = (written * 100) / contentLength;
                if (percent != lastPercent) {
                    if (lbl_ota_progress) {
                        lv_label_set_text_fmt(lbl_ota_progress, "%d%%", percent);
                    }
                    if (bar_ota_progress) {
                        lv_bar_set_value(bar_ota_progress, percent, LV_ANIM_OFF);
                    }
                    lastPercent = percent;

                    if (percent % OTA_PROGRESS_LOG_INTERVAL == 0) {
                        uint32_t ui_now = millis();
                        lv_tick_inc(ui_now - lastUIUpdate);
                        lv_refr_now(NULL);
                        lastUIUpdate = ui_now;
                        Serial.printf("[OTA] %d%% (%d/%d bytes) - Free DMA: %d bytes\n",
                            percent, written, contentLength, heap_caps_get_free_size(MALLOC_CAP_DMA));
                    }
                }
            } else {
                esp_task_wdt_reset();
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        }

        if (fatal_abort) {
            // Stall, timeout, or Update.begin() failure — not retryable
            if (update_begun) Update.abort();
            httpPtr->end(); clientPtr->stop();
            delete httpPtr; delete clientPtr;
            otaRecovery();
            return;
        }

        if (written == (size_t)contentLength) {
            break;  // SUCCESS — exit retry loop
        }

        // Connection dropped before completion — retryable
        Serial.printf("[OTA] Attempt %d/%d: %d/%d bytes — %s\n",
            attempt, OTA_TLS_MAX_RETRIES, written, contentLength,
            (attempt < OTA_TLS_MAX_RETRIES) ? "retrying" : "failed");
        if (update_begun) Update.abort();
        httpPtr->end(); clientPtr->stop();
        delete httpPtr; delete clientPtr;
        httpPtr = nullptr; clientPtr = nullptr;

        if (attempt == OTA_TLS_MAX_RETRIES) {
            if (lbl_ota_status) {
                lv_label_set_text(lbl_ota_status, MDI_ALERT " Download failed - try again later");
                lv_obj_set_style_text_color(lbl_ota_status, lv_color_hex(0xFF6B6B), 0);
            }
            lv_tick_inc(10);
            lv_refr_now(NULL);
            otaRecovery();
            return;
        }
    }

    // Guard: all retry attempts exhausted via DMA check (continue on last attempt
    // exits the loop normally without hitting the return above).
    if (written != (size_t)contentLength) {
        Serial.printf("[OTA] All %d attempts failed (written=%d, expected=%d) — recovering\n",
            OTA_TLS_MAX_RETRIES, written, contentLength);
        if (lbl_ota_status) {
            lv_label_set_text(lbl_ota_status, MDI_ALERT " Download failed - try again later");
            lv_obj_set_style_text_color(lbl_ota_status, lv_color_hex(0xFF6B6B), 0);
        }
        lv_tick_inc(10);
        lv_refr_now(NULL);
        otaRecovery();
        return;
    }

    // ================================================================
    // PHASE 7: VERIFY AND INSTALL
    // ================================================================
    // Reaches here only on full successful download (written == contentLength).
    if (bar_ota_progress) lv_bar_set_value(bar_ota_progress, 100, LV_ANIM_OFF);
    if (lbl_ota_progress) lv_label_set_text(lbl_ota_progress, "100%");
    if (lbl_ota_status) lv_label_set_text(lbl_ota_status, MDI_CHECK " Download complete!");
    lv_tick_inc(10);
    lv_refr_now(NULL);

    Serial.printf("[OTA] Download complete: %d bytes in %lus\n", written, (millis() - download_start) / 1000);
    vTaskDelay(pdMS_TO_TICKS(500));

    // START INSTALL
    if (bar_ota_progress) lv_bar_set_value(bar_ota_progress, 0, LV_ANIM_OFF);
    if (lbl_ota_progress) lv_label_set_text(lbl_ota_progress, "");
    if (lbl_ota_status) lv_label_set_text(lbl_ota_status, MDI_REFRESH " Installing & verifying...");
    lv_tick_inc(10);
    lv_refr_now(NULL);

    // Animate install progress
    for (int i = 0; i <= 100; i += 10) {
        if (bar_ota_progress) lv_bar_set_value(bar_ota_progress, i, LV_ANIM_OFF);
        lv_tick_inc(50);
        lv_refr_now(NULL);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (Update.end()) {
        if (Update.isFinished()) {
            // INSTALL COMPLETE - Clean screen and show reboot message
            lv_obj_clean(lv_screen_active());
            lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x000000), 0);

            lv_obj_t *reboot_label = lv_label_create(lv_screen_active());
            lv_label_set_text(reboot_label, "REBOOTING...");
            lv_obj_set_style_text_color(reboot_label, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_text_font(reboot_label, &lv_font_montserrat_24, 0);
            lv_obj_center(reboot_label);

            lv_tick_inc(10);
            lv_refr_now(NULL);
            vTaskDelay(pdMS_TO_TICKS(1000));

            display_set_brightness(0);
            vTaskDelay(pdMS_TO_TICKS(100));

            ESP.restart();
        } else {
            if (lbl_ota_status) {
                lv_label_set_text(lbl_ota_status, MDI_ALERT " Update failed: Not finished");
                lv_obj_set_style_text_color(lbl_ota_status, lv_color_hex(0xFF6B6B), 0);
            }
        }
    } else {
        if (lbl_ota_status) {
            lv_label_set_text_fmt(lbl_ota_status, MDI_ALERT " Update failed: %s", Update.errorString());
            lv_obj_set_style_text_color(lbl_ota_status, lv_color_hex(0xFF6B6B), 0);
        }
    }

    httpPtr->end(); clientPtr->stop();
    delete httpPtr; delete clientPtr;
    otaRecovery();
}

void ev_check_update(lv_event_t* e) {
    checkForUpdates();
}

void ev_install_update(lv_event_t* e) {
    if (download_url.isEmpty()) {
        if (lbl_ota_status) {
            lv_label_set_text(lbl_ota_status, MDI_ALERT " No firmware URL — check for updates first");
            lv_obj_set_style_text_color(lbl_ota_status, lv_color_hex(0xFF6B6B), 0);
        }
        return;
    }

    // Save URL to NVS and restart immediately.
    // The firmware download runs at the START of the next boot, before any background
    // tasks (art, Sonos, lyrics) are created. This gives TLS the full ~125KB DMA
    // headroom it needs. The ~71KB consumed by the TLS handshake leaves ~54KB free —
    // enough for the SDIO RX pool, AES alignment buffers, and Update.begin() buffer.
    //
    // Attempting a live download (tasks running) leaves only ~34KB DMA after TLS,
    // which starves the SDIO RX pool → sdio_push_data_to_queue assert crash.
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putBool(NVS_KEY_OTA_PENDING, true);
    prefs.putString(NVS_KEY_OTA_URL, download_url.c_str());
    prefs.end();

    Serial.println("[OTA] URL saved to NVS — restarting for boot OTA");
    if (lbl_ota_status) {
        lv_label_set_text(lbl_ota_status, MDI_REFRESH " Restarting to install update...");
        lv_obj_set_style_text_color(lbl_ota_status, lv_color_hex(0xFFFFFF), 0);
    }
    lv_tick_inc(10);
    lv_refr_now(NULL);
    vTaskDelay(pdMS_TO_TICKS(1500));  // let user see the message
    display_set_brightness(0);
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP.restart();
}

// Called from loop() when ota_auto_pending is set (device rebooted for OTA due to low DMA).
// Uses the URL saved before reboot - skips checkForUpdates() entirely so no HTTPS session
// consumes DMA before the OTA TLS handshake. This breaks the reboot loop.
void triggerPendingOTA() {
    // Load saved URL - skip checkForUpdates() (its HTTPS session costs ~8KB DMA we can't afford)
    Preferences prefs;
    prefs.begin(NVS_NAMESPACE, false);
    String saved_url = prefs.getString(NVS_KEY_OTA_URL, "");
    prefs.remove(NVS_KEY_OTA_URL);
    prefs.end();

    lv_screen_load(scr_ota);
    lv_tick_inc(10);
    lv_refr_now(NULL);
    vTaskDelay(pdMS_TO_TICKS(500));

    if (saved_url.length() > 0) {
        Serial.printf("[OTA] Auto-trigger: using saved URL (no pre-OTA HTTPS - max DMA preserved)\n");
        download_url = saved_url;
        if (lbl_ota_status) {
            lv_label_set_text(lbl_ota_status, MDI_REFRESH " Resuming update after restart...");
            lv_obj_set_style_text_color(lbl_ota_status, lv_color_hex(0xFFFFFF), 0);
        }
        lv_tick_inc(10);
        lv_refr_now(NULL);
        performOTAUpdate();
    } else {
        // No saved URL (e.g. flag set manually) - fall back to normal check
        Serial.println("[OTA] Auto-trigger: no saved URL - running check");
        checkForUpdates();
        if (download_url.length() > 0) {
            performOTAUpdate();
        }
    }
}

// ============================================================================
// ============================================================================
// updateUI() Sub-Functions (static — implementation details, not in any header)
// ============================================================================

// Handles disconnect/reconnect UI state. Returns true if device is connected
// and updateUI() should continue. Returns false → caller must return immediately.
static bool updateConnectionState(SonosDevice* d) {
    static bool was_connected  = false;
    static bool ui_cleared     = false;
    static bool last_conn_state = false;

    if (d->connected != last_conn_state) {
        Serial.printf("[UI] Connection state changed: %s (errorCount=%d)\n",
                     d->connected ? "CONNECTED" : "DISCONNECTED", d->errorCount);
        last_conn_state = d->connected;
    }

    if (!d->connected) {
        if (was_connected || !ui_cleared) {
            lv_label_set_text(lbl_title, "Device Not Connected");
            lv_label_set_text(lbl_artist, "");
            lv_label_set_text(lbl_album, "");
            lv_label_set_text(lbl_time, "0:00");
            lv_label_set_text(lbl_time_remaining, "0:00");
            lv_slider_set_value(slider_progress, 0, LV_ANIM_OFF);

            lv_obj_add_flag(img_album, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(art_placeholder, LV_OBJ_FLAG_HIDDEN);

            lv_obj_add_flag(img_next_album, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lbl_next_title, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lbl_next_artist, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lbl_next_header, LV_OBJ_FLAG_HIDDEN);

            if (panel_art)   lv_obj_set_style_bg_color(panel_art,   lv_color_hex(0x1a1a1a), 0);
            if (panel_right) lv_obj_set_style_bg_color(panel_right, COL_BG, 0);

            lv_obj_t* lbl = lv_obj_get_child(btn_play, 0);
            lv_label_set_text(lbl, MDI_PAUSE);
            lv_obj_set_style_text_font(lbl, &lv_font_mdi_40, 0);
            lv_obj_center(lbl);

            ui_title = "";
            ui_artist = "";
            was_connected = false;
            ui_cleared = true;
            Serial.println("[UI] Device disconnected - UI cleared");
        }
        return false;  // not connected
    }

    if (d->connected && !was_connected) {
        was_connected = true;
        ui_cleared = false;
        ui_title = "";
        ui_artist = "";
        Serial.println("[UI] Device reconnected - forcing UI refresh");
    }
    return true;  // connected
}

// Displays art or placeholder from the background art task.
// Must be called on the main LVGL thread. Takes art_mutex internally.
static void displayCompletedArt() {
    if (!xSemaphoreTake(art_mutex, 0)) return;

    if (art_ready) {
        // Build art_dsc here on the main thread — same thread as lv_timer_handler() /
        // LVGL renderer — so there is never concurrent read+write of the descriptor.
        // The background art task only writes art_buffer (pixels); we set the header here.
        memset(&art_dsc, 0, sizeof(art_dsc));
        art_dsc.header.w    = ART_SIZE;
        art_dsc.header.h    = ART_SIZE;
        art_dsc.header.cf   = LV_COLOR_FORMAT_RGB565;
        art_dsc.data_size   = ART_SIZE * ART_SIZE * 2;
        art_dsc.data        = (const uint8_t*)art_buffer;
        lv_img_set_src(img_album, &art_dsc);
        lv_obj_set_size(img_album, ART_SIZE, ART_SIZE);
        lv_obj_center(img_album);
        lv_obj_remove_flag(img_album, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(art_placeholder, LV_OBJ_FLAG_HIDDEN);
        art_ready = false;
        art_show_placeholder = false;
    } else if (art_show_placeholder) {
        lv_obj_add_flag(img_album, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(art_placeholder, LV_OBJ_FLAG_HIDDEN);
        art_show_placeholder = false;
    }
    if (blur_bg_ready && img_blur_bg && blur_bg_buf) {
        memset(&blur_bg_dsc, 0, sizeof(blur_bg_dsc));
        blur_bg_dsc.header.w  = 800;
        blur_bg_dsc.header.h  = 480;
        blur_bg_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        blur_bg_dsc.data_size = 800 * 480 * 2;
        blur_bg_dsc.data      = (const uint8_t*)blur_bg_buf;
        lv_img_set_src(img_blur_bg, &blur_bg_dsc);
        lv_obj_remove_flag(img_blur_bg, LV_OBJ_FLAG_HIDDEN);
        blur_bg_ready = false;
    }
    if (color_ready) {
        // Restore progress bar + button pressed accent colors from dominant art color.
        // Panel bg changes in setBackgroundColor are invisible (panels are transparent)
        // but slider_progress indicator/knob + button pressed highlights still update.
        setBackgroundColor(dominant_color);
        color_ready = false;
    }
    xSemaphoreGive(art_mutex);
}

// Updates the "Next Up" track labels from the cached queue.
static void updateNextTrackUI(SonosDevice* d) {
    static String last_next_title = "";

    if (!d->isRadioStation && !d->isLineIn && !d->isTvAudio && d->queueSize > 0 && d->currentTrackNumber > 0) {
        int nextIdx = -1;

        // Find next track after current
        for (int i = 0; i < d->queueSize; i++) {
            if (d->queue[i].trackNumber == d->currentTrackNumber + 1) {
                nextIdx = i;
                break;
            }
        }

        // If we're on last track and repeat is on, show first track
        if (nextIdx < 0 && (d->repeatMode == "ALL" || d->repeatMode == "ONE")) {
            for (int i = 0; i < d->queueSize; i++) {
                if (d->queue[i].trackNumber == 1) {
                    nextIdx = i;
                    break;
                }
            }
        }

        if (nextIdx >= 0 && d->queue[nextIdx].title.length() > 0) {
            String nextTitle = d->queue[nextIdx].title;
            if (nextTitle != last_next_title) {
                lv_label_set_text(lbl_next_title, d->queue[nextIdx].title.c_str());
                lv_label_set_text(lbl_next_artist, d->queue[nextIdx].artist.c_str());
                lv_obj_clear_flag(lbl_next_header, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(lbl_next_title, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(lbl_next_artist, LV_OBJ_FLAG_HIDDEN);
                last_next_title = nextTitle;
            }
        } else if (nextIdx < 0) {
            // Only hide if next track is truly unavailable (not just temporarily)
            if (last_next_title != "") {
                lv_obj_add_flag(lbl_next_header, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(lbl_next_title, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(lbl_next_artist, LV_OBJ_FLAG_HIDDEN);
                last_next_title = "";
            }
        }
    } else {
        if (last_next_title != "") {
            lv_obj_add_flag(lbl_next_header, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lbl_next_title, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(lbl_next_artist, LV_OBJ_FLAG_HIDDEN);
            last_next_title = "";
        }
    }
}

static String extractYtmdArtUrl(JsonVariantConst root) {
    String artUrl = "";

    const char* cover = root["cover"];
    if (cover && cover[0]) artUrl = String(cover);

    if (artUrl.length() == 0) {
        const char* imageSrc = root["imageSrc"];
        if (imageSrc && imageSrc[0]) artUrl = String(imageSrc);
    }

    if (artUrl.length() == 0) {
        JsonArrayConst thumbs = root["thumbnails"].as<JsonArrayConst>();
        if (!thumbs.isNull()) {
            const char* best = nullptr;
            uint32_t bestW = 0;
            for (JsonVariantConst t : thumbs) {
                const char* u = t["url"];
                if (!u || !u[0]) continue;
                uint32_t w = t["width"] | 0u;
                if (w >= 300 && (!best || bestW == 0 || w < bestW)) {
                    best = u;
                    bestW = w;
                } else if (!best) {
                    best = u;
                }
            }
            if (best) artUrl = String(best);
        }
    }

    if (artUrl.length() == 0) {
        JsonArrayConst thumbs = root["thumbnail"]["thumbnails"].as<JsonArrayConst>();
        if (!thumbs.isNull()) {
            for (JsonVariantConst t : thumbs) {
                const char* u = t["url"];
                if (u && u[0]) { artUrl = String(u); break; }
            }
        }
    }

    if (artUrl.length() == 0) {
        JsonObjectConst track = root["track"].as<JsonObjectConst>();
        if (!track.isNull()) {
            const char* trackCover = track["cover"];
            if (trackCover && trackCover[0]) artUrl = String(trackCover);
            if (artUrl.length() == 0) {
                JsonArrayConst thumbs = track["thumbnail"]["thumbnails"].as<JsonArrayConst>();
                if (!thumbs.isNull()) {
                    for (JsonVariantConst t : thumbs) {
                        const char* u = t["url"];
                        if (u && u[0]) { artUrl = String(u); break; }
                    }
                }
            }
        }
    }

    if (artUrl.length() > 0 &&
        (artUrl.indexOf("googleusercontent.com") > 0 ||
         artUrl.indexOf("ytimg.com") > 0 ||
         artUrl.indexOf("ggpht.com") > 0)) {
        int eqPos = artUrl.lastIndexOf('=');
        if (eqPos > 0 && (eqPos + 1) < (int)artUrl.length()) {
            char suffix = artUrl.charAt(eqPos + 1);
            if (suffix == 'w' || suffix == 's') {
                // Balance stability and visual quality for Google-hosted artwork.
                artUrl = artUrl.substring(0, eqPos) + "=w300-h300";
            }
        }
    }

    // ytimg path-style thumbnails (e.g. /hq720.jpg) don't use '=w..-h..' params.
    // Normalize those to mqdefault (~320x180) to keep payload moderate and stable.
    if (artUrl.indexOf("i.ytimg.com/vi/") > 0) {
        artUrl.replace("/maxresdefault.jpg", "/mqdefault.jpg");
        artUrl.replace("/sddefault.jpg", "/mqdefault.jpg");
        artUrl.replace("/hq720.jpg", "/mqdefault.jpg");
        artUrl.replace("/hq720_live.jpg", "/mqdefault.jpg");
        artUrl.replace("/hqdefault.jpg", "/mqdefault.jpg");
    }

    return artUrl;
}

static bool isYtmdVirtualDevice(const SonosDevice* d) {
    return d && d->rinconID == "YTMD_VIRTUAL";
}

// Fallback path for builds where no Sonos device is active:
// poll pear-desktop directly and feed album art into the existing art task.
// Uses short timeout + exponential backoff on failure to avoid blocking the UI task.
static void maybePollYtmdArtFallback() {
    static unsigned long lastPollMs = 0;
    static unsigned long lastVolPollMs = 0;
    static unsigned long lastCtlPollMs = 0;
    static unsigned long lastNextPollMs = 0;
    static String lastNextTrackKey = "";
    static String lastRequestedYtmdArt = "";
    static String lastProgressTrackKey = "";
    static uint32_t noDeviceLogGate = 0;
    static int consecutiveFailures = 0;
    static uint32_t skipLogGate = 0;
    static uint32_t rawJsonLogGate = 0;
    static uint32_t metaLogGate = 0;
    static uint32_t parseLogGate = 0;
    static uint32_t staleElapsedLogGate = 0;
    // Backoff intervals (ms): 3s, 10s, 30s, 60s — caps at 60s after 4+ failures
    static const uint32_t kBackoffMs[] = {3000, 10000, 30000, 60000};
    static const uint32_t kVolumePollMs = 1200;
    static const uint32_t kControlPollMs = 1500;
    // /queue can be heavy; prioritize track-change refresh and keep periodic sync sparse.
    static const uint32_t kNextPollMs = 60000;

    const uint32_t now = millis();

    if (ytmd_ip.length() == 0 || ytmd_token.length() == 0) {
        if (now - skipLogGate > 8000) {
            YTMD_FB_LOG("[YTMD/FB] skip: config missing (ip=%d token=%d)\n",
                        (int)(ytmd_ip.length() > 0), (int)(ytmd_token.length() > 0));
            skipLogGate = now;
        }
        return;
    }
    if (WiFi.status() != WL_CONNECTED) {
        if (now - skipLogGate > 8000) {
            YTMD_FB_LOG("[YTMD/FB] skip: wifi disconnected (status=%d)\n", (int)WiFi.status());
            skipLogGate = now;
        }
        return;
    }
    if (art_download_in_progress) {
        if (now - skipLogGate > 8000) {
            YTMD_FB_LOG_LN("[YTMD/FB] skip: art download in progress");
            skipLogGate = now;
        }
        return;
    }

    char auth[300];
    snprintf(auth, sizeof(auth), "Bearer %s", ytmd_token.c_str());

    // Refresh volume independently from /song backoff.
    if (now - lastVolPollMs >= kVolumePollMs) {
        if (network_mutex && xSemaphoreTake(network_mutex, pdMS_TO_TICKS(180)) == pdTRUE) {
            pollAndApplyYtmdVolumeState(auth);
            xSemaphoreGive(network_mutex);
            lastVolPollMs = now;
        }
    }

    // Refresh shuffle/repeat UI independently from /song payload shape/backoff.
    if (now - lastCtlPollMs >= kControlPollMs) {
        if (network_mutex && xSemaphoreTake(network_mutex, pdMS_TO_TICKS(220)) == pdTRUE) {
            pollAndApplyYtmdControlStates(auth);
            xSemaphoreGive(network_mutex);
            lastCtlPollMs = now;
        }
    }

    int backoffIdx = consecutiveFailures < 4 ? consecutiveFailures : 3;
    if (now - lastPollMs < kBackoffMs[backoffIdx]) {
        if (now - skipLogGate > 8000) {
            YTMD_FB_LOG("[YTMD/FB] skip: backoff (failures=%d wait=%lu)\n",
                        consecutiveFailures, (unsigned long)kBackoffMs[backoffIdx]);
            skipLogGate = now;
        }
        return;
    }
    lastPollMs = now;

    if (!network_mutex || xSemaphoreTake(network_mutex, pdMS_TO_TICKS(120)) != pdTRUE) {
        if (now - skipLogGate > 8000) {
            YTMD_FB_LOG_LN("[YTMD/FB] skip: network mutex busy");
            skipLogGate = now;
        }
        return;
    }

    const char* paths[] = {"/api/v1/song", "/api/v1/song-info"};
    bool gotArt = false;

    for (size_t i = 0; i < (sizeof(paths) / sizeof(paths[0])); ++i) {
        char url[192];
        snprintf(url, sizeof(url), "http://%s:%d%s",
                 ytmd_ip.c_str(),
                 (ytmd_port > 0 ? ytmd_port : YTMD_DEFAULT_PORT),
                 paths[i]);

        HTTPClient http;
        http.begin(url);
        http.addHeader("Authorization", auth);
        // Short timeout: local network requests should respond in <500ms.
        // Long timeout (2500ms) on the UI task blocks LVGL and freezes touch.
        http.setTimeout(600);
        int code = http.GET();
        YTMD_FB_LOG("[YTMD/FB] GET %s -> HTTP %d\n", url, code);

        if (code == 404) {
            http.end();
            continue;
        }
        if (code != 200) {
            // Connection failure (HTTP -1) or unexpected error — increment backoff
            consecutiveFailures++;
            http.end();
            break;
        }

        String resp = http.getString();
        http.end();

        // Debug: periodic raw JSON to verify current payload shape.
        if (now - rawJsonLogGate > 10000) {
            YTMD_FB_LOG("[YTMD/FB] RAW JSON (first 600): %.600s\n", resp.c_str());
            rawJsonLogGate = now;
        }

        // Filter: must include every field we intend to read.
        // pear-desktop structure: { track:{title,author,cover,duration}, player:{isPaused,
        //   seekbarCurrentPosition,repeatType,queue:{shuffleEnabled}} }
        // 1024 bytes is enough for this many filter entries.
        StaticJsonDocument<1024> filter;
        // Art URL candidates
        filter["cover"] = true;
        filter["imageSrc"] = true;
        filter["thumbnails"][0]["url"] = true;
        filter["thumbnails"][0]["width"] = true;
        filter["thumbnail"]["thumbnails"][0]["url"] = true;
        filter["track"]["cover"] = true;
        filter["track"]["thumbnail"]["thumbnails"][0]["url"] = true;
        // pear-desktop track fields (author OR artist depending on version)
        filter["track"]["title"] = true;
        filter["track"]["author"] = true;
        filter["track"]["artist"] = true;   // alt field name
        filter["track"]["duration"] = true; // may be seconds or ms — handled below
        filter["track"]["durationSeconds"] = true;
        // pear-desktop player object
        filter["player"]["isPaused"] = true;
        filter["player"]["isPlaying"] = true;
        filter["player"]["seekbarCurrentPosition"] = true;
        filter["player"]["seekbarCurrentPositionHuman"] = true; // "M:SS" string
        filter["player"]["statePercent"] = true;   // 0.0-1.0 fraction fallback
        filter["player"]["duration"] = true;
        filter["player"]["elapsedSeconds"] = true;
        filter["player"]["repeatType"] = true;
        filter["player"]["queue"]["shuffleEnabled"] = true;
        filter["player"]["queue"]["isShuffleEnabled"] = true;
        // Flat / legacy fallbacks
        filter["title"] = true;
        filter["videoId"] = true;
        filter["author"] = true;
        filter["artist"] = true;
        filter["isPaused"] = true;
        filter["isPlaying"] = true;
        filter["elapsedSeconds"] = true;
        filter["songDuration"] = true;
        filter["repeatMode"] = true;
        filter["shuffleMode"] = true;
        filter["track"]["videoId"] = true;

        DynamicJsonDocument doc(4096);
        DeserializationError err = deserializeJson(
            doc,
            resp,
            DeserializationOption::Filter(filter),
            DeserializationOption::NestingLimit(48)
        );
        if (err) {
            YTMD_FB_LOG("[YTMD/FB] JSON parse error: %s\n", err.c_str());
            continue;
        }

        // pear-desktop uses { track:{...}, player:{...} }
        // Fall back to flat layout for legacy / other API versions.
        JsonVariantConst root       = doc.as<JsonVariantConst>();
        JsonVariantConst trackObj   = root["track"];   // may be null
        JsonVariantConst playerObj  = root["player"];  // may be null

        // Periodic field-availability log for payload-shape diagnostics.
        if (now - metaLogGate > 10000) {
            YTMD_FB_LOG("[YTMD/FB] track.isNull=%d player.isNull=%d\n",
                        (int)trackObj.isNull(), (int)playerObj.isNull());
            if (!trackObj.isNull()) {
                YTMD_FB_LOG("[YTMD/FB] track.title=%s track.author=%s track.duration=%.0f\n",
                            trackObj["title"] | "?", trackObj["author"] | "?",
                            trackObj["duration"] | 0.0f);
            }
            if (!playerObj.isNull()) {
                YTMD_FB_LOG("[YTMD/FB] player.isPaused=%d player.seek=%.1f player.repeat=%s\n",
                            (int)(playerObj["isPaused"] | -1),
                            playerObj["seekbarCurrentPosition"] | 0.0f,
                            playerObj["repeatType"] | "?");
            }
            metaLogGate = now;
        }

        // --- Update track metadata on UI ---
        // Title: prefer track.title, then flat title
        const char* title = (!trackObj.isNull() ? trackObj["title"].as<const char*>() : nullptr);
        if (!title) title = root["title"] | (const char*)nullptr;
        if (title && strlen(title) > 0) {
            String t(title);
            if (t != ui_title) {
                lv_label_set_text(lbl_title, t.c_str());
                ui_title = t;
            }
        } else if (ui_title.length() == 0) {
            lv_label_set_text(lbl_title, "Not Playing");
        }

        // Artist: check both "author" and "artist" (pear-desktop uses "author" in track object)
        const char* author = nullptr;
        if (!trackObj.isNull()) {
            author = trackObj["author"].as<const char*>();
            if (!author || author[0] == '\0') author = trackObj["artist"].as<const char*>();
        }
        if (!author || author[0] == '\0') author = root["author"] | root["artist"] | (const char*)nullptr;
        if (author && author[0] != '\0') {
            String a(author);
            if (a != ui_artist) {
                lv_label_set_text(lbl_artist, a.c_str());
                ui_artist = a;
            }
        }

        // Play/Pause: support both isPaused and isPlaying.
        bool isPlaying = s_ytmd_is_playing;
        // Prefer flat root keys first (known-good API shape), nested as fallback.
        if (!root["isPlaying"].isNull()) {
            isPlaying = parseJsonBoolFlexible(root["isPlaying"], isPlaying);
        } else if (!root["isPaused"].isNull()) {
            bool isPaused = parseJsonBoolFlexible(root["isPaused"], !isPlaying);
            isPlaying = !isPaused;
        } else if (!playerObj.isNull()) {
            if (!playerObj["isPlaying"].isNull()) {
                isPlaying = parseJsonBoolFlexible(playerObj["isPlaying"], isPlaying);
            } else if (!playerObj["isPaused"].isNull()) {
                bool isPaused = parseJsonBoolFlexible(playerObj["isPaused"], !isPlaying);
                isPlaying = !isPaused;
            }
        }

        // Track key for progress anti-rollback: prefer stable videoId, fallback title|artist.
        String trackKey = "";
        const char* videoId = root["videoId"] | (const char*)nullptr;
        if ((!videoId || !videoId[0]) && !trackObj.isNull()) {
            videoId = trackObj["videoId"] | (const char*)nullptr;
        }
        if (videoId && videoId[0]) {
            trackKey = String("id:") + String(videoId);
        } else {
            String t = title ? String(title) : String("");
            String a = author ? String(author) : String("");
            if (t.length() > 0 || a.length() > 0) trackKey = t + "|" + a;
        }
        bool progressTrackChanged = (trackKey.length() > 0 && trackKey != lastProgressTrackKey);
        bool nextTrackChanged = (trackKey.length() > 0 && trackKey != lastNextTrackKey);

        // Duration parsing (numeric, string time, sec/ms variants).
        // Prefer flat root keys first to match known-good YTMD controller behavior.
        float songDurationSec = 0.0f;
        float tmpSec = 0.0f;
        if (parseJsonSeconds(root["songDuration"], &tmpSec)) songDurationSec = tmpSec;
        if (songDurationSec <= 0.0f && parseJsonSeconds(root["durationSeconds"], &tmpSec)) songDurationSec = tmpSec;
        if (songDurationSec <= 0.0f && !trackObj.isNull() && parseJsonSeconds(trackObj["duration"], &tmpSec)) songDurationSec = tmpSec;
        if (songDurationSec <= 0.0f && !trackObj.isNull() && parseJsonSeconds(trackObj["durationSeconds"], &tmpSec)) songDurationSec = tmpSec;
        if (songDurationSec <= 0.0f && !playerObj.isNull() && parseJsonSeconds(playerObj["duration"], &tmpSec)) songDurationSec = tmpSec;

        // Elapsed parsing.
        float rawElapsed = 0.0f;
        bool hasElapsed = false;
        float elapsedHumanSec = 0.0f;
        bool hasElapsedHuman = false;
        if (parseJsonSeconds(root["elapsedSeconds"], &rawElapsed)) {
            hasElapsed = true;
        }
        if (!hasElapsed && !playerObj.isNull()) {
            hasElapsedHuman = parseJsonSeconds(playerObj["seekbarCurrentPositionHuman"], &elapsedHumanSec);
            hasElapsed = parseJsonSeconds(playerObj["seekbarCurrentPosition"], &rawElapsed);
            if (!hasElapsed) hasElapsed = parseJsonSeconds(playerObj["elapsedSeconds"], &rawElapsed);
        }
        if (!hasElapsed && hasElapsedHuman) {
            rawElapsed = elapsedHumanSec;
            hasElapsed = true;
        }

        // Normalize duration units: prefer values consistent with human elapsed.
        if (songDurationSec > 0.0f) {
            if (songDurationSec > 10000.0f) {
                // Most APIs expose ms at this magnitude (e.g. 248000).
                songDurationSec /= 1000.0f;
            } else if (hasElapsedHuman && songDurationSec > (elapsedHumanSec * 20.0f) && songDurationSec > 1000.0f) {
                songDurationSec /= 1000.0f;
            }
        }

        // Fallback: derive elapsed from statePercent if direct elapsed fields are absent.
        if (!hasElapsed && songDurationSec > 0.0f && !playerObj.isNull()) {
            float pct = 0.0f;
            if (parseJsonSeconds(playerObj["statePercent"], &pct) && pct > 0.0f) {
                if (pct > 1.0f && pct <= 100.0f) pct /= 100.0f;  // 0..100 -> 0..1
                if (pct > 0.0f && pct <= 1.0f) {
                    rawElapsed = pct * songDurationSec;
                    hasElapsed = true;
                }
            }
        }

        float elapsedSec = hasElapsed ? rawElapsed : 0.0f;
        // Unit sanity for elapsed: if way bigger than duration, assume ms.
        if (songDurationSec > 0.0f && elapsedSec > (songDurationSec * 5.0f) && elapsedSec > 1000.0f) {
            elapsedSec /= 1000.0f;
        }
        if (elapsedSec < 0.0f) elapsedSec = 0.0f;

        int durSec  = (int)songDurationSec;
        if (durSec > 0) {
            float stableElapsed = elapsedSec;
            // Some pear-desktop builds occasionally report elapsedSeconds=0 while playing.
            // Avoid rewinding progress on same-track polls in that stale-zero case.
            if (!progressTrackChanged && s_ytmd_pos_ts > 0) {
                float projectedLocal = s_ytmd_elapsed_sec;
                if (isPlaying) projectedLocal += (millis() - s_ytmd_pos_ts) / 1000.0f;
                if (projectedLocal > durSec) projectedLocal = (float)durSec;

                bool staleZero = hasElapsed && isPlaying && stableElapsed <= 0.1f && projectedLocal > 2.0f;
                bool missingElapsed = !hasElapsed && isPlaying && projectedLocal > 0.0f;
                if (staleZero || missingElapsed) {
                    if (now - staleElapsedLogGate > 5000) {
                        YTMD_FB_LOG("[YTMD/FB] keep local elapsed: server=%.2f local=%.2f hasElapsed=%d\n",
                                    stableElapsed, projectedLocal, (int)hasElapsed);
                        staleElapsedLogGate = now;
                    }
                    stableElapsed = projectedLocal;
                }
            }

            ytmd_duration_seconds = durSec;
            s_ytmd_dur_sec     = durSec;
            s_ytmd_elapsed_sec = stableElapsed;
            s_ytmd_pos_ts      = millis();
            if (trackKey.length() > 0) lastProgressTrackKey = trackKey;
            if (now - parseLogGate > 3000) {
                YTMD_FB_LOG("[YTMD/FB] parsed: dur=%.2f elapsed=%.2f stable=%.2f hasElapsed=%d\n",
                            songDurationSec, elapsedSec, stableElapsed, (int)hasElapsed);
                parseLogGate = now;
            }
            // Note: time labels and slider are updated by updateYtmdProgressInterp()
            // which is called every second from updateUI(). No direct label update here.
        }

        if (isPlaying != ui_playing) {
            applyYtmdPlayState(isPlaying);
        } else {
            s_ytmd_is_playing = isPlaying;
        }

        // Shuffle: player.queue.shuffleEnabled / isShuffleEnabled (pear-desktop), with flat fallback.
        bool shuffle = ui_shuffle;
        bool hasShuffle = false;
        if (!playerObj.isNull()) {
            JsonVariantConst q = playerObj["queue"];
            if (!q.isNull()) {
                if (!q["shuffleEnabled"].isNull()) {
                    shuffle = parseJsonBoolFlexible(q["shuffleEnabled"], shuffle);
                    hasShuffle = true;
                } else if (!q["isShuffleEnabled"].isNull()) {
                    shuffle = parseJsonBoolFlexible(q["isShuffleEnabled"], shuffle);
                    hasShuffle = true;
                }
            }
        }
        if (!hasShuffle) {
            shuffle = root["shuffleMode"] | false;
        }
        if (shuffle != ui_shuffle) applyYtmdShuffleState(shuffle);

        // Repeat: player.repeatType (pear-desktop: "NONE","ONE","ALL"), with flat fallback.
        const char* repeatMode = nullptr;
        if (!playerObj.isNull()) repeatMode = playerObj["repeatType"] | (const char*)nullptr;
        if (!repeatMode) repeatMode = root["repeatMode"] | (const char*)nullptr;
        if (repeatMode) {
            String rm(repeatMode);
            if (rm != ui_repeat) applyYtmdRepeatMode(rm);
        }

        // Next track line / YTMD playlist cache:
        // refresh on track change, explicit queue-screen request, or sparse periodic sync.
        bool queueRefreshRequested = ytmd_queue_fetch_requested;
        if (queueRefreshRequested || nextTrackChanged || (now - lastNextPollMs >= kNextPollMs)) {
            bool queueOk = pollAndApplyYtmdNextTrack(auth);
            if (queueOk) {
                lastNextPollMs = now;
                if (trackKey.length() > 0) lastNextTrackKey = trackKey;
                ytmd_queue_fetch_requested = false;
            }
        }

        // Album art
        String art = extractYtmdArtUrl(root);
        if (art.length() > 0) {
            gotArt = true;
            consecutiveFailures = 0;  // Reset backoff on success
            if (art != lastRequestedYtmdArt) {
                YTMD_FB_LOG("[YTMD/FB] Requesting art: %s\n", art.c_str());
                requestAlbumArt(art);
                lastRequestedYtmdArt = art;
            }
            break;
        }

        // Even if no art, we got a valid response — reset failure count
        consecutiveFailures = 0;
        gotArt = true;  // Prevent "no art" spam log when track metadata was parsed
        break;
    }

    if (!gotArt && (now - noDeviceLogGate > 10000)) {
        YTMD_FB_LOG("[YTMD/FB] No art URL available from pear endpoints yet (failures=%d)\n",
                    consecutiveFailures);
        noDeviceLogGate = now;
    }

    xSemaphoreGive(network_mutex);
}

// Interpolates YTMD playback position every second between 3-second polls.
// Reads from s_ytmd_* statics written by maybePollYtmdArtFallback().
static void updateYtmdProgressInterp() {
    if (s_ytmd_dur_sec <= 0 || s_ytmd_pos_ts == 0) return;
    static unsigned long lastUpdateMs = 0;
    unsigned long now = millis();
    if (now - lastUpdateMs < 1000) return;
    lastUpdateMs = now;

    float elapsed = s_ytmd_elapsed_sec;
    if (s_ytmd_is_playing) elapsed += (now - s_ytmd_pos_ts) / 1000.0f;
    if (elapsed > s_ytmd_dur_sec) elapsed = (float)s_ytmd_dur_sec;

    int elapSec = (int)elapsed;
    int durSec  = s_ytmd_dur_sec;

    char tbuf[16];
    formatTimeLabel(elapSec, tbuf, sizeof(tbuf), false);
    lv_label_set_text(lbl_time, tbuf);

    char rbuf[16];
    formatTimeLabel(durSec, rbuf, sizeof(rbuf), false);
    lv_label_set_text(lbl_time_remaining, rbuf);

    if (!dragging_prog) {
        lv_slider_set_value(slider_progress, (elapSec * 100) / durSec, LV_ANIM_OFF);
    }
}

// Selects the correct art URL and calls requestAlbumArt() when needed.
// Also handles URI change detection and "not playing" transitions.
static void updateAlbumArtRequest(SonosDevice* d) {
    static String last_track_uri = "";
    static String last_source_prefix = "";
    static bool had_track = false;

    // Extract source prefix to detect actual source changes (not just track changes)
    String current_source_prefix = "";
    if (d->currentURI.startsWith("x-sonos-vli:")) {
        current_source_prefix = "x-sonos-vli";  // Spotify, Apple Music, etc
    } else if (d->currentURI.startsWith("hls-radio://")) {
        current_source_prefix = "hls-radio";  // Radio
    } else if (d->currentURI.startsWith("x-sonos-http:")) {
        current_source_prefix = "x-sonos-http";  // Radio
    } else if (d->currentURI.startsWith("x-rincon-mp3radio:")) {
        current_source_prefix = "x-rincon-mp3radio";  // Radio
    } else {
        // Extract first part before colon for unknown sources
        int colonPos = d->currentURI.indexOf(':');
        if (colonPos > 0) {
            current_source_prefix = d->currentURI.substring(0, colonPos);
        }
    }

    // Detect ACTUAL source changes (Spotify→Radio, not Spotify track1→track2)
    bool actual_source_change = (current_source_prefix != last_source_prefix && current_source_prefix.length() > 0);

    // Detect any URI change (track or source)
    bool uri_changed = (d->currentURI != last_track_uri);

    if (uri_changed) {
        // Always update last_track_uri (even when empty) to prevent repeated firing
        // when Sonos reports empty URI in stopped state
        last_track_uri = d->currentURI;

        if (d->currentURI.length() > 0) {
            if (actual_source_change) {
                YTMD_FB_LOG("[ART] SOURCE CHANGE: %s -> %s\n", last_source_prefix.c_str(), current_source_prefix.c_str());
                last_source_prefix = current_source_prefix;
            } else {
                YTMD_FB_LOG("[ART] Track changed (same source: %s)\n", current_source_prefix.c_str());
            }
            // CRITICAL: Abort any in-progress album art download immediately
            // Applies to ALL track changes (not just source changes) so the art task
            // doesn't wait for a 10-second HTTP timeout before processing the new track
            art_abort_download = true;
            // CRITICAL: Must hold art_mutex when writing last_art_url/pending_art_url -
            // the art task reads both under mutex, and String assignment is not atomic.
            if (xSemaphoreTake(art_mutex, pdMS_TO_TICKS(50))) {
                last_art_url    = "";  // Force art refresh on any URI change
                pending_art_url = "";  // Prevent art task downloading old song's art during
                                       // the window between URI detection and requestAlbumArt()
                                       // call ~100 lines below. Without this, art task wakes,
                                       // sees pending=old_url != last_art_url="" and starts
                                       // downloading wrong art, wasting SDIO traffic and setting
                                       // last_art_download_end_ms (adding 1s cooldown for new art).
                art_ready = false;  // Discard any just-completed download — prevents old art
                                    // flashing for the new track if displayCompletedArt() fires
                                    // before the new art task iteration starts.
                xSemaphoreGive(art_mutex);
            }
            // Clear LRU cache: prevents cached art from the OLD track flashing for
            // the NEW track if the art task picks up the new URL within the same
            // updateUI frame (cache hit would bypass the placeholder entirely).
            clearAlbumArtCache();
            // Show placeholder immediately (main thread = LVGL-safe)
            if (img_album)       lv_obj_add_flag(img_album, LV_OBJ_FLAG_HIDDEN);
            if (art_placeholder) lv_obj_remove_flag(art_placeholder, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Show placeholder when device transitions to "Not Playing" (no active track)
    // Without this, the last track's art stays frozen when playback stops
    bool has_track = (d->currentTrack.length() > 0);
    if (had_track && !has_track) {
        Serial.println("[ART] Not playing - clearing art display");
        art_abort_download = true;  // Stop any in-progress download immediately
        clearAlbumArtCache();
        if (img_album) lv_obj_add_flag(img_album, LV_OBJ_FLAG_HIDDEN);
        if (art_placeholder) lv_obj_remove_flag(art_placeholder, LV_OBJ_FLAG_HIDDEN);
        if (xSemaphoreTake(art_mutex, pdMS_TO_TICKS(50))) {
            last_art_url = "";
            pending_art_url = "";  // Prevent art task re-fetching the old URL
            art_ready = false;     // Discard any just-completed download (prevents art flash)
            xSemaphoreGive(art_mutex);
        }
    }
    had_track = has_track;

    // Request album art if URL provided and (URL changed or track changed)
    // Compare against last_requested_art_url (HTTPS, same type as d->albumArtURL) NOT pending_art_url.
    // The art task converts pending_art_url to HTTP internally — comparing HTTPS vs HTTP always
    // returns "changed", calling requestAlbumArt() every frame and keeping art_download_in_progress=true
    // permanently (blocking the clock screensaver and spamming last_track_change_ms).
    static String last_requested_art_url = "";
    bool hasArt = !d->isLineIn && !d->isTvAudio &&
                  ((d->albumArtURL.length() > 0) || (d->isRadioStation && d->radioStationArtURL.length() > 0));
    bool artChanged = uri_changed || (d->albumArtURL.length() > 0 && d->albumArtURL != last_requested_art_url);

    // For radio stations: also check if radioStationArtURL changed (even if albumArtURL is empty)
    if (d->isRadioStation && d->radioStationArtURL.length() > 0 && d->radioStationArtURL != last_requested_art_url) {
        artChanged = true;
    }

    if (!hasArt && uri_changed) {
        // Track changed but has NO art URL — clear old art and show placeholder immediately
        // (Without this, the old track's art stays on screen forever)
        Serial.println("[ART] No art URL for this track - showing placeholder");
        if (img_album) lv_obj_add_flag(img_album, LV_OBJ_FLAG_HIDDEN);
        if (art_placeholder) lv_obj_remove_flag(art_placeholder, LV_OBJ_FLAG_HIDDEN);
        if (xSemaphoreTake(art_mutex, pdMS_TO_TICKS(50))) {
            last_art_url = "";
            pending_art_url = "";  // Prevent art task re-fetching the old URL
            art_ready = false;     // Discard any just-completed download (prevents art flash)
            xSemaphoreGive(art_mutex);
        }
    } else if (hasArt && artChanged) {
        String artURL = "";
        bool usingStationLogo = false;  // Track if we're using station logo (PNG allowed)

        // Determine which art to use
        if (d->albumArtURL.length() > 0) {
            artURL = d->albumArtURL;
        }

        // RADIO STATION LOGO FALLBACK:
        // If playing radio and no song art available, use station logo instead
        if (d->isRadioStation) {
            bool hasSongArt = (artURL.length() > 0);
            bool hasStationLogo = (d->radioStationArtURL.length() > 0);
            YTMD_FB_LOG("[ART] Radio check - hasSongArt=%d, hasStationLogo=%d, artURL='%s', stationURL='%s'\n",
                        hasSongArt, hasStationLogo, artURL.c_str(), d->radioStationArtURL.c_str());

            // If no song art but have station logo, use the logo
            if (!hasSongArt && hasStationLogo) {
                artURL = d->radioStationArtURL;
                usingStationLogo = true;
                Serial.println("[ART] Radio: Using station logo (no song art)");
            }
            // If song art is just a generic Sonos radio icon, prefer the actual station logo
            else if (hasSongArt && hasStationLogo && artURL.indexOf("/getaa?") > 0) {
                // Check if it's pointing to the radio URI (generic icon)
                if (artURL.indexOf("x-sonosapi-stream") > 0 ||
                    artURL.indexOf("x-rincon-mp3radio") > 0 ||
                    artURL.indexOf("x-sonosapi-radio") > 0 ||
                    artURL.indexOf("x-sonosapi-hls") > 0) {
                    artURL = d->radioStationArtURL;
                    usingStationLogo = true;
                    Serial.println("[ART] Radio: Using station logo (replacing generic icon)");
                }
            }
        }

        // Set the flag for album art task to know if PNG is allowed
        pending_is_station_logo = usingStationLogo;

        if (artURL.length() > 0) {
            // Note: Using ESP32-P4 hardware JPEG decoder - can handle full 640x640 Spotify images!

            // Apple Music: reduce image size to avoid "too large" errors (1400x1400 can be 500KB+)
            if (artURL.indexOf("mzstatic.com") > 0) {
                if (artURL.indexOf("/1400x1400bb.jpg") > 0) {
                    artURL.replace("/1400x1400bb.jpg", "/400x400bb.jpg");
                    Serial.println("[ART] Apple Music - reduced to 400x400");
                } else if (artURL.indexOf("/1080x1080cc.jpg") > 0) {
                    artURL.replace("/1080x1080cc.jpg", "/400x400cc.jpg");
                    Serial.println("[ART] Apple Music - reduced to 400x400");
                }
            }

            requestAlbumArt(artURL);
            last_requested_art_url = artURL;  // track HTTPS URL; prevents HTTPS!=HTTP false-positive on next frame
        } else {
            // No art available - clear display
            Serial.println("[ART] No art URL - clearing display");
            if (img_album) lv_obj_add_flag(img_album, LV_OBJ_FLAG_HIDDEN);
            if (art_placeholder) lv_obj_remove_flag(art_placeholder, LV_OBJ_FLAG_HIDDEN);
            // CRITICAL: Must hold art_mutex when writing last_art_url (not atomic)
            if (xSemaphoreTake(art_mutex, pdMS_TO_TICKS(50))) {
                last_art_url = "";  // Clear to allow next art request
                xSemaphoreGive(art_mutex);
            }
        }
    }
}

// ============================================================================
// UI Update Function
// ============================================================================
void updateUI() {
    static bool ytmd_header_set = false;
    SonosDevice* d = sonos.getCurrentDevice();
    if (!d || isYtmdVirtualDevice(d)) {
        // Ensure device name shows "YTMD" in YTMD-only mode
        if (!ytmd_header_set && lbl_device_name) {
            lv_label_set_text(lbl_device_name, "YTMD");
            ytmd_header_set = true;
        }
        maybePollYtmdArtFallback();
        updateYtmdProgressInterp();
        displayCompletedArt();
        return;
    }
    // Reset flag so if user switches to Sonos mode it updates again
    ytmd_header_set = false;

    if (!updateConnectionState(d)) return;

    // Device is connected - update UI normally

    // Title
    if (d->currentTrack != ui_title) {
        lv_label_set_text(lbl_title, d->currentTrack.length() > 0 ? d->currentTrack.c_str() : "Not Playing");
        ui_title = d->currentTrack;
    }

    // Artist
    if (d->currentArtist != ui_artist) {
        lv_label_set_text(lbl_artist, d->currentArtist.c_str());
        ui_artist = d->currentArtist;
    }

    // Fetch synced lyrics when track changes
    // lyrics_last_track is a global (ui_globals.cpp) so clock exit can reset it to ""
    // forcing a re-fetch for the current track when returning from the clock screen.
    String lyrics_key = d->currentArtist + "|" + d->currentTrack;
    if (lyrics_key != lyrics_last_track && d->currentTrack.length() > 0) {
        last_track_change_ms = millis();
        if (lyrics_enabled && !d->isRadioStation && !d->isLineIn && !d->isTvAudio) {
            // Abort any running task FIRST. requestLyrics() checks artist.length()==0
            // at line 1 and returns false without ever touching lyrics_abort_requested —
            // so for podcasts/audiobooks the old task keeps running, finishes, writes
            // stale lyrics into the buffer, and main thread displays them even after
            // clearLyrics(). Setting the flag here stops the old task before clear.
            lyrics_abort_requested = true;
            clearLyrics();
            // requestLyrics() returns false if the previous task is still running
            // (e.g. blocked inside http.GET() with up to 10s timeout). In that case
            // we do NOT update lyrics_last_track — the condition fires again next
            // frame until the old task exits and requestLyrics() can safely spawn.
            // This prevents TCB/stack reuse while the old task is alive, which would
            // permanently leak ~32KB of TLS DMA buffers per rapid track change.
            if (requestLyrics(d->currentArtist, d->currentTrack, d->durationSeconds)) {
                lyrics_last_track = lyrics_key;
            } else if (!lyrics_fetching) {
                // Task not running and spawn failed (e.g. empty artist for podcast/audiobook,
                // buffer not initialised). Accept "no lyrics" to avoid a per-frame retry loop.
                lyrics_last_track = lyrics_key;
            }
        } else {
            lyrics_last_track = lyrics_key;
            clearLyrics();
        }
    }

    // Album name (below album art)
    static String ui_album_name = "";
    if (d->currentAlbum != ui_album_name) {
        lv_label_set_text(lbl_album, d->currentAlbum.c_str());
        ui_album_name = d->currentAlbum;
    }

    // Device name in header
    static String ui_device_name = "";
    if (d->roomName != ui_device_name) {
        String np = "YouTube Music Desktop";
        lv_label_set_text(lbl_device_name, np.c_str());
        ui_device_name = d->roomName;
    }

    // Time display
    String t = d->relTime;
    if (t.startsWith("0:")) t = t.substring(2);
    lv_label_set_text(lbl_time, t.c_str());

    // Total duration (fixed): M:SS / H:MM:SS
    if (d->durationSeconds > 0) {
        char buf[16];
        formatTimeLabel(d->durationSeconds, buf, sizeof(buf), false);
        lv_label_set_text(lbl_time_remaining, buf);
    }

    // Progress slider
    if (!dragging_prog && d->durationSeconds > 0)
        lv_slider_set_value(slider_progress, (d->relTimeSeconds * 100) / d->durationSeconds, LV_ANIM_OFF);

    // Update synced lyrics display and status indicator
    updateLyricsDisplay(d->relTimeSeconds);
    updateLyricsStatus();  // Update status indicator from main thread

    // Play/Pause button
    if (d->isPlaying != ui_playing) {
        lv_obj_t* lbl = lv_obj_get_child(btn_play, 0);
        lv_label_set_text(lbl, d->isPlaying ? MDI_PAUSE : MDI_PLAY);
        lv_obj_set_style_text_font(lbl, &lv_font_mdi_40, 0);
        lv_obj_center(lbl);  // MDI icons are optically centered — no offset needed

        ui_playing = d->isPlaying;
    }

    // Volume slider update
    if (!dragging_vol && d->volume != ui_vol && slider_vol) {
        lv_slider_set_value(slider_vol, d->volume, LV_ANIM_OFF);
        ui_vol = d->volume;
    }

    // Mute button
    if (d->isMuted != ui_muted && btn_mute) {
        lv_obj_t* lbl = lv_obj_get_child(btn_mute, 0);
        lv_label_set_text(lbl, d->isMuted ? MDI_VOLUME_OFF : MDI_VOLUME_HIGH);
        ui_muted = d->isMuted;
    }

    // Shuffle
    if (d->shuffleMode != ui_shuffle) {
        lv_obj_t* lbl = lv_obj_get_child(btn_shuffle, 0);
        lv_obj_set_style_text_color(lbl, d->shuffleMode ? COL_ACCENT : COL_TEXT2, 0);
        ui_shuffle = d->shuffleMode;
    }

    // Next track info - find next track in queue
    // SKIP FOR RADIO MODE - radio stations don't have a queue/next track
    updateNextTrackUI(d);

    // Repeat
    if (d->repeatMode != ui_repeat) {
        lv_obj_t* lbl = lv_obj_get_child(btn_repeat, 0);
        if (d->repeatMode == "ONE") {
            lv_label_set_text(lbl, MDI_REPEAT_ONCE);
            lv_obj_set_style_text_font(lbl, &lv_font_mdi_32, 0);
            lv_obj_set_style_text_color(lbl, COL_ACCENT, 0);
        } else if (d->repeatMode == "ALL") {
            lv_label_set_text(lbl, MDI_REPEAT);
            lv_obj_set_style_text_font(lbl, &lv_font_mdi_32, 0);
            lv_obj_set_style_text_color(lbl, COL_ACCENT, 0);
        } else {
            lv_label_set_text(lbl, MDI_REPEAT);
            lv_obj_set_style_text_font(lbl, &lv_font_mdi_32, 0);
            lv_obj_set_style_text_color(lbl, COL_TEXT2, 0);
        }
        ui_repeat = d->repeatMode;
    }

    // Album art - only request if URL changed to prevent download loops
    // NOTE: last_art_url is GLOBAL (extern in ui_common.h), don't shadow it!
    updateAlbumArtRequest(d);
    displayCompletedArt();

    // Line-in and TV audio modes take priority over radio mode — checked first.
    // setLineInMode/setTvAudioMode(false) restores UI when transitioning back to music/radio.
    if (d->isLineIn) {
        setTvAudioMode(false);
        updateLineInUI();
        return;
    }
    if (d->isTvAudio) {
        setLineInMode(false);
        updateTvAudioUI();
        return;
    }
    setLineInMode(false);
    setTvAudioMode(false);

    // Radio mode UI adaptation - must be at the END of updateUI()
    updateRadioModeUI();
}

void processUpdates() {
    static uint32_t lastUpdate = 0;
    UIUpdate_t upd;
    bool need = false;
    bool queue_updated = false;
    while (xQueueReceive(sonos.getUIUpdateQueue(), &upd, 0)) {
        need = true;
        if (upd.type == UPDATE_QUEUE) queue_updated = true;
    }
    if (need && (millis() - lastUpdate > 200)) { updateUI(); lastUpdate = millis(); }
    else {
        SonosDevice* d = sonos.getCurrentDevice();
        if (!d || isYtmdVirtualDevice(d)) { maybePollYtmdArtFallback(); updateYtmdProgressInterp(); }
        displayCompletedArt();  // Run even without Sonos events (e.g. art ready while polling suppressed)
    }
    // Auto-refresh queue list if the queue screen is visible when new data arrives
    if (queue_updated && lv_screen_active() == scr_queue) refreshQueueList();
}
