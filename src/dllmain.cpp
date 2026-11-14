#pragma warning(disable : 4996)

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

// ============================================================================
// 1. OBS Studio 核心 C ABI 类型声明（免 SDK 头文件直连 obs.dll）
// ============================================================================
typedef struct obs_module obs_module_t;
typedef struct obs_source obs_source_t;
typedef struct obs_data obs_data_t;
typedef struct obs_properties obs_properties_t;
typedef struct obs_property obs_property_t;
typedef struct gs_effect gs_effect_t;

enum obs_source_type {
    OBS_SOURCE_TYPE_INPUT = 0,
    OBS_SOURCE_TYPE_FILTER = 1,
    OBS_SOURCE_TYPE_TRANSITION = 2,
    OBS_SOURCE_TYPE_SCENE = 3,
};

enum obs_combo_type {
    OBS_COMBO_TYPE_INVALID = 0,
    OBS_COMBO_TYPE_EDITABLE = 1,
    OBS_COMBO_TYPE_LIST = 2,
};

enum obs_combo_format {
    OBS_COMBO_FORMAT_INVALID = 0,
    OBS_COMBO_FORMAT_INT = 1,
    OBS_COMBO_FORMAT_FLOAT = 2,
    OBS_COMBO_FORMAT_STRING = 3,
};

enum obs_text_type {
    OBS_TEXT_DEFAULT = 0,
    OBS_TEXT_PASSWORD = 1,
    OBS_TEXT_MULTILINE = 2,
};

#define OBS_SOURCE_VIDEO            (1 << 0)
#define OBS_SOURCE_AUDIO            (1 << 1)
#define OBS_SOURCE_ASYNC            (1 << 2)
#define OBS_SOURCE_CUSTOM_DRAW      (1 << 3)
#define OBS_SOURCE_DO_NOT_DUPLICATE (1 << 8)

#define LOG_INFO 300

typedef void (*obs_source_enum_proc_t)(obs_source_t* parent, obs_source_t* child, void* param);
typedef bool (*obs_property_clicked_t)(obs_properties_t* props, obs_property_t* property, void* data);
typedef bool (*obs_property_modified_t)(obs_properties_t* props, obs_property_t* property, obs_data_t* settings);

struct obs_source_info {
    const char* id;
    enum obs_source_type type;
    uint32_t output_flags;
    const char* (*get_name)(void* type_data);
    void* (*create)(obs_data_t* settings, obs_source_t* source);
    void (*destroy)(void* data);
    uint32_t(*get_width)(void* data);
    uint32_t(*get_height)(void* data);
    void (*get_defaults)(obs_data_t* settings);
    obs_properties_t* (*get_properties)(void* data);
    void (*update)(void* data, obs_data_t* settings);
    void (*activate)(void* data);
    void (*deactivate)(void* data);
    void (*show)(void* data);
    void (*hide)(void* data);
    void (*video_tick)(void* data, float seconds);
    void (*video_render)(void* data, gs_effect_t* effect);
    void* filter_video;
    void* filter_audio;
    void (*enum_active_sources)(void* data, obs_source_enum_proc_t enum_callback, void* param);
    void (*save)(void* data, obs_data_t* settings);
    void (*load)(void* data, obs_data_t* settings);
    void* mouse_click;
    void* mouse_move;
    void* mouse_wheel;
    void* focus;
    void* key_click;
    void* filter_remove;
    void* type_data;
    void* free_type_data;
    void* audio_render;
    void (*enum_all_sources)(void* data, obs_source_enum_proc_t enum_callback, void* param);
};

// ============================================================================
// 2. 运行时动态绑定 obs.dll 导出函数 + FFmpeg avcodec_open2 IAT 零延迟钩子
// ============================================================================
static uint32_t(*p_obs_get_version)(void) = nullptr;
static void (*p_blog)(int log_level, const char* format, ...) = nullptr;
static void (*p_obs_register_source_s)(const struct obs_source_info* info, size_t size) = nullptr;
static obs_source_t* (*p_obs_source_create_private)(const char* id, const char* name, obs_data_t* settings) = nullptr;
static void (*p_obs_source_release)(obs_source_t* source) = nullptr;
static void (*p_obs_source_update)(obs_source_t* source, obs_data_t* settings) = nullptr;
static obs_data_t* (*p_obs_source_get_settings)(const obs_source_t* source) = nullptr;
static void (*p_obs_source_video_render)(obs_source_t* source) = nullptr;
static uint32_t(*p_obs_source_get_width)(const obs_source_t* source) = nullptr;
static uint32_t(*p_obs_source_get_height)(const obs_source_t* source) = nullptr;
static void (*p_obs_source_add_active_child)(obs_source_t* parent, obs_source_t* child) = nullptr;
static void (*p_obs_source_remove_active_child)(obs_source_t* parent, obs_source_t* child) = nullptr;
static void (*p_obs_source_inc_showing)(obs_source_t* source) = nullptr;
static void (*p_obs_source_dec_showing)(obs_source_t* source) = nullptr;
static void (*p_obs_source_inc_active)(obs_source_t* source) = nullptr;
static void (*p_obs_source_dec_active)(obs_source_t* source) = nullptr;
static void (*p_obs_source_set_async_unbuffered)(obs_source_t* source, bool unbuffered) = nullptr;

static obs_data_t* (*p_obs_data_create)(void) = nullptr;
static void (*p_obs_data_release)(obs_data_t* data) = nullptr;
static void (*p_obs_data_set_string)(obs_data_t* data, const char* name, const char* val) = nullptr;
static void (*p_obs_data_set_int)(obs_data_t* data, const char* name, long long val) = nullptr;
static void (*p_obs_data_set_bool)(obs_data_t* data, const char* name, bool val) = nullptr;
static const char* (*p_obs_data_get_string)(obs_data_t* data, const char* name) = nullptr;
static long long (*p_obs_data_get_int)(obs_data_t* data, const char* name) = nullptr;
static void (*p_obs_data_set_default_string)(obs_data_t* data, const char* name, const char* val) = nullptr;
static void (*p_obs_data_set_default_int)(obs_data_t* data, const char* name, long long val) = nullptr;

