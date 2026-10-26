// main/wx_tg_text.c —— 转发到 Telegram 的文字(纯逻辑)。
#include "wx_tg_text.h"
#include "wx_ancs_proto.h"   // wx_utf8_trim_incomplete

#include <stdio.h>
#include <string.h>

typedef struct {
    char *out;
    size_t cap;
    size_t len;
    bool full;
} text_t;

static void append(text_t *t, const char *s)
{
    if (t->full || !s) return;
    const size_t room = t->cap - 1 - t->len;
    size_t n = strlen(s);
    if (n > room) {
        n = room;
        t->full = true;
    }
    memcpy(t->out + t->len, s, n);
    t->len += n;
    t->out[t->len] = '\0';
    if (t->full) t->len = wx_utf8_trim_incomplete(t->out, t->len);
}

static bool present(const char *s)
{
    return s && s[0] != '\0';
}

size_t wx_tg_format(char *out, size_t cap, wx_lang_t lang, const char *app_name, const char *phone,
                    const char *title, bool title_generic, const char *message)
{
    if (!out || cap == 0) return 0;
    out[0] = '\0';
    text_t t = { .out = out, .cap = cap };
    const bool zh = lang == WX_LANG_ZH;

    append(&t, zh ? "【" : "[");
    append(&t, app_name);
    if (present(phone)) {
        append(&t, zh ? "，" : ", ");
        append(&t, phone);
    }
    append(&t, zh ? "】" : "] ");
    const bool has_title = !title_generic && present(title);
    if (has_title) append(&t, title);
    if (present(message)) {
        append(&t, has_title ? "\n" : "");
        append(&t, message);
    } else if (has_title) {
        append(&t, zh ? " 发来新消息" : " sent a new message");
    } else {
        append(&t, zh ? "有新消息" : "New message");
    }
    return t.len;
}

static bool is_token_char(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           c == '_' || c == '-';
}

bool wx_tg_token_valid(const char *token)
{
    if (!token) return false;
    size_t i = 0;
    while (token[i] >= '0' && token[i] <= '9') i++;
    if (i < 5 || i > 16 || token[i] != ':') return false;
    const size_t secret = ++i;
    while (is_token_char(token[i])) i++;
    const size_t secret_len = i - secret;
    return token[i] == '\0' && secret_len >= 30 && i <= WX_TG_TOKEN_MAX;
}

void wx_tg_token_hint(const char *token, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!wx_tg_token_valid(token)) return;
    const char *colon = strchr(token, ':');
    const size_t len = strlen(token);
    snprintf(out, cap, "%.*s:••••%s", (int)(colon - token), token, token + len - 4);
}
