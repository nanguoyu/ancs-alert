// main/wx_phone.c —— iPhone 型号代码与名称(纯逻辑)。
#include "wx_phone.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char *model;
    const char *name;
} model_name_t;

static const model_name_t MODELS[] = {
    { "iPhone10,1", "iPhone 8" },        { "iPhone10,4", "iPhone 8" },
    { "iPhone10,2", "iPhone 8 Plus" },   { "iPhone10,5", "iPhone 8 Plus" },
    { "iPhone10,3", "iPhone X" },        { "iPhone10,6", "iPhone X" },
    { "iPhone11,2", "iPhone XS" },
    { "iPhone11,4", "iPhone XS Max" },   { "iPhone11,6", "iPhone XS Max" },
    { "iPhone11,8", "iPhone XR" },
    { "iPhone12,1", "iPhone 11" },       { "iPhone12,3", "iPhone 11 Pro" },
    { "iPhone12,5", "iPhone 11 Pro Max" }, { "iPhone12,8", "iPhone SE" },
    { "iPhone13,1", "iPhone 12 mini" },  { "iPhone13,2", "iPhone 12" },
    { "iPhone13,3", "iPhone 12 Pro" },   { "iPhone13,4", "iPhone 12 Pro Max" },
    { "iPhone14,4", "iPhone 13 mini" },  { "iPhone14,5", "iPhone 13" },
    { "iPhone14,2", "iPhone 13 Pro" },   { "iPhone14,3", "iPhone 13 Pro Max" },
    { "iPhone14,6", "iPhone SE" },
    { "iPhone14,7", "iPhone 14" },       { "iPhone14,8", "iPhone 14 Plus" },
    { "iPhone15,2", "iPhone 14 Pro" },   { "iPhone15,3", "iPhone 14 Pro Max" },
    { "iPhone15,4", "iPhone 15" },       { "iPhone15,5", "iPhone 15 Plus" },
    { "iPhone16,1", "iPhone 15 Pro" },   { "iPhone16,2", "iPhone 15 Pro Max" },
    { "iPhone17,1", "iPhone 16 Pro" },   { "iPhone17,2", "iPhone 16 Pro Max" },
    { "iPhone17,3", "iPhone 16" },       { "iPhone17,4", "iPhone 16 Plus" },
    { "iPhone17,5", "iPhone 16e" },
};

void wx_phone_model_name(const char *model, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    if (!model || model[0] == '\0') {
        snprintf(out, cap, "iPhone");
        return;
    }
    for (size_t i = 0; i < sizeof(MODELS) / sizeof(MODELS[0]); i++) {
        if (strcmp(model, MODELS[i].model) == 0) {
            snprintf(out, cap, "%s", MODELS[i].name);
            return;
        }
    }
    snprintf(out, cap, "%s", model);
}

const char *wx_phone_short_name(const char *name)
{
    static const char prefix[] = "iPhone ";
    const size_t n = sizeof(prefix) - 1;
    if (name && strncmp(name, prefix, n) == 0 && name[n] != '\0') return name + n;
    return name ? name : "";
}