static obs_properties_t* (*p_obs_properties_create)(void) = nullptr;
static obs_property_t* (*p_obs_properties_add_list)(obs_properties_t* props, const char* name, const char* desc,
    enum obs_combo_type type, enum obs_combo_format format) = nullptr;
static size_t(*p_obs_property_list_add_string)(obs_property_t* p, const char* name, const char* val) = nullptr;
static size_t(*p_obs_property_list_add_int)(obs_property_t* p, const char* name, long long val) = nullptr;
static obs_property_t* (*p_obs_properties_add_text)(obs_properties_t* props, const char* name, const char* desc,
    enum obs_text_type type) = nullptr;
static obs_property_t* (*p_obs_properties_add_int)(obs_properties_t* props, const char* name, const char* desc,
    int min, int max, int step) = nullptr;
static obs_property_t* (*p_obs_properties_add_button)(obs_properties_t* props, const char* name, const char* text,
    obs_property_clicked_t callback) = nullptr;
static void (*p_obs_property_set_modified_callback)(obs_property_t* p, obs_property_modified_t modified) = nullptr;

typedef int (*avcodec_open2_fn)(void* avctx, const void* codec, void** options);
typedef int (*av_opt_set_fn)(void* obj, const char* name, const char* val, int search_flags);
typedef int (*av_dict_set_fn)(void** pm, const char* key, const char* value, int flags);
typedef void (*av_dict_free_fn)(void** pm);

static avcodec_open2_fn Real_avcodec_open2 = nullptr;
static av_opt_set_fn p_av_opt_set = nullptr;
static av_dict_set_fn p_av_dict_set = nullptr;
static av_dict_free_fn p_av_dict_free = nullptr;
static std::atomic<bool> g_hooks_installed{ false };

static int Hooked_avcodec_open2(void* avctx, const void* codec, void** options) {
    if (avctx && p_av_opt_set) {
        p_av_opt_set(avctx, "threads", "1", 0);
        p_av_opt_set(avctx, "thread_type", "slice", 0);
        p_av_opt_set(avctx, "flags", "+low_delay", 0);
        p_av_opt_set(avctx, "flags2", "+fast", 0);
    }

    if (options && p_av_dict_set) {
        p_av_dict_set(options, "threads", "1", 0);
        p_av_dict_set(options, "thread_type", "slice", 0);
        p_av_dict_set(options, "flags", "+low_delay", 0);
        p_av_dict_set(options, "flags2", "+fast", 0);
        int ret = Real_avcodec_open2(avctx, codec, options);
        if (p_blog) {
            p_blog(LOG_INFO, "[obs-lumen-source] Hooked avcodec_open2 (with dict): forced threads=1, slice, low_delay (ret=%d)", ret);
        }
        return ret;
    }
    else if (p_av_dict_set && p_av_dict_free) {
        void* local_opts = nullptr;
        p_av_dict_set(&local_opts, "threads", "1", 0);
        p_av_dict_set(&local_opts, "thread_type", "slice", 0);
        p_av_dict_set(&local_opts, "flags", "+low_delay", 0);
        p_av_dict_set(&local_opts, "flags2", "+fast", 0);
        int ret = Real_avcodec_open2(avctx, codec, &local_opts);
        p_av_dict_free(&local_opts);
        if (p_blog) {
            p_blog(LOG_INFO, "[obs-lumen-source] Hooked avcodec_open2 (null dict): forced threads=1, slice, low_delay (ret=%d)", ret);
        }
        return ret;
    }

    return Real_avcodec_open2(avctx, codec, options);
}

static bool PatchModuleIatForAvcodecOpen2(HMODULE hModule) {
    if (!hModule) return false;

    uint8_t* base = reinterpret_cast<uint8_t*>(hModule);
    auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;

    auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    auto& imp_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imp_dir.VirtualAddress == 0 || imp_dir.Size == 0) return false;

    auto* imp = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(base + imp_dir.VirtualAddress);
    bool patched = false;

    for (; imp->Name != 0; ++imp) {
        const char* dll_name = reinterpret_cast<const char*>(base + imp->Name);
        HMODULE hImpDll = GetModuleHandleA(dll_name);

        if (hImpDll) {
            if (!p_av_opt_set) {
                p_av_opt_set = reinterpret_cast<av_opt_set_fn>(GetProcAddress(hImpDll, "av_opt_set"));
            }
            if (!p_av_dict_set) {
                p_av_dict_set = reinterpret_cast<av_dict_set_fn>(GetProcAddress(hImpDll, "av_dict_set"));
            }
            if (!p_av_dict_free) {
                p_av_dict_free = reinterpret_cast<av_dict_free_fn>(GetProcAddress(hImpDll, "av_dict_free"));
            }
        }

        if (!imp->OriginalFirstThunk || !imp->FirstThunk) continue;

        auto* orig_thunk = reinterpret_cast<PIMAGE_THUNK_DATA>(base + imp->OriginalFirstThunk);
        auto* iat_thunk = reinterpret_cast<PIMAGE_THUNK_DATA>(base + imp->FirstThunk);

        for (; orig_thunk->u1.AddressOfData != 0; ++orig_thunk, ++iat_thunk) {
            if (IMAGE_SNAP_BY_ORDINAL(orig_thunk->u1.Ordinal)) continue;

            auto* by_name = reinterpret_cast<PIMAGE_IMPORT_BY_NAME>(base + orig_thunk->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(by_name->Name), "avcodec_open2") == 0) {
                DWORD old_prot = 0;
                if (VirtualProtect(&iat_thunk->u1.Function, sizeof(ULONG_PTR), PAGE_EXECUTE_READWRITE, &old_prot)) {
                    uintptr_t cur_fn = static_cast<uintptr_t>(iat_thunk->u1.Function);
                    if (cur_fn != reinterpret_cast<uintptr_t>(&Hooked_avcodec_open2)) {
                        if (!Real_avcodec_open2) {
                            Real_avcodec_open2 = reinterpret_cast<avcodec_open2_fn>(cur_fn);
                        }
                        iat_thunk->u1.Function = reinterpret_cast<ULONG_PTR>(&Hooked_avcodec_open2);
                    }
                    VirtualProtect(&iat_thunk->u1.Function, sizeof(ULONG_PTR), old_prot, &old_prot);
                    patched = true;
                }
            }
        }
    }
    return patched;
}

