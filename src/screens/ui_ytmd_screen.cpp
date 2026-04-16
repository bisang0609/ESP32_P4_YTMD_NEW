/**
 * YTMD Settings Screen — pear-desktop companion server
 *
 * Auth flow (pear-desktop API server):
 *   1. POST http://{ip}:{port}/auth/{appId}
 *      → Desktop app shows Allow/Deny dialog
 *      → On allow: HTTP 200  {"accessToken": "<jwt>"}
 *      → On deny:  HTTP 403
 *   2. Store accessToken to NVS; use as:
 *        Authorization: Bearer <token>
 *      in every subsequent API call.
 *
 * Default port: 26538 (YTMD_DEFAULT_PORT)
 */

#include "ui_common.h"
#include "config.h"

// Forward declaration (defined in ui_settings_screens.cpp)
lv_obj_t* createSettingsSidebar(lv_obj_t* screen, int activeIdx);

static String sanitizeYtmdIP(lv_obj_t* ta_ip)
{
    String ip = String(lv_textarea_get_text(ta_ip));
    if (ip.indexOf(';') >= 0) {
        ip.replace(';', '.');
        lv_textarea_set_text(ta_ip, ip.c_str());
    }
    return ip;
}

// ============================================================================
// createYTMDScreen
// ============================================================================
void createYTMDScreen() {
    scr_ytmd = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_ytmd, lv_color_hex(0x121212), 0);

    // Sidebar — YTMD is index 4
    lv_obj_t* content = createSettingsSidebar(scr_ytmd, 4);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content, 14, 0);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    // ── Title ─────────────────────────────────────────────────────────────────
    lv_obj_t* lbl_title = lv_label_create(content);
    lv_label_set_text(lbl_title, "YouTube Music Desktop");
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_title, COL_TEXT, 0);
    lv_obj_set_style_pad_bottom(lbl_title, 4, 0);

    // ── Target IP ─────────────────────────────────────────────────────────────
    lv_obj_t* lbl_ip = lv_label_create(content);
    lv_label_set_text(lbl_ip, "Target IP");
    lv_obj_set_style_text_font(lbl_ip, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_ip, COL_TEXT2, 0);

    lv_obj_t* ta_ip = lv_textarea_create(content);
    lv_obj_set_size(ta_ip, lv_pct(100), 48);
    lv_textarea_set_one_line(ta_ip, true);
    lv_textarea_set_accepted_chars(ta_ip, "0123456789.");
    lv_textarea_set_placeholder_text(ta_ip, "192.168.1.xxx");
    lv_textarea_set_text(ta_ip, ytmd_ip.c_str());
    lv_obj_set_style_bg_color(ta_ip, lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_border_color(ta_ip, lv_color_hex(0x3A3A3A), 0);
    lv_obj_set_style_border_width(ta_ip, 1, 0);
    lv_obj_set_style_border_color(ta_ip, COL_ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_radius(ta_ip, 10, 0);
    lv_obj_set_style_text_color(ta_ip, COL_TEXT, 0);
    lv_obj_set_style_text_font(ta_ip, &lv_font_montserrat_16, 0);
    lv_obj_set_style_pad_left(ta_ip, 14, 0);

    // ── Port ──────────────────────────────────────────────────────────────────
    lv_obj_t* lbl_port = lv_label_create(content);
    lv_label_set_text(lbl_port, "Port");
    lv_obj_set_style_text_font(lbl_port, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_port, COL_TEXT2, 0);

    lv_obj_t* ta_port = lv_textarea_create(content);
    lv_obj_set_size(ta_port, lv_pct(100), 48);
    lv_textarea_set_one_line(ta_port, true);
    lv_textarea_set_accepted_chars(ta_port, "0123456789");
    lv_textarea_set_placeholder_text(ta_port, "26538");
    lv_textarea_set_text(ta_port, String(ytmd_port).c_str());
    lv_obj_set_style_bg_color(ta_port, lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_border_color(ta_port, lv_color_hex(0x3A3A3A), 0);
    lv_obj_set_style_border_width(ta_port, 1, 0);
    lv_obj_set_style_border_color(ta_port, COL_ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_radius(ta_port, 10, 0);
    lv_obj_set_style_text_color(ta_port, COL_TEXT, 0);
    lv_obj_set_style_text_font(ta_port, &lv_font_montserrat_16, 0);
    lv_obj_set_style_pad_left(ta_port, 14, 0);

    // ── Keyboard (shared for IP and port fields) ──────────────────────────────
    lv_obj_t* ytmd_kb = lv_keyboard_create(scr_ytmd);
    lv_obj_set_size(ytmd_kb, 800, 200);
    lv_obj_align(ytmd_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(ytmd_kb, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_mode(ytmd_kb, LV_KEYBOARD_MODE_NUMBER);  // Always open numeric keypad first
    lv_obj_set_style_bg_color(ytmd_kb, lv_color_hex(0x1A1A1A), 0);

    // Static pointers used by keyboard save handlers and auth button
    static lv_obj_t* ta_ptrs[2];
    ta_ptrs[0] = ta_ip;
    ta_ptrs[1] = ta_port;

    // Show keyboard on focus
    lv_obj_add_event_cb(ta_ip, [](lv_event_t* e) {
        lv_obj_t* kb_obj = (lv_obj_t*)lv_event_get_user_data(e);
        lv_keyboard_set_textarea(kb_obj, (lv_obj_t*)lv_event_get_target(e));
        lv_keyboard_set_mode(kb_obj, LV_KEYBOARD_MODE_NUMBER);
        lv_obj_clear_flag(kb_obj, LV_OBJ_FLAG_HIDDEN);
    }, LV_EVENT_FOCUSED, ytmd_kb);

    lv_obj_add_event_cb(ta_port, [](lv_event_t* e) {
        lv_obj_t* kb_obj = (lv_obj_t*)lv_event_get_user_data(e);
        lv_keyboard_set_textarea(kb_obj, (lv_obj_t*)lv_event_get_target(e));
        lv_keyboard_set_mode(kb_obj, LV_KEYBOARD_MODE_NUMBER);
        lv_obj_clear_flag(kb_obj, LV_OBJ_FLAG_HIDDEN);
    }, LV_EVENT_FOCUSED, ytmd_kb);

    lv_obj_add_event_cb(ta_ip, [](lv_event_t* e) {
        lv_obj_t* ta = (lv_obj_t*)lv_event_get_target(e);
        String ip = String(lv_textarea_get_text(ta));
        if (ip.indexOf(';') >= 0) {
            ip.replace(';', '.');
            lv_textarea_set_text(ta, ip.c_str());
        }
    }, LV_EVENT_VALUE_CHANGED, NULL);

    // Save on Enter or close
    lv_obj_add_event_cb(ytmd_kb, [](lv_event_t* e) {
        lv_obj_add_flag((lv_obj_t*)lv_event_get_target(e), LV_OBJ_FLAG_HIDDEN);
        ytmd_ip   = sanitizeYtmdIP(ta_ptrs[0]);
        ytmd_port = atoi(lv_textarea_get_text(ta_ptrs[1]));
        if (ytmd_port <= 0) ytmd_port = YTMD_DEFAULT_PORT;
        wifiPrefs.putString(NVS_KEY_YTMD_IP,   ytmd_ip);
        wifiPrefs.putInt   (NVS_KEY_YTMD_PORT, ytmd_port);
        Serial.printf("[YTMD] Saved IP=%s Port=%d\n", ytmd_ip.c_str(), ytmd_port);
    }, LV_EVENT_READY, NULL);

    lv_obj_add_event_cb(ytmd_kb, [](lv_event_t* e) {
        lv_obj_add_flag((lv_obj_t*)lv_event_get_target(e), LV_OBJ_FLAG_HIDDEN);
        ytmd_ip   = sanitizeYtmdIP(ta_ptrs[0]);
        ytmd_port = atoi(lv_textarea_get_text(ta_ptrs[1]));
        if (ytmd_port <= 0) ytmd_port = YTMD_DEFAULT_PORT;
        wifiPrefs.putString(NVS_KEY_YTMD_IP,   ytmd_ip);
        wifiPrefs.putInt   (NVS_KEY_YTMD_PORT, ytmd_port);
        Serial.printf("[YTMD] Saved IP=%s Port=%d\n", ytmd_ip.c_str(), ytmd_port);
    }, LV_EVENT_CANCEL, NULL);

    // ── Auth section header ────────────────────────────────────────────────────
    lv_obj_t* lbl_auth_hdr = lv_label_create(content);
    lv_label_set_text(lbl_auth_hdr, "Authentication");
    lv_obj_set_style_text_font(lbl_auth_hdr, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_auth_hdr, COL_TEXT2, 0);
    lv_obj_set_style_pad_top(lbl_auth_hdr, 4, 0);

    // ── Token status row (icon + text) ────────────────────────────────────────
    lv_obj_t* row_token = lv_obj_create(content);
    lv_obj_set_size(row_token, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row_token, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_border_color(row_token, lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_border_width(row_token, 1, 0);
    lv_obj_set_style_radius(row_token, 8, 0);
    lv_obj_set_style_pad_all(row_token, 10, 0);
    lv_obj_set_flex_flow(row_token, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row_token, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(row_token, 8, 0);
    lv_obj_clear_flag(row_token, LV_OBJ_FLAG_SCROLLABLE);

    // Dot indicator (green = token saved, grey = no token)
    lv_obj_t* dot = lv_label_create(row_token);
    lv_label_set_text(dot, LV_SYMBOL_BULLET);
    lv_obj_set_style_text_font(dot, &lv_font_montserrat_14, 0);
    bool has_token = ytmd_token.length() > 0;
    lv_obj_set_style_text_color(dot,
        has_token ? lv_color_hex(0x44CC66) : lv_color_hex(0x555555), 0);

    lbl_ytmd_status = lv_label_create(row_token);
    lv_obj_set_style_text_font(lbl_ytmd_status, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_ytmd_status, COL_TEXT2, 0);
    lv_obj_set_flex_grow(lbl_ytmd_status, 1);
    lv_label_set_long_mode(lbl_ytmd_status, LV_LABEL_LONG_WRAP);
    if (has_token) {
        lv_label_set_text(lbl_ytmd_status, "Token saved — authenticated");
        lv_obj_set_style_text_color(lbl_ytmd_status, lv_color_hex(0x44CC66), 0);
    } else {
        lv_label_set_text(lbl_ytmd_status, "No token — press Request Auth");
        lv_obj_set_style_text_color(lbl_ytmd_status, COL_TEXT2, 0);
    }

    // ── Request Auth button ────────────────────────────────────────────────────
    // POST http://{ip}:{port}/auth/{appId}
    // Desktop shows Allow/Deny dialog; on Allow → 200 {"accessToken": "<jwt>"}
    lv_obj_t* btn_auth = lv_button_create(content);
    lv_obj_set_size(btn_auth, lv_pct(100), 52);
    lv_obj_set_style_bg_color(btn_auth, COL_ACCENT, 0);
    lv_obj_set_style_bg_color(btn_auth, lv_color_hex(0xB8902A), LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn_auth, 10, 0);
    lv_obj_set_style_shadow_width(btn_auth, 0, 0);

    lv_obj_t* lbl_btn = lv_label_create(btn_auth);
    lv_label_set_text(lbl_btn, "Request Auth");
    lv_obj_set_style_text_font(lbl_btn, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_btn, lv_color_hex(0x000000), 0);
    lv_obj_center(lbl_btn);

    // ── Clear Token button ─────────────────────────────────────────────────────
    lv_obj_t* btn_clear = lv_button_create(content);
    lv_obj_set_size(btn_clear, lv_pct(100), 44);
    lv_obj_set_style_bg_color(btn_clear, lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_bg_color(btn_clear, lv_color_hex(0x3A3A3A), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn_clear, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_width(btn_clear, 1, 0);
    lv_obj_set_style_radius(btn_clear, 10, 0);
    lv_obj_set_style_shadow_width(btn_clear, 0, 0);

    lv_obj_t* lbl_clear = lv_label_create(btn_clear);
    lv_label_set_text(lbl_clear, "Clear Saved Token");
    lv_obj_set_style_text_font(lbl_clear, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_clear, COL_TEXT2, 0);
    lv_obj_center(lbl_clear);

    // ── Test Connection button ─────────────────────────────────────────────────
    // GET /api/v1/song with Bearer token → verifies server is reachable + token valid
    lv_obj_t* btn_test = lv_button_create(content);
    lv_obj_set_size(btn_test, lv_pct(100), 44);
    lv_obj_set_style_bg_color(btn_test, lv_color_hex(0x1E3A2A), 0);
    lv_obj_set_style_bg_color(btn_test, lv_color_hex(0x2A4A36), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn_test, lv_color_hex(0x2A6A44), 0);
    lv_obj_set_style_border_width(btn_test, 1, 0);
    lv_obj_set_style_radius(btn_test, 10, 0);
    lv_obj_set_style_shadow_width(btn_test, 0, 0);

    lv_obj_t* lbl_test = lv_label_create(btn_test);
    lv_label_set_text(lbl_test, "Test Connection");
    lv_obj_set_style_text_font(lbl_test, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_test, lv_color_hex(0x44CC66), 0);
    lv_obj_center(lbl_test);

    // ── Auth button event ─────────────────────────────────────────────────────
    // Capture dot + status label for the callback via static pointers
    static lv_obj_t* s_dot        = nullptr;
    static lv_obj_t* s_status_lbl = nullptr;
    s_dot        = dot;
    s_status_lbl = lbl_ytmd_status;

    lv_obj_add_event_cb(btn_auth, [](lv_event_t* e) {
        // Persist current field values first
        ytmd_ip   = sanitizeYtmdIP(ta_ptrs[0]);
        ytmd_port = atoi(lv_textarea_get_text(ta_ptrs[1]));
        if (ytmd_port <= 0) ytmd_port = YTMD_DEFAULT_PORT;

        if (ytmd_ip.length() == 0) {
            lv_obj_set_style_text_color(s_dot, lv_color_hex(0xFF5555), 0);
            lv_label_set_text(s_status_lbl, "Error: IP address is empty");
            lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(0xFF5555), 0);
            return;
        }

        // Save IP/port immediately so it persists even if auth fails
        wifiPrefs.putString(NVS_KEY_YTMD_IP,   ytmd_ip);
        wifiPrefs.putInt   (NVS_KEY_YTMD_PORT, ytmd_port);

        lv_obj_set_style_text_color(s_dot, lv_color_hex(0x888888), 0);
        lv_label_set_text(s_status_lbl, "Requesting auth — approve in desktop app...");
        lv_obj_set_style_text_color(s_status_lbl, COL_TEXT2, 0);
        lv_refr_now(NULL);

        // Build URL: POST /auth/{appId}
        char url[160];
        snprintf(url, sizeof(url), "http://%s:%d/auth/%s",
                 ytmd_ip.c_str(), ytmd_port, YTMD_APP_ID);

        HTTPClient http;
        http.begin(url);
        http.addHeader("Content-Type", "application/json");
        http.setTimeout(10000);  // 10 s — user needs time to click Allow

        // Empty body — pear-desktop auth only requires the appId in the URL
        int code = http.POST((uint8_t*)"", 0);
        Serial.printf("[YTMD] POST %s → HTTP %d\n", url, code);

        if (code == 200) {
            String resp = http.getString();
            Serial.printf("[YTMD] Auth response: %s\n", resp.c_str());

            // Parse {"accessToken": "<jwt>"}
            StaticJsonDocument<512> doc;
            if (deserializeJson(doc, resp) == DeserializationError::Ok) {
                const char* token = doc["accessToken"];
                if (token && strlen(token) > 0) {
                    ytmd_token = String(token);
                    wifiPrefs.putString(NVS_KEY_YTMD_TOKEN, ytmd_token);
                    Serial.printf("[YTMD] Token saved (%d chars)\n", ytmd_token.length());

                    lv_obj_set_style_text_color(s_dot, lv_color_hex(0x44CC66), 0);
                    lv_label_set_text(s_status_lbl, "Token saved — authenticated");
                    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(0x44CC66), 0);
                } else {
                    lv_obj_set_style_text_color(s_dot, lv_color_hex(0xFF5555), 0);
                    lv_label_set_text(s_status_lbl, "Error: no accessToken in response");
                    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(0xFF5555), 0);
                }
            } else {
                lv_obj_set_style_text_color(s_dot, lv_color_hex(0xFF5555), 0);
                lv_label_set_text(s_status_lbl, "Error: invalid JSON response");
                lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(0xFF5555), 0);
            }
        } else if (code == 403) {
            lv_obj_set_style_text_color(s_dot, lv_color_hex(0xFF5555), 0);
            lv_label_set_text(s_status_lbl, "Denied — press Allow in the desktop app");
            lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(0xFF9944), 0);
        } else if (code < 0) {
            lv_obj_set_style_text_color(s_dot, lv_color_hex(0xFF5555), 0);
            lv_label_set_text(s_status_lbl, "Error: connection failed");
            lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(0xFF5555), 0);
        } else {
            char msg[48];
            snprintf(msg, sizeof(msg), "Error: HTTP %d", code);
            lv_obj_set_style_text_color(s_dot, lv_color_hex(0xFF5555), 0);
            lv_label_set_text(s_status_lbl, msg);
            lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(0xFF5555), 0);
        }
        http.end();
    }, LV_EVENT_CLICKED, NULL);

    // ── Clear token button event ───────────────────────────────────────────────
    lv_obj_add_event_cb(btn_clear, [](lv_event_t* e) {
        ytmd_token = "";
        wifiPrefs.remove(NVS_KEY_YTMD_TOKEN);
        Serial.println("[YTMD] Token cleared");

        lv_obj_set_style_text_color(s_dot, lv_color_hex(0x555555), 0);
        lv_label_set_text(s_status_lbl, "Token cleared — press Request Auth");
        lv_obj_set_style_text_color(s_status_lbl, COL_TEXT2, 0);
    }, LV_EVENT_CLICKED, NULL);

    // ── Test Connection button event ───────────────────────────────────────────
    // GET /api/v1/song  — ping that also validates the Bearer token
    lv_obj_add_event_cb(btn_test, [](lv_event_t* e) {
        if (ytmd_ip.length() == 0) {
            lv_obj_set_style_text_color(s_dot, lv_color_hex(0xFF5555), 0);
            lv_label_set_text(s_status_lbl, "Error: IP address is empty");
            lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(0xFF5555), 0);
            return;
        }
        if (ytmd_token.length() == 0) {
            lv_obj_set_style_text_color(s_dot, lv_color_hex(0xFF9944), 0);
            lv_label_set_text(s_status_lbl, "No token — press Request Auth first");
            lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(0xFF9944), 0);
            return;
        }

        lv_obj_set_style_text_color(s_dot, lv_color_hex(0x888888), 0);
        lv_label_set_text(s_status_lbl, "Testing connection...");
        lv_obj_set_style_text_color(s_status_lbl, COL_TEXT2, 0);
        lv_refr_now(NULL);

        char url[160];
        snprintf(url, sizeof(url), "http://%s:%d/api/v1/song",
                 ytmd_ip.c_str(), ytmd_port);

        char auth_header[300];
        snprintf(auth_header, sizeof(auth_header), "Bearer %s", ytmd_token.c_str());

        HTTPClient http;
        http.begin(url);
        http.addHeader("Authorization", auth_header);
        http.setTimeout(5000);
        int code = http.GET();
        Serial.printf("[YTMD] GET %s → HTTP %d\n", url, code);

        if (code == 200) {
            lv_obj_set_style_text_color(s_dot, lv_color_hex(0x44CC66), 0);
            lv_label_set_text(s_status_lbl, "Connected — server reachable, token valid");
            lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(0x44CC66), 0);
        } else if (code == 401 || code == 403) {
            lv_obj_set_style_text_color(s_dot, lv_color_hex(0xFF9944), 0);
            lv_label_set_text(s_status_lbl, "Token invalid — re-authenticate");
            lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(0xFF9944), 0);
        } else if (code < 0) {
            lv_obj_set_style_text_color(s_dot, lv_color_hex(0xFF5555), 0);
            lv_label_set_text(s_status_lbl, "Server unreachable — check IP/port");
            lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(0xFF5555), 0);
        } else {
            char msg[48];
            snprintf(msg, sizeof(msg), "Unexpected HTTP %d", code);
            lv_obj_set_style_text_color(s_dot, lv_color_hex(0xFF5555), 0);
            lv_label_set_text(s_status_lbl, msg);
            lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(0xFF5555), 0);
        }
        http.end();
    }, LV_EVENT_CLICKED, NULL);
}
