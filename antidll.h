#pragma once

#include <ISmmPlugin.h>
#include <entity2/entitysystem.h>
#include "menus.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <cmath>

class AntiDLL : public ISmmPlugin, public IMetamodListener
{
public:
    bool Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late);
    bool Unload(char* error, size_t maxlen);
    void AllPluginsLoaded();
    const char* GetAuthor();
    const char* GetName();
    const char* GetDescription();
    const char* GetURL();
    const char* GetLicense();
    const char* GetVersion();
    const char* GetDate();
    const char* GetLogTag();
};

extern AntiDLL g_AntiDLL;
PLUGIN_GLOBALVARS();