static void InstallLowLatencyFFmpegHook() {
    if (g_hooks_installed.load()) return;

    HMODULE hObsFfmpeg = GetModuleHandleW(L"obs-ffmpeg.dll");
    if (!hObsFfmpeg) return;

    uint8_t* base = reinterpret_cast<uint8_t*>(hObsFfmpeg);
    auto* dos = reinterpret_cast<PIMAGE_DOS_HEADER>(base);
    if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
        auto* nt = reinterpret_cast<PIMAGE_NT_HEADERS>(base + dos->e_lfanew);
        auto& imp_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (imp_dir.VirtualAddress != 0) {
            auto* imp = reinterpret_cast<PIMAGE_IMPORT_DESCRIPTOR>(base + imp_dir.VirtualAddress);
            for (; imp->Name != 0; ++imp) {
                const char* dll_name = reinterpret_cast<const char*>(base + imp->Name);
                if (_strnicmp(dll_name, "avformat", 8) == 0) {
                    HMODULE hAvFormat = GetModuleHandleA(dll_name);
                    if (hAvFormat) {
                        PatchModuleIatForAvcodecOpen2(hAvFormat);
                    }
                }
            }
        }
    }

    if (PatchModuleIatForAvcodecOpen2(hObsFfmpeg)) {
        g_hooks_installed.store(true);
        if (p_blog) {
            p_blog(LOG_INFO, "[obs-lumen-source] Installed zero-latency avcodec_open2 IAT hook on obs-ffmpeg.dll!");
        }
    }
}

static bool LoadObsFunctions() {
    HMODULE hObs = GetModuleHandleW(L"obs.dll");
    if (!hObs) hObs = GetModuleHandleW(L"libobs.dll");
    if (!hObs) hObs = LoadLibraryW(L"obs.dll");
    if (!hObs) return false;

#define BIND_OBS(fn)                                                       \
    p_##fn = reinterpret_cast<decltype(p_##fn)>(GetProcAddress(hObs, #fn)); \
    if (!p_##fn) return false;

    BIND_OBS(obs_get_version);
    BIND_OBS(obs_register_source_s);
    BIND_OBS(obs_source_create_private);
    BIND_OBS(obs_source_release);
    BIND_OBS(obs_source_update);
    BIND_OBS(obs_source_video_render);
    BIND_OBS(obs_source_get_width);
    BIND_OBS(obs_source_get_height);
    BIND_OBS(obs_source_add_active_child);
    BIND_OBS(obs_source_remove_active_child);
    BIND_OBS(obs_source_inc_showing);
    BIND_OBS(obs_source_dec_showing);
    BIND_OBS(obs_source_inc_active);
    BIND_OBS(obs_source_dec_active);
    BIND_OBS(obs_data_create);
    BIND_OBS(obs_data_release);
    BIND_OBS(obs_data_set_string);
    BIND_OBS(obs_data_set_int);
    BIND_OBS(obs_data_set_bool);
    BIND_OBS(obs_data_get_string);
    BIND_OBS(obs_data_get_int);
    BIND_OBS(obs_data_set_default_string);
    BIND_OBS(obs_data_set_default_int);
    BIND_OBS(obs_properties_create);
    BIND_OBS(obs_properties_add_list);
    BIND_OBS(obs_property_list_add_string);
    BIND_OBS(obs_property_list_add_int);
    BIND_OBS(obs_properties_add_text);
    BIND_OBS(obs_properties_add_int);
    BIND_OBS(obs_properties_add_button);
#undef BIND_OBS

    p_blog = reinterpret_cast<decltype(p_blog)>(GetProcAddress(hObs, "blog"));
    p_obs_source_get_settings = reinterpret_cast<decltype(p_obs_source_get_settings)>(
        GetProcAddress(hObs, "obs_source_get_settings"));
    p_obs_property_set_modified_callback = reinterpret_cast<decltype(p_obs_property_set_modified_callback)>(
        GetProcAddress(hObs, "obs_property_set_modified_callback"));
    p_obs_source_set_async_unbuffered = reinterpret_cast<decltype(p_obs_source_set_async_unbuffered)>(
        GetProcAddress(hObs, "obs_source_set_async_unbuffered"));

    return true;
}

// ============================================================================
// 3. 局域网广播包参数解析 + HTTP 8080 扫描兜底 + Tally 灯光同步
// ============================================================================
struct DiscoveredNodeInfo {
    std::string ip;
    int mode = 1; // 1 = MODE_SRT_CALLER
    int srt_port = 10000;
    int rtsp_port = 8554;
    int http_port = 8080;
    int latency_ms = 25;
};

static std::mutex g_devices_mutex;
static std::vector<DiscoveredNodeInfo> g_discovered_nodes;
static std::atomic<bool> g_is_scanning{ false };
static std::atomic<bool> g_beacon_listener_running{ false };

static int ExtractJsonInt(const char* json, const char* key, int default_val) {
    if (!json || !key) return default_val;
    std::string pattern = std::string("\"") + key + "\":";
    const char* pos = strstr(json, pattern.c_str());
    if (!pos) return default_val;
    pos += pattern.size();
    while (*pos == ' ' || *pos == '\t') ++pos;
    int val = atoi(pos);
    return (val > 0) ? val : default_val;
}

static void RegisterDiscoveredNode(const DiscoveredNodeInfo& info) {
    if (info.ip.empty() || info.ip == "0.0.0.0" || info.ip.find("127.") == 0) return;
    std::lock_guard<std::mutex> lk(g_devices_mutex);
    for (auto& existing : g_discovered_nodes) {
        if (existing.ip == info.ip) {
            existing = info;
            return;
        }
    }
    g_discovered_nodes.insert(g_discovered_nodes.begin(), info);
    if (p_blog) {
        p_blog(LOG_INFO,
            "[obs-lumen-source] Discovered Lumen Node via LAN: ip=%s, srt_port=%d, http_port=%d, latency=%dms",
            info.ip.c_str(), info.srt_port, info.http_port, info.latency_ms);
    }
}

static bool GetDiscoveredNodeByIp(const std::string& ip, DiscoveredNodeInfo& out_info) {
    std::lock_guard<std::mutex> lk(g_devices_mutex);
    for (const auto& node : g_discovered_nodes) {
        if (node.ip == ip) {
            out_info = node;
            return true;
        }
    }
    return false;
}

static bool GetFirstDiscoveredNode(DiscoveredNodeInfo& out_info) {
    std::lock_guard<std::mutex> lk(g_devices_mutex);
    if (g_discovered_nodes.empty()) return false;
    out_info = g_discovered_nodes.front();
    return true;
}

static void StartUdpBeaconListener() {
    if (g_beacon_listener_running.exchange(true)) return;

    std::thread([]() {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);

        SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == INVALID_SOCKET) {
            g_beacon_listener_running.store(false);
            return;
        }

        BOOL reuse = TRUE;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        DWORD timeout_ms = 500;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));

        sockaddr_in bind_addr{};
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_port = htons(19800);
        bind_addr.sin_addr.s_addr = INADDR_ANY;

        if (bind(sock, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) {
            closesocket(sock);
            g_beacon_listener_running.store(false);
            return;
        }

        ip_mreq mreq{};
        inet_pton(AF_INET, "239.255.19.80", &mreq.imr_multiaddr);
        mreq.imr_interface.s_addr = INADDR_ANY;
        setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&mreq), sizeof(mreq));

        char buf[512];
        while (g_beacon_listener_running.load()) {
            sockaddr_in sender_addr{};
            int sender_len = sizeof(sender_addr);
            int n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                reinterpret_cast<sockaddr*>(&sender_addr), &sender_len);
            if (n > 0) {
                buf[n] = '\0';
                if (strstr(buf, "LUMEN_NODE_V1") != nullptr) {
                    char ip_str[INET_ADDRSTRLEN] = { 0 };
                    inet_ntop(AF_INET, &sender_addr.sin_addr, ip_str, sizeof(ip_str));

                    DiscoveredNodeInfo info;
                    info.ip = ip_str;
                    info.mode = ExtractJsonInt(buf, "mode", 1);
                    info.srt_port = ExtractJsonInt(buf, "srt_port", 10000);
                    info.rtsp_port = ExtractJsonInt(buf, "rtsp_port", 8554);
                    info.http_port = ExtractJsonInt(buf, "http_port", 8080);
                    info.latency_ms = ExtractJsonInt(buf, "latency_ms", 25);
                    if (info.latency_ms < 20) info.latency_ms = 25;
                    RegisterDiscoveredNode(info);
                }
            }
        }
        closesocket(sock);
        }).detach();
}

static bool ProbeLumenNodeHttp(const std::string& ip) {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return false;

    u_long non_blocking = 1;
    ioctlsocket(sock, FIONBIO, &non_blocking);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8080);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

    connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

    fd_set write_fds;
    FD_ZERO(&write_fds);
    FD_SET(sock, &write_fds);
    timeval tv{ 0, 120000 };

    bool found = false;
    if (select(0, nullptr, &write_fds, nullptr, &tv) > 0) {
        u_long blocking = 0;
        ioctlsocket(sock, FIONBIO, &blocking);
        DWORD timeout_ms = 150;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));

        const char* req = "GET /api/state HTTP/1.1\r\nHost: lumen\r\nConnection: close\r\n\r\n";
        send(sock, req, static_cast<int>(strlen(req)), 0);

        char buf[512] = { 0 };
        int n = recv(sock, buf, sizeof(buf) - 1, 0);
        if (n > 0 && (strstr(buf, "\"is_live\"") != nullptr || strstr(buf, "\"tally\"") != nullptr)) {
            found = true;
        }
    }
    closesocket(sock);
    return found;
}

static void ScanLanForLumenNodesAsync() {
    if (g_is_scanning.exchange(true)) return;

    std::thread([]() {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);

        std::vector<std::string> prefixes;
        char hostname[256] = { 0 };
        if (gethostname(hostname, sizeof(hostname)) == 0) {
            addrinfo hints{}, * res = nullptr;
            hints.ai_family = AF_INET;
            if (getaddrinfo(hostname, nullptr, &hints, &res) == 0) {
                for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
                    sockaddr_in* ipv4 = reinterpret_cast<sockaddr_in*>(p->ai_addr);
                    char ip_str[INET_ADDRSTRLEN] = { 0 };
                    inet_ntop(AF_INET, &ipv4->sin_addr, ip_str, sizeof(ip_str));
                    std::string s(ip_str);
                    if (s.find("127.") != 0 && s.find("169.254.") != 0) {
                        size_t last_dot = s.rfind('.');
                        if (last_dot != std::string::npos) {
                            prefixes.push_back(s.substr(0, last_dot + 1));
                        }
                    }
                }
                freeaddrinfo(res);
            }
        }

        for (const auto& prefix : prefixes) {
            for (int batch = 0; batch < 4; ++batch) {
                std::vector<std::thread> workers;
                for (int i = batch * 64 + 1; i <= (batch + 1) * 64 && i < 255; ++i) {
                    std::string target_ip = prefix + std::to_string(i);
                    workers.emplace_back([target_ip]() {
                        if (ProbeLumenNodeHttp(target_ip)) {
                            DiscoveredNodeInfo info;
                            info.ip = target_ip;
                            RegisterDiscoveredNode(info);
                        }
                        });
                }
                for (auto& t : workers) {
                    if (t.joinable()) t.join();
                }
            }
        }

        g_is_scanning.store(false);
        }).detach();
}

static void SendTallyToPhoneAsync(const std::string& ip, int http_port, const std::string& tally_mode) {
    if (ip.empty() || ip == "0.0.0.0") return;
    int port = (http_port > 0 && http_port <= 65535) ? http_port : 8080;
    std::thread([ip, port, tally_mode]() {
        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET) return;

        DWORD timeout_ms = 300;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<u_short>(port));
        inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

        if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            char body[64];
            snprintf(body, sizeof(body), "{\"tally\":\"%s\"}", tally_mode.c_str());
            char req[256];
            snprintf(req, sizeof(req),
                "POST /api/tally HTTP/1.1\r\n"
                "Host: %s:%d\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: %zu\r\n"
                "Connection: close\r\n\r\n%s",
                ip.c_str(), port, strlen(body), body);
            send(sock, req, static_cast<int>(strlen(req)), 0);
        }
        closesocket(sock);
        }).detach();
}

// ============================================================================
// 4. OBS Source 生命周期：广播参数自动回填 + 允许用户手动修改覆盖
// ============================================================================
enum StreamMode {
    MODE_SRT_LISTENER = 0,
    MODE_SRT_CALLER = 1,
    MODE_RTSP_PULL = 2,
};

struct LumenSourceContext {
    obs_source_t* self = nullptr;
    obs_source_t* ffmpeg_child = nullptr;
    int mode = MODE_SRT_CALLER;
    int srt_port = 10000;
    int http_port = 8080;
    int latency_ms = 25;
    std::string selected_dev = "auto";
    std::string phone_ip;
    std::string current_uri;
    bool is_active = false;
    bool is_showing = false;
    float scan_retry_timer = 0.0f;
};

static const char* lumen_source_get_name(void* type_data) {
    (void)type_data;
    return "Lumen Node Camera";
}

static void AutoFillSettingsFromNode(obs_data_t* settings, const DiscoveredNodeInfo& node) {
    if (!settings) return;
    p_obs_data_set_int(settings, "stream_mode", node.mode);
    p_obs_data_set_string(settings, "manual_ip", node.ip.c_str());
    int stream_port = (node.mode == MODE_RTSP_PULL) ? node.rtsp_port : node.srt_port;
    p_obs_data_set_int(settings, "srt_port", stream_port);
    p_obs_data_set_int(settings, "http_port", node.http_port);
    p_obs_data_set_int(settings, "latency_ms", node.latency_ms);
}

static void RebuildInternalMediaSource(LumenSourceContext* ctx) {
    if (!ctx) return;

    InstallLowLatencyFFmpegHook();

    char url_buf[512] = { 0 };
    int latency_us = ctx->latency_ms * 1000;

    if (ctx->mode == MODE_RTSP_PULL) {
        if (ctx->phone_ip.empty()) return;
        int rtsp_port = (ctx->srt_port == 10000) ? 8554 : ctx->srt_port;
        snprintf(url_buf, sizeof(url_buf), "rtsp://%s:%d/live", ctx->phone_ip.c_str(), rtsp_port);
    }
    else if (ctx->mode == MODE_SRT_CALLER) {
        if (ctx->phone_ip.empty()) return;
        snprintf(url_buf, sizeof(url_buf),
            "srt://%s:%d?mode=caller&latency=%d&rcvlatency=%d&peerlatency=%d&tlpktdrop=1&maxbw=0&oheadbw=100&rcvbuf=4194304",
            ctx->phone_ip.c_str(), ctx->srt_port, latency_us, latency_us, latency_us);
    }
    else {
        snprintf(url_buf, sizeof(url_buf),
            "srt://0.0.0.0:%d?mode=listener&latency=%d&rcvlatency=%d&peerlatency=%d&tlpktdrop=1&maxbw=0&oheadbw=100&rcvbuf=4194304",
            ctx->srt_port, latency_us, latency_us, latency_us);
    }

    if (ctx->ffmpeg_child && ctx->current_uri == url_buf) {
        return;
    }
    ctx->current_uri = url_buf;

    if (p_blog) {
        p_blog(LOG_INFO, "[obs-lumen-source] Connecting internal stream to: %s", url_buf);
    }

    obs_data_t* settings = p_obs_data_create();
    p_obs_data_set_string(settings, "input", url_buf);
    p_obs_data_set_bool(settings, "is_local_file", false);
    if (ctx->mode != MODE_RTSP_PULL) {
        p_obs_data_set_string(settings, "input_format", "mpegts");
    }
    else {
        p_obs_data_set_string(settings, "input_format", "");
    }

    p_obs_data_set_int(settings, "buffering_mb", 0);
    p_obs_data_set_bool(settings, "hw_decode", false);
    p_obs_data_set_int(settings, "reconnect_delay_sec", 1);
    p_obs_data_set_bool(settings, "restart_on_activate", false);
    p_obs_data_set_bool(settings, "clear_on_media_end", false);
    p_obs_data_set_bool(settings, "close_when_inactive", false);
    p_obs_data_set_int(settings, "speed_percent", 100);
    p_obs_data_set_string(
        settings, "ffmpeg_options",
        "fflags=nobuffer flags=low_delay analyzeduration=1 fpsprobesize=0 probesize=2048 max_delay=0");

    if (!ctx->ffmpeg_child) {
        ctx->ffmpeg_child = p_obs_source_create_private("ffmpeg_source", "Lumen_Internal_FFmpeg", settings);
        if (ctx->ffmpeg_child) {
            if (p_obs_source_set_async_unbuffered) {
                p_obs_source_set_async_unbuffered(ctx->ffmpeg_child, true);
            }
            if (ctx->self) {
                p_obs_source_add_active_child(ctx->self, ctx->ffmpeg_child);
                if (ctx->is_showing) p_obs_source_inc_showing(ctx->ffmpeg_child);
                if (ctx->is_active) p_obs_source_inc_active(ctx->ffmpeg_child);
            }
        }
    }
    else {
        p_obs_source_update(ctx->ffmpeg_child, settings);
        if (p_obs_source_set_async_unbuffered) {
            p_obs_source_set_async_unbuffered(ctx->ffmpeg_child, true);
        }
    }

    p_obs_data_release(settings);
}

static void lumen_source_update(void* data, obs_data_t* settings) {
    auto* ctx = static_cast<LumenSourceContext*>(data);
    if (!ctx || !settings) return;

    const char* selected_dev = p_obs_data_get_string(settings, "discovered_ip");
    const char* last_applied = p_obs_data_get_string(settings, "_last_applied_dev");
    ctx->selected_dev = (selected_dev && strlen(selected_dev) > 0) ? selected_dev : "auto";

    if (ctx->selected_dev != "auto" && ctx->selected_dev != "manual") {
        if (!last_applied || ctx->selected_dev != last_applied) {
            DiscoveredNodeInfo node;
            if (GetDiscoveredNodeByIp(ctx->selected_dev, node)) {
                AutoFillSettingsFromNode(settings, node);
                p_obs_data_set_string(settings, "_last_applied_dev", ctx->selected_dev.c_str());
            }
        }
    }
    else if (ctx->selected_dev == "auto") {
        const char* cur_ip = p_obs_data_get_string(settings, "manual_ip");
        if (!cur_ip || strlen(cur_ip) == 0) {
            DiscoveredNodeInfo first_node;
            if (GetFirstDiscoveredNode(first_node)) {
                AutoFillSettingsFromNode(settings, first_node);
                p_obs_data_set_string(settings, "_last_applied_dev", first_node.ip.c_str());
            }
        }
    }

    ctx->mode = static_cast<int>(p_obs_data_get_int(settings, "stream_mode"));
    ctx->srt_port = static_cast<int>(p_obs_data_get_int(settings, "srt_port"));
    ctx->http_port = static_cast<int>(p_obs_data_get_int(settings, "http_port"));
    ctx->latency_ms = static_cast<int>(p_obs_data_get_int(settings, "latency_ms"));
    if (ctx->srt_port <= 0) ctx->srt_port = 10000;
    if (ctx->http_port <= 0) ctx->http_port = 8080;
    if (ctx->latency_ms < 15) ctx->latency_ms = 25;

    const char* manual_ip = p_obs_data_get_string(settings, "manual_ip");
    if (manual_ip && strlen(manual_ip) > 0) {
        ctx->phone_ip = manual_ip;
    }

    RebuildInternalMediaSource(ctx);
}

static void* lumen_source_create(obs_data_t* settings, obs_source_t* source) {
    auto* ctx = new LumenSourceContext();
    ctx->self = source;
    StartUdpBeaconListener();
    ScanLanForLumenNodesAsync();
    lumen_source_update(ctx, settings);
    return ctx;
}

static void lumen_source_destroy(void* data) {
    auto* ctx = static_cast<LumenSourceContext*>(data);
    if (!ctx) return;

    if (!ctx->phone_ip.empty()) {
        SendTallyToPhoneAsync(ctx->phone_ip, ctx->http_port, "off");
    }

    if (ctx->ffmpeg_child) {
        if (ctx->is_active) p_obs_source_dec_active(ctx->ffmpeg_child);
        if (ctx->is_showing) p_obs_source_dec_showing(ctx->ffmpeg_child);
        if (ctx->self) p_obs_source_remove_active_child(ctx->self, ctx->ffmpeg_child);
        p_obs_source_release(ctx->ffmpeg_child);
        ctx->ffmpeg_child = nullptr;
    }
    delete ctx;
}

static uint32_t lumen_source_get_width(void* data) {
    auto* ctx = static_cast<LumenSourceContext*>(data);
    return (ctx && ctx->ffmpeg_child) ? p_obs_source_get_width(ctx->ffmpeg_child) : 0;
}

static uint32_t lumen_source_get_height(void* data) {
    auto* ctx = static_cast<LumenSourceContext*>(data);
    return (ctx && ctx->ffmpeg_child) ? p_obs_source_get_height(ctx->ffmpeg_child) : 0;
}

static void lumen_source_video_tick(void* data, float seconds) {
    auto* ctx = static_cast<LumenSourceContext*>(data);
    if (!ctx) return;

    uint32_t w = (ctx->ffmpeg_child) ? p_obs_source_get_width(ctx->ffmpeg_child) : 0;
    if (w > 0) {
        ctx->scan_retry_timer = 0.0f;
        return;
    }

    if (ctx->selected_dev == "auto") {
        DiscoveredNodeInfo first_node;
        if (GetFirstDiscoveredNode(first_node)) {
            if (ctx->phone_ip != first_node.ip || ctx->srt_port != first_node.srt_port) {
                ctx->phone_ip = first_node.ip;
                ctx->mode = first_node.mode;
                ctx->srt_port = first_node.srt_port;
                ctx->http_port = first_node.http_port;
                ctx->latency_ms = first_node.latency_ms;

                if (p_obs_source_get_settings && ctx->self) {
                    obs_data_t* settings = p_obs_source_get_settings(ctx->self);
                    if (settings) {
                        AutoFillSettingsFromNode(settings, first_node);
                        p_obs_data_set_string(settings, "_last_applied_dev", first_node.ip.c_str());
                        p_obs_data_release(settings);
                    }
                }

                RebuildInternalMediaSource(ctx);
                SendTallyToPhoneAsync(ctx->phone_ip, ctx->http_port,
                    ctx->is_active ? "program" : (ctx->is_showing ? "preview" : "off"));
                return;
            }
        }
    }

    ctx->scan_retry_timer += seconds;
    if (ctx->scan_retry_timer >= 4.0f) {
        ctx->scan_retry_timer = 0.0f;
        ScanLanForLumenNodesAsync();
    }
}

static void lumen_source_video_render(void* data, gs_effect_t* effect) {
    (void)effect;
    auto* ctx = static_cast<LumenSourceContext*>(data);
    if (ctx && ctx->ffmpeg_child) {
        p_obs_source_video_render(ctx->ffmpeg_child);
    }
}

static void lumen_source_enum_active_sources(void* data, obs_source_enum_proc_t enum_callback, void* param) {
    auto* ctx = static_cast<LumenSourceContext*>(data);
    if (ctx && ctx->ffmpeg_child) {
        enum_callback(ctx->self, ctx->ffmpeg_child, param);
    }
}

static void lumen_source_activate(void* data) {
    auto* ctx = static_cast<LumenSourceContext*>(data);
    if (!ctx) return;
    ctx->is_active = true;
    if (ctx->ffmpeg_child) p_obs_source_inc_active(ctx->ffmpeg_child);
    SendTallyToPhoneAsync(ctx->phone_ip, ctx->http_port, "program");
}

static void lumen_source_deactivate(void* data) {
    auto* ctx = static_cast<LumenSourceContext*>(data);
    if (!ctx) return;
    ctx->is_active = false;
    if (ctx->ffmpeg_child) p_obs_source_dec_active(ctx->ffmpeg_child);
    SendTallyToPhoneAsync(ctx->phone_ip, ctx->http_port, ctx->is_showing ? "preview" : "off");
}

static void lumen_source_show(void* data) {
    auto* ctx = static_cast<LumenSourceContext*>(data);
    if (!ctx) return;
    ctx->is_showing = true;
    if (ctx->ffmpeg_child) p_obs_source_inc_showing(ctx->ffmpeg_child);
    if (!ctx->is_active) {
        SendTallyToPhoneAsync(ctx->phone_ip, ctx->http_port, "preview");
    }
}

static void lumen_source_hide(void* data) {
    auto* ctx = static_cast<LumenSourceContext*>(data);
    if (!ctx) return;
    ctx->is_showing = false;
    if (ctx->ffmpeg_child) p_obs_source_dec_showing(ctx->ffmpeg_child);
    if (!ctx->is_active) {
        SendTallyToPhoneAsync(ctx->phone_ip, ctx->http_port, "off");
    }
}

static void lumen_source_get_defaults(obs_data_t* settings) {
    p_obs_data_set_default_string(settings, "discovered_ip", "auto");
    p_obs_data_set_default_int(settings, "stream_mode", MODE_SRT_CALLER);
    p_obs_data_set_default_int(settings, "srt_port", 10000);
    p_obs_data_set_default_int(settings, "http_port", 8080);
    p_obs_data_set_default_int(settings, "latency_ms", 25);

    DiscoveredNodeInfo first_node;
    if (GetFirstDiscoveredNode(first_node)) {
        p_obs_data_set_default_string(settings, "manual_ip", first_node.ip.c_str());
    }
    else {
        p_obs_data_set_default_string(settings, "manual_ip", "");
    }
}

static bool OnDiscoveredDeviceModified(obs_properties_t* props, obs_property_t* property, obs_data_t* settings) {
    (void)props;
    (void)property;
    if (!settings) return false;

    const char* selected_dev = p_obs_data_get_string(settings, "discovered_ip");
    const char* last_applied = p_obs_data_get_string(settings, "_last_applied_dev");
    if (!selected_dev) return false;

    if (strcmp(selected_dev, "manual") == 0) {
        return false;
    }

    if (strcmp(selected_dev, "auto") == 0) {
        DiscoveredNodeInfo first_node;
        if (GetFirstDiscoveredNode(first_node)) {
            const char* cur_ip = p_obs_data_get_string(settings, "manual_ip");
            if (!cur_ip || strlen(cur_ip) == 0) {
                AutoFillSettingsFromNode(settings, first_node);
                p_obs_data_set_string(settings, "_last_applied_dev", "auto");
                return true;
            }
        }
        return false;
    }

    if (!last_applied || strcmp(selected_dev, last_applied) != 0) {
        DiscoveredNodeInfo node;
        if (GetDiscoveredNodeByIp(selected_dev, node)) {
            AutoFillSettingsFromNode(settings, node);
            p_obs_data_set_string(settings, "_last_applied_dev", selected_dev);
            return true;
        }
    }
    return false;
}

static bool OnScanButtonClicked(obs_properties_t* props, obs_property_t* property, void* data) {
    (void)props;
    (void)property;
    ScanLanForLumenNodesAsync();

    auto* ctx = static_cast<LumenSourceContext*>(data);
    if (ctx && ctx->self && p_obs_source_get_settings) {
        DiscoveredNodeInfo first_node;
        if (GetFirstDiscoveredNode(first_node)) {
            obs_data_t* settings = p_obs_source_get_settings(ctx->self);
            if (settings) {
                AutoFillSettingsFromNode(settings, first_node);
                p_obs_data_release(settings);
                lumen_source_update(ctx, settings);
                return true;
            }
        }
    }
    return true;
}

static obs_properties_t* lumen_source_get_properties(void* data) {
    (void)data;
    obs_properties_t* props = p_obs_properties_create();

    obs_property_t* dev_list = p_obs_properties_add_list(
        props, "discovered_ip", "Discovered Camera (Auto-Fill)", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

    p_obs_property_list_add_string(dev_list, "[Auto] Auto-Discover & Auto-Fill First Camera", "auto");

    {
        std::lock_guard<std::mutex> lk(g_devices_mutex);
        for (const auto& node : g_discovered_nodes) {
            char label[128];
            snprintf(label, sizeof(label), "Lumen Node (%s : %d)", node.ip.c_str(), node.srt_port);
            p_obs_property_list_add_string(dev_list, label, node.ip.c_str());
        }
    }
    p_obs_property_list_add_string(dev_list, "[Manual] Lock Custom IP / Port Settings", "manual");

    if (p_obs_property_set_modified_callback) {
        p_obs_property_set_modified_callback(dev_list, OnDiscoveredDeviceModified);
    }

    p_obs_properties_add_button(props, "scan_btn", "Scan LAN & Auto-Fill Parameters", OnScanButtonClicked);

    obs_property_t* mode_list = p_obs_properties_add_list(
        props, "stream_mode", "Connection Mode", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    p_obs_property_list_add_int(mode_list, "SRT Caller (Recommended: Pull from Phone Listener)", MODE_SRT_CALLER);
    p_obs_property_list_add_int(mode_list, "SRT Listener (Wait for Phone to Push)", MODE_SRT_LISTENER);
    p_obs_property_list_add_int(mode_list, "RTSP Pull (rtsp://IP:Port/live)", MODE_RTSP_PULL);

    p_obs_properties_add_text(props, "manual_ip", "Target IP Address (Auto-filled / Editable)", OBS_TEXT_DEFAULT);
    p_obs_properties_add_int(props, "srt_port", "Stream Port (SRT: 10000 / RTSP: 8554)", 1024, 65535, 1);
    p_obs_properties_add_int(props, "http_port", "Tally & Control HTTP Port", 1024, 65535, 1);
    p_obs_properties_add_int(props, "latency_ms", "SRT Latency (ms)", 15, 500, 5);

    return props;
}

static struct obs_source_info g_lumen_source_info = {};

// ============================================================================
// 5. OBS 插件标准导出入口函数
// ============================================================================
static obs_module_t* g_obs_module_pointer = nullptr;

extern "C" __declspec(dllexport) void obs_module_set_pointer(obs_module_t * module) {
    g_obs_module_pointer = module;
}

extern "C" __declspec(dllexport) uint32_t obs_module_ver(void) {
    if (!p_obs_get_version) {
        LoadObsFunctions();
    }
    return p_obs_get_version ? p_obs_get_version() : ((30U << 24) | (0U << 16) | 0U);
}

extern "C" __declspec(dllexport) const char* obs_module_name(void) {
    return "lumen-node-obs";
}

extern "C" __declspec(dllexport) const char* obs_module_description(void) {
    return "Lumen Node Camera Source for OBS (Low-Latency SRT)";
}

extern "C" __declspec(dllexport) bool obs_module_load(void) {
    if (!LoadObsFunctions()) {
        return false;
    }

    InstallLowLatencyFFmpegHook();
    StartUdpBeaconListener();
    ScanLanForLumenNodesAsync();

    memset(&g_lumen_source_info, 0, sizeof(g_lumen_source_info));
    g_lumen_source_info.id = "lumen_node_source";
    g_lumen_source_info.type = OBS_SOURCE_TYPE_INPUT;
    g_lumen_source_info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_DO_NOT_DUPLICATE;
    g_lumen_source_info.get_name = lumen_source_get_name;
    g_lumen_source_info.create = lumen_source_create;
    g_lumen_source_info.destroy = lumen_source_destroy;
    g_lumen_source_info.get_width = lumen_source_get_width;
    g_lumen_source_info.get_height = lumen_source_get_height;
    g_lumen_source_info.get_defaults = lumen_source_get_defaults;
    g_lumen_source_info.get_properties = lumen_source_get_properties;
    g_lumen_source_info.update = lumen_source_update;
    g_lumen_source_info.activate = lumen_source_activate;
    g_lumen_source_info.deactivate = lumen_source_deactivate;
    g_lumen_source_info.show = lumen_source_show;
    g_lumen_source_info.hide = lumen_source_hide;
    g_lumen_source_info.video_tick = lumen_source_video_tick;
    g_lumen_source_info.video_render = lumen_source_video_render;
    g_lumen_source_info.enum_active_sources = lumen_source_enum_active_sources;
    g_lumen_source_info.enum_all_sources = lumen_source_enum_active_sources;

    p_obs_register_source_s(&g_lumen_source_info, sizeof(g_lumen_source_info));
    return true;
}

extern "C" __declspec(dllexport) void obs_module_unload(void) {
    g_beacon_listener_running.store(false);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    (void)hModule;
    (void)ul_reason_for_call;
    (void)lpReserved;
    return TRUE;
}