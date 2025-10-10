// src/main.cpp
#include <imgui.h>
#include <utils/flog.h>
#include <module.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <gui/style.h>
#include <config.h>
#include <gui/smgui.h>
#include <utils/optionlist.h>

#include <sidekiq_api.h>
#include <sidekiq_params.h>
#include <sidekiq_types.h>

#include <atomic>
#include <thread>
#include <vector>
#include <string>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <unordered_set>
#include <fstream>
#include <map>
#include <chrono>
#include <mutex>

#define CONCAT_STR(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "sidekiq_source",
    /* Description:     */ "Epiq Sidekiq source module for SDR++",
    /* Author:          */ "Aaron",
    /* Version:         */ 0, 2, 2,
    /* Max instances    */ 1
};

static ConfigManager g_config;

class SidekiqSourceModule : public ModuleManager::Instance {
public:
    SidekiqSourceModule(std::string name) {
        this->name = name;

        handler.ctx             = this;
        handler.selectHandler   = &onSelect;
        handler.deselectHandler = &onDeselect;
        handler.menuHandler     = &onMenu;
        handler.startHandler    = &onStart;
        handler.stopHandler     = &onStop;
        handler.tuneHandler     = &onTune;
        handler.stream          = &stream;

        // Defaults
        sampleRate = 10'000'000;
        bandwidth  = 10'000'000;
        centerFreq = 100'000'000;
        gainMode   = skiq_rx_gain_manual;
        gainIndex  = 30;
        streamMode = skiq_rx_stream_mode_high_tput;
        gainMin    = 0;
        gainMax    = 76;

        // Presets / NV state
        useProfiles        = false;
        nvProfilesEnabled  = false;
        nvSelectedRateHz   = 10'000'000;
        nvBwPresetHz       = 8'000'000; // 80%

        // Stream modes
        modes.define("High Throughput", "High Throughput", skiq_rx_stream_mode_high_tput);
        modes.define("Low Latency",     "Low Latency",     skiq_rx_stream_mode_low_latency);
        modes.define("Balanced",        "Balanced",        skiq_rx_stream_mode_balanced);

        refreshDevices();

        // Restore config
        g_config.acquire();
        if (g_config.conf.contains("device")) {
            std::string lastDev = g_config.conf["device"];
            if (devices.keyExists(lastDev)) deviceIdx = devices.keyId(lastDev);
        }
        if (g_config.conf.contains("streamMode")) {
            int sm = g_config.conf["streamMode"];
            for (int i = 0; i < modes.size(); ++i) {
                if ((int)modes.value(i) == sm) { streamModeIdx = i; streamMode = modes.value(i); break; }
            }
        }
        if (g_config.conf.contains("sampleRate"))       sampleRate       = (int)g_config.conf["sampleRate"];
        if (g_config.conf.contains("bandwidth"))        bandwidth        = (int)g_config.conf["bandwidth"];
        if (g_config.conf.contains("gainMode"))         gainMode         = (skiq_rx_gain_t)((int)g_config.conf["gainMode"]);
        if (g_config.conf.contains("gainIndex"))        gainIndex        = (int)g_config.conf["gainIndex"];
        if (g_config.conf.contains("useProfiles"))      useProfiles      = (bool)g_config.conf["useProfiles"];
        if (g_config.conf.contains("nvSelectedRateHz")) nvSelectedRateHz = (int)g_config.conf["nvSelectedRateHz"];
        if (g_config.conf.contains("nvBwPresetHz"))     nvBwPresetHz     = (int)g_config.conf["nvBwPresetHz"];
        if (g_config.conf.contains("rfPortByDevice"))   rfPortByDevice   = g_config.conf["rfPortByDevice"];
        g_config.release();

        core::setInputSampleRate(sampleRate);
        sigpath::sourceManager.registerSource("Sidekiq", &handler);
    }

    ~SidekiqSourceModule() {
        stopStream();
        sigpath::sourceManager.unregisterSource("Sidekiq");
        safeExitSdk();
    }

    void postInit() {}

    void enable()  { enabled = true;  }
    void disable() { enabled = false; }
    bool isEnabled() { return enabled; }

    // Enumerate cards (prefer PCIe). Also resets the SDK if the topology changed so serial mappings don't linger.
    void refreshDevices(bool allow_short_retry = true) {
        std::lock_guard<std::recursive_mutex> lk(sdkMtx);

        // Cooldown after stop/unplug to avoid SDK touching a disappearing card
        auto now = std::chrono::steady_clock::now();
        if (now - lastStopOrUnplug_ < std::chrono::milliseconds(3000)) {
            flog::info("Sidekiq: cooldown after stop/unplug; skipping SDK probe this refresh");
            devices.clear();
            hadAnyDevice = false;
            deviceIdx = handleIdx = rfPortIdx = 0;
            lastCardSet_.clear();
            return;
        }

        // Init SDK if needed
        if (!libInited.load()) {
            int32_t st_init = skiq_init_without_cards();
            if (st_init != 0) {
                flog::error("Sidekiq: skiq_init_without_cards failed ({})", (int)st_init);
                return;
            }
            libInited.store(true);
        }

        auto do_get = [&](skiq_xport_type_t x, uint8_t& out_num, uint8_t* out_cards){
            std::memset(out_cards, 0, SKIQ_MAX_NUM_CARDS);
            out_num = 0;
            int32_t rc = skiq_get_cards(x, &out_num, out_cards);
            flog::info("Sidekiq: get_cards(xport={}, rc={}, num={})", (int)x, (int)rc, (int)out_num);
            return rc;
        };

        // First probe
        uint8_t cards[SKIQ_MAX_NUM_CARDS] = {0};
        uint8_t num = 0;
        int32_t st = do_get(skiq_xport_type_pcie, num, cards);
        if (st != 0 || num == 0) st = do_get(skiq_xport_type_auto, num, cards);

        // Short retry if 0
        if (allow_short_retry && (st == 0 && num == 0)) {
            for (int i = 0; i < 5; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                st = do_get(skiq_xport_type_auto, num, cards);
                if (st == 0 && num > 0) break;
            }
        }

        // Compare topology with last seen
        auto curSet = toVec(cards, num);
        bool topoChanged = (curSet != lastCardSet_);

        if (topoChanged) {
            flog::info("Sidekiq: card topology changed -> reinit SDK");
            // Full reset so lib forgets previous serial↔slot mapping
            safeExitSdk();
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            int32_t st_init = skiq_init_without_cards();
            if (st_init != 0) {
                flog::error("Sidekiq: reinit after topology change failed ({})", (int)st_init);
                devices.clear();
                hadAnyDevice = false;
                lastCardSet_.clear();
                return;
            }
            libInited.store(true);
            knownSerialByCard.clear();
            firstSeen.clear();

            // Re-probe once from a clean slate
            std::memset(cards, 0, sizeof(cards));
            num = 0;
            st = do_get(skiq_xport_type_auto, num, cards);
            curSet = toVec(cards, num);
        }

        lastCardSet_ = curSet;
        devices.clear();

        if (st != 0 || num == 0) {
            flog::warn("Sidekiq: no cards detected (st={}, num={})", (int)st, (int)num);
            hadAnyDevice = false;
            deviceIdx     = 0;
            handleIdx     = 0;
            rfPortIdx     = 0;
            streamModeIdx = std::min(streamModeIdx, std::max(0, modes.size()-1));
            // If nothing is present, also clear SDK so the next plug-in starts fresh
            safeExitSdk();
            lastCardSet_.clear();
            return;
        }
        hadAnyDevice = true;

        // Just list cards; do not enable or read params here
        std::unordered_set<std::string> seenKeys;
        auto tnow = std::chrono::steady_clock::now();
        for (uint8_t i = 0; i < num; ++i) {
            uint8_t cid = cards[i];
            if (!firstSeen.count(cid)) firstSeen[cid] = tnow;

            char tmp[32]; std::snprintf(tmp, sizeof(tmp), "card-%u", cid);
            std::string key  = tmp;
            std::string disp = key;

            auto itSer = knownSerialByCard.find(cid);
            if (itSer != knownSerialByCard.end() && !itSer->second.empty()) {
                disp = itSer->second + " (" + key + ")";
            }

            if (seenKeys.insert(key).second) {
                devices.define(key, disp.c_str(), cid);
            }
        }

        deviceIdx     = std::min(deviceIdx,     std::max(0, devices.size()-1));
        handleIdx     = std::min(handleIdx,     std::max(0, handles.size()-1));
        rfPortIdx     = std::min(rfPortIdx,     std::max(0, rfPorts.size()-1));
        streamModeIdx = std::min(streamModeIdx, std::max(0, modes.size()-1));
    }

    void reinitAndRescan() {
        std::lock_guard<std::recursive_mutex> lk(sdkMtx);
        safeExitSdk();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        int32_t st_init = skiq_init_without_cards();
        if (st_init != 0) {
            flog::error("Sidekiq: reinit failed ({})", (int)st_init);
            return;
        }
        libInited.store(true);
        knownSerialByCard.clear();
        firstSeen.clear();
        lastCardSet_.clear();
        refreshDevices(/*allow_short_retry=*/true);
    }

private:
    // ===== SDR++ handlers =====
    static void onSelect(void* ctx) {
        auto* s = static_cast<SidekiqSourceModule*>(ctx);
        core::setInputSampleRate(s->sampleRate);
        flog::info("SidekiqSource {}: Menu Select", s->name);
    }
    static void onDeselect(void* ctx) {
        auto* s = static_cast<SidekiqSourceModule*>(ctx);
        flog::info("SidekiqSource {}: Menu Deselect", s->name);
    }
    static void onStart(void* ctx) {
        auto* s = static_cast<SidekiqSourceModule*>(ctx);
        s->startStream();
    }
    static void onStop(void* ctx) {
        auto* s = static_cast<SidekiqSourceModule*>(ctx);
        s->stopStream();
    }
    static void onTune(double freq, void* ctx) {
        auto* s = static_cast<SidekiqSourceModule*>(ctx);
        s->centerFreq = static_cast<uint64_t>(freq);
        std::lock_guard<std::recursive_mutex> lk(s->sdkMtx);
        if (s->running.load()) {
            (void)skiq_write_rx_LO_freq(s->activeCard, s->activeRxHdl, s->centerFreq);
        }
        flog::info("SidekiqSource {}: Tune {} Hz", s->name, (uint64_t)freq);
    }

    // ===== GUI =====
    static void onMenu(void* ctx) {
        auto* s = static_cast<SidekiqSourceModule*>(ctx);

        // Refresh & Re-scan (reinit)
        SmGui::FillWidth();
        if (SmGui::Button(CONCAT_STR("Refresh##_sidekiq_refr_", s->name))) {
            s->refreshDevices();
        }
        SmGui::SameLine();
        if (SmGui::Button(CONCAT_STR("Re-scan (reinit)##_sidekiq_reinit_", s->name))) {
            s->reinitAndRescan();
        }

        // Device dropdown (LOCKED while running)
        if (s->running.load()) SmGui::BeginDisabled();
        SmGui::FillWidth();
        SmGui::ForceSync();

        if (s->devices.empty()) {
            SmGui::Text("No Sidekiq devices found. Plug in and click Refresh or Re-scan.");
        } else {
            if (SmGui::Combo(CONCAT_STR("Device##_sidekiq_dev_", s->name), &s->deviceIdx, s->devices.txt)) {
                g_config.acquire();
                g_config.conf["device"] = s->devices.key(s->deviceIdx);
                g_config.release(true);
                s->enumerateHandlesForCard(s->devices.value(s->deviceIdx));
            }
        }

        if (!s->activeSerial.empty()) {
            std::string ser = std::string("Connected serial: ") + s->activeSerial;
            SmGui::Text(ser.c_str());
        }

        // RX handle (LOCKED while running)
        SmGui::FillWidth();
        if (!s->handles.empty()) {
            int prevHandleIdx = s->handleIdx;
            if (SmGui::Combo(CONCAT_STR("RX Handle##_sidekiq_hdl_", s->name), &s->handleIdx, s->handles.txt)) {
                if (prevHandleIdx != s->handleIdx) {
                    s->enumerateRfPorts(s->devices.value(s->deviceIdx), s->handles.value(s->handleIdx));
                    if (s->nvProfilesEnabled && s->handles.value(s->handleIdx) == skiq_rx_hdl_B1) {
                        if (s->rfPorts.keyExists("J2")) {
                            s->rfPortIdx = s->rfPorts.keyId("J2");
                            s->saveRfPortChoice();
                        }
                    }
                }
            }
        } else {
            SmGui::Text("No RX handles (select a device).");
        }

        // Stream mode (LOCKED while running)
        SmGui::FillWidth();
        if (SmGui::Combo(CONCAT_STR("Stream mode##_sidekiq_mode_", s->name), &s->streamModeIdx, s->modes.txt)) {
            s->streamMode = s->modes.value(s->streamModeIdx);
            g_config.acquire();
            g_config.conf["streamMode"] = (int)s->streamMode;
            g_config.release(true);
        }

        // ---- Tuning controls: NV presets OR manual ----
        auto drawManualSRBW = [&](){
            SmGui::LeftLabel("Sample rate (Hz)");
            SmGui::FillWidth();
            int sr_tmp = (int)s->sampleRate;
            if (SmGui::InputInt(CONCAT_STR("##_sidekiq_sr_", s->name), &sr_tmp)) {
                s->sampleRate = std::max(100000, sr_tmp);
                core::setInputSampleRate(s->sampleRate);
                g_config.acquire();
                g_config.conf["sampleRate"] = (int)s->sampleRate;
                g_config.release(true);
            }

            SmGui::LeftLabel("Chan bandwidth (Hz)");
            SmGui::FillWidth();
            int bw_tmp = (int)s->bandwidth;
            if (SmGui::InputInt(CONCAT_STR("##_sidekiq_bw_", s->name), &bw_tmp)) {
                s->bandwidth = std::max(100000, bw_tmp);
                g_config.acquire();
                g_config.conf["bandwidth"] = (int)s->bandwidth;
                g_config.release(true);
            }
        };

        auto drawNvPresets = [&](){
            // Curated pairs
            struct Pair { uint32_t sr; uint32_t bw; const char* label; };
            static const Pair knownPairs[] = {
                {  250000,   155000,  "0.25 Msps / 155 kHz" },
                {  740740,   400000,  "0.74074 Msps / 400 kHz" },
                {  750000,   450000,  "0.75 Msps / 450 kHz" },
                { 1000000,   800000,  "1 Msps / 800 kHz" },
                { 2500000,  2000000,  "2.5 Msps / 2 MHz" },
                { 5000000,  4000000,  "5 Msps / 4 MHz" },
                {61440000, 50000000,  "61.44 Msps / 50 MHz (example)" },
            };
            int curIdx = 0, idx = 0;
            std::string pairItems;
            for (const auto& p : knownPairs) {
                pairItems += p.label; pairItems.push_back('\0');
                if ((int)p.sr == (int)s->nvSelectedRateHz && (int)p.bw == (int)s->nvBwPresetHz) curIdx = idx;
                ++idx;
            }
            pairItems.push_back('\0');
            SmGui::LeftLabel("Known-good SR/BW");
            SmGui::FillWidth();
            if (SmGui::Combo(CONCAT_STR("##_sidekiq_nv_pair_", s->name), &curIdx, pairItems.c_str())) {
                s->nvSelectedRateHz = knownPairs[curIdx].sr;
                s->nvBwPresetHz     = knownPairs[curIdx].bw;
                s->saveNvPresetConfig();
            }

            // Advanced lists (known-good SRs + BW % with 80% default)
            SmGui::LeftLabel("Advanced");
            SmGui::FillWidth();
            if (ImGui::TreeNode(CONCAT_STR("##_sidekiq_nv_adv_", s->name))) {
                static const uint32_t nvRatesHz[] = {
                    250000, 541667, 740740, 750000, 1000000, 1920000, 2457600, 2500000, 2800000,
                    3840000, 4000000, 4915200, 5000000, 5600000, 7680000, 9830400, 10000000, 11200000,
                    15360000, 16000000, 20000000, 21666700, 22000000, 23040000, 30720000, 40000000, 61440000
                };
                std::string rateItems;
                int ri = 0, rsel = 0;
                for (auto hz : nvRatesHz) {
                    double msps = hz / 1e6;
                    char tmp[32]; std::snprintf(tmp, sizeof(tmp), "%.6g Msps", msps);
                    rateItems += tmp; rateItems.push_back('\0');
                    if ((int)hz == (int)s->nvSelectedRateHz) rsel = ri;
                    ++ri;
                }
                rateItems.push_back('\0');
                SmGui::LeftLabel("Sample rate");
                SmGui::FillWidth();
                if (SmGui::Combo(CONCAT_STR("##_sidekiq_nv_sr_", s->name), &rsel, rateItems.c_str())) {
                    s->nvSelectedRateHz = nvRatesHz[rsel];
                    s->saveNvPresetConfig();
                }

                static const int bwPercents[] = { 5, 10, 20, 40, 50, 60, 80, 86, 89, 95 };
                std::string bwItems;
                int bi = 0, bsel = 0;
                for (auto pct : bwPercents) {
                    uint32_t hz = (uint32_t)((double)s->nvSelectedRateHz * (pct/100.0));
                    char tmp[48]; std::snprintf(tmp, sizeof(tmp), "%d%%  (~%u Hz)", pct, hz);
                    bwItems += tmp; bwItems.push_back('\0');
                    if ((int)hz == (int)s->nvBwPresetHz) bsel = bi;
                    ++bi;
                }
                bwItems.push_back('\0');
                SmGui::LeftLabel("Bandwidth");
                SmGui::FillWidth();
                if (SmGui::Combo(CONCAT_STR("##_sidekiq_nv_bw_", s->name), &bsel, bwItems.c_str())) {
                    int pct = bwPercents[bsel];
                    s->nvBwPresetHz = (uint32_t)((double)s->nvSelectedRateHz * (pct/100.0));
                    s->saveNvPresetConfig();
                }
                ImGui::TreePop();
            }
            ImGui::Separator();
        };

        if (s->nvProfilesEnabled) {
            SmGui::LeftLabel("Use presets (NV100/NVM2)");
            SmGui::FillWidth();
            bool checkbox = s->useProfiles;
            if (SmGui::Checkbox(CONCAT_STR("##_sidekiq_useprof_", s->name), &checkbox)) {
                s->useProfiles = checkbox;
                s->saveNvPresetConfig();
            }
            if (s->useProfiles) drawNvPresets(); else drawManualSRBW();
        } else {
            drawManualSRBW();
        }

        // RF Port (LOCKED while running)
        if (!s->rfPorts.empty()) {
            SmGui::LeftLabel("RF Port");
            SmGui::FillWidth();
            int prevIdx = s->rfPortIdx;
            if (SmGui::Combo(CONCAT_STR("##_sidekiq_rfport_", s->name), &s->rfPortIdx, s->rfPorts.txt)) {
                if (prevIdx != s->rfPortIdx) s->saveRfPortChoice();
            }
        }
        if (s->running.load()) SmGui::EndDisabled();

        // ---- Live controls ----
        SmGui::LeftLabel("Gain mode");
        SmGui::FillWidth();
        {
            static const char items[] = "Manual\0Auto\0\0";
            int gm = (s->gainMode == skiq_rx_gain_manual) ? 0 : 1;
            if (SmGui::Combo(CONCAT_STR("##_sidekiq_gainmode_", s->name), &gm, items, 2)) {
                s->gainMode = (gm == 0) ? skiq_rx_gain_manual : skiq_rx_gain_auto;
                g_config.acquire();
                g_config.conf["gainMode"] = (int)gm;
                g_config.release(true);

                std::lock_guard<std::recursive_mutex> lk(s->sdkMtx);
                if (s->running.load()) {
                    (void)skiq_write_rx_gain_mode(s->activeCard, s->activeRxHdl, s->gainMode);
                    if (s->gainMode == skiq_rx_gain_manual) {
                        uint8_t gi8 = (uint8_t)std::clamp(s->gainIndex, (int)s->gainMin, (int)s->gainMax);
                        (void)skiq_write_rx_gain(s->activeCard, s->activeRxHdl, gi8);
                    }
                }
            }
        }

        SmGui::LeftLabel("Gain index");
        SmGui::FillWidth();
        {
            if (s->gainMode != skiq_rx_gain_manual) SmGui::BeginDisabled();
            int rangeMin = s->running.load() ? (int)s->gainMin : 0;
            int rangeMax = s->running.load() ? (int)s->gainMax : 76;
            if (rangeMax < rangeMin) { rangeMin = 0; rangeMax = 76; }
            int gi = std::clamp(s->gainIndex, rangeMin, rangeMax);
            if (SmGui::SliderInt(CONCAT_STR("##_sidekiq_gainidx_", s->name), &gi, rangeMin, rangeMax)) {
                s->gainIndex = gi;
                g_config.acquire();
                g_config.conf["gainIndex"] = (int)s->gainIndex;
                g_config.release(true);

                std::lock_guard<std::recursive_mutex> lk(s->sdkMtx);
                if (s->running.load() && s->gainMode == skiq_rx_gain_manual) {
                    uint8_t gi8 = (uint8_t)std::clamp(s->gainIndex, (int)s->gainMin, (int)s->gainMax);
                    (void)skiq_write_rx_gain(s->activeCard, s->activeRxHdl, gi8);
                }
            }
            if (s->gainMode != skiq_rx_gain_manual) SmGui::EndDisabled();
        }
    }

    // ===== helpers =====
    int enableCardWithRetry(uint8_t card, skiq_xport_init_level_t lvl, bool& weEnabled,
                            int tries = 10, int sleep_ms = 100) {
        weEnabled = false;
        uint8_t list[1] = { card };
        for (int i = 0; i < tries; ++i) {
            int rc = skiq_enable_cards(list, 1, lvl);
            if (rc == 0)      { weEnabled = true; return 0; }
            if (rc == -EBUSY) { weEnabled = false; return 0; }
            if (rc == -EINVAL) {
                std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        }
        return -EINVAL;
    }

    static bool isNvFamily(const skiq_param_t& p) {
        auto pt = p.card_param.part_type;
        return (pt == skiq_nv100 || pt == skiq_nvm2);
    }

    void detectNvSupport() {
        std::lock_guard<std::recursive_mutex> lk(sdkMtx);
        nvProfilesEnabled = false;
        if (devices.empty()) return;
        uint8_t card = devices.value(deviceIdx);
        if (!cardIsEnabled && !running.load()) return; // only detect when enabled

        skiq_param_t param{};
        if (skiq_read_parameters(card, &param) == 0) {
            if (isNvFamily(param)) nvProfilesEnabled = true;
        }
    }

    void enumerateHandlesForCard(uint8_t card) {
        if (devices.empty()) return;

        if (!cardIsEnabled && !running.load()) {
            handles.clear();
            addHandleUnique("A1", skiq_rx_hdl_A1);
            addHandleUnique("A2", skiq_rx_hdl_A2);
            addHandleUnique("B1", skiq_rx_hdl_B1);
            addHandleUnique("B2", skiq_rx_hdl_B2);
            handleIdx = std::min(handleIdx, std::max(0, handles.size()-1));
            enumerateRfPorts(card, handles.value(handleIdx));
            return;
        }

        std::lock_guard<std::recursive_mutex> lk(sdkMtx);

        skiq_rx_hdl_t wanted = (!handles.empty() && handleIdx >= 0 && handleIdx < handles.size())
                               ? handles.value(handleIdx)
                               : skiq_rx_hdl_A1;

        handles.clear();

        skiq_param_t param{};
        if (skiq_read_parameters(card, &param) != 0) {
            addHandleUnique("A1", skiq_rx_hdl_A1);
            addHandleUnique("A2", skiq_rx_hdl_A2);
            addHandleUnique("B1", skiq_rx_hdl_B1);
            addHandleUnique("B2", skiq_rx_hdl_B2);
        } else {
            std::unordered_set<std::string> seen;
            uint8_t n = param.rf_param.num_rx_channels;
            for (uint8_t i = 0; i < n; ++i) {
                skiq_rx_hdl_t h = param.rf_param.rx_handles[i];
                const char* label = nullptr;
                switch (h) {
                    case skiq_rx_hdl_A1: label = "A1"; break;
                    case skiq_rx_hdl_A2: label = "A2"; break;
                    case skiq_rx_hdl_B1: label = "B1"; break;
                    case skiq_rx_hdl_B2: label = "B2"; break;
                    case skiq_rx_hdl_C1: label = "C1"; break;
                    case skiq_rx_hdl_D1: label = "D1"; break;
                    default: break;
                }
                if (label && seen.insert(label).second) handles.define(label, label, h);
            }
        }
        handleIdx = 0;
        for (int i = 0; i < handles.size(); ++i) {
            if (handles.value(i) == wanted) { handleIdx = i; break; }
        }

        if (!handles.empty()) enumerateRfPorts(card, handles.value(handleIdx));
    }

    void addHandleUnique(const char* key, skiq_rx_hdl_t h) {
        if (!handles.keyExists(key)) handles.define(key, key, h);
    }

    void enumerateRfPorts(uint8_t card, skiq_rx_hdl_t hdl) {
        if (devices.empty()) return;

        if (!cardIsEnabled && !running.load()) {
            rfPorts.clear();
            addRfPortUnique(skiq_rf_port_J1);
            addRfPortUnique(skiq_rf_port_J2);
            rfPortIdx = std::min(rfPortIdx, std::max(0, rfPorts.size()-1));
            return;
        }

        std::lock_guard<std::recursive_mutex> lk(sdkMtx);

        std::string devKey = devices.key(deviceIdx);
        std::string prevPortName;
        auto it = rfPortByDevice.find(devKey);
        if (it != rfPortByDevice.end() && it->is_string()) prevPortName = it->get<std::string>();

        rfPorts.clear();

        uint8_t num_fixed = 0, num_trx = 0;
        skiq_rf_port_t fixed_list[8]{}, trx_list[8]{};

        int32_t st = skiq_read_rx_rf_ports_avail_for_hdl(card, hdl,
                                                         &num_fixed, fixed_list,
                                                         &num_trx, trx_list);
        if (st != 0) {
            addRfPortUnique(skiq_rf_port_J1);
            addRfPortUnique(skiq_rf_port_J2);
        } else {
            std::unordered_set<int> seen;
            auto addPort = [&](skiq_rf_port_t p){
                if (seen.insert((int)p).second) {
                    std::string key = rfPortName(p);
                    if (!rfPorts.keyExists(key)) rfPorts.define(key, key, p);
                }
            };
            for (uint8_t i = 0; i < num_fixed; ++i) addPort(fixed_list[i]);
            for (uint8_t i = 0; i < num_trx;   ++i) addPort(trx_list[i]);
        }

        rfPortIdx = 0;
        if (!prevPortName.empty() && rfPorts.keyExists(prevPortName)) {
            rfPortIdx = rfPorts.keyId(prevPortName);
        }
    }

    void addRfPortUnique(skiq_rf_port_t p) {
        std::string key = rfPortName(p);
        if (!rfPorts.keyExists(key)) rfPorts.define(key, key, p);
    }

    std::string rfPortName(skiq_rf_port_t p) {
        switch (p) {
            case skiq_rf_port_J1: return "J1";
            case skiq_rf_port_J2: return "J2";
            case skiq_rf_port_J3: return "J3";
            case skiq_rf_port_J4: return "J4";
            case skiq_rf_port_J5: return "J5";
            case skiq_rf_port_J6: return "J6";
            default: return "RF";
        }
    }

    void startStream() {
        std::lock_guard<std::recursive_mutex> lk(sdkMtx);
        if (running.load()) return;

        if (!libInited.load()) {
            int32_t st = skiq_init_without_cards();
            if (st != 0) { flog::error("Sidekiq: init failed ({})", (int)st); return; }
            libInited.store(true);
        }

        if (devices.empty()) {
            refreshDevices();
            if (devices.empty()) { flog::error("Sidekiq: no cards present"); return; }
        }

        activeCard = devices.value(deviceIdx);

        // Confirm selected card still present
        {
            uint8_t cards[SKIQ_MAX_NUM_CARDS] = {0};
            uint8_t num = 0;
            (void)skiq_get_cards(skiq_xport_type_auto, &num, cards);
            bool present = false;
            for (uint8_t i = 0; i < num; ++i) if (cards[i] == activeCard) { present = true; break; }
            if (!present) {
                flog::error("Sidekiq: selected card-{} not present; please Refresh/Re-scan", (unsigned)activeCard);
                return;
            }
        }

        // Hot-plug settle
        auto now = std::chrono::steady_clock::now();
        auto itFS = firstSeen.find(activeCard);
        if (itFS != firstSeen.end()) {
            constexpr auto kSettle = std::chrono::milliseconds(2500);
            auto age = now - itFS->second;
            if (age < kSettle) {
                auto remain = kSettle - age;
                flog::info("Sidekiq: waiting {} ms for card-{} to settle before FULL enable",
                           (int)std::chrono::duration_cast<std::chrono::milliseconds>(remain).count(),
                           (unsigned)activeCard);
                std::this_thread::sleep_for(remain);
            }
        }

        { uint8_t listTmp[1] = { activeCard }; (void)skiq_disable_cards(listTmp, 1); }

        bool weEnabledFull = false;
        int32_t st = enableCardWithRetry(activeCard, skiq_xport_init_level_full, weEnabledFull, 40, 125);
        if (st != 0) {
            flog::error("Sidekiq: enable_cards(FULL) failed after retries");
            return;
        }
        cardIsEnabled = weEnabledFull ? true : false;

        // Serial for UI
        activeSerial.clear();
        {
            char* serial = nullptr;
            if (skiq_read_serial_string(activeCard, &serial) == 0 && serial && serial[0]) {
                activeSerial = serial;
                knownSerialByCard[activeCard] = activeSerial;
                refreshDevices(/*allow_short_retry=*/false);
            }
        }

        // Handles
        enumerateHandlesForCard(activeCard);
        if (handles.empty()) {
            flog::error("Sidekiq: no valid RX handles found on card {}", (unsigned)activeCard);
            if (cardIsEnabled) { uint8_t list[1] = { activeCard }; (void)skiq_disable_cards(list, 1); cardIsEnabled = false; }
            return;
        }
        handleIdx = std::min(handleIdx, std::max(0, handles.size()-1));
        activeRxHdl = handles.value(handleIdx);

        // RF ports
        enumerateRfPorts(activeCard, activeRxHdl);
        if (!rfPorts.empty()) {
            rfPortIdx = std::min(rfPortIdx, std::max(0, rfPorts.size()-1));
            skiq_rf_port_t chosen = rfPorts.value(rfPortIdx);
            if (nvProfilesEnabled && chosen == skiq_rf_port_J2) {
                (void)skiq_write_rf_port_config(activeCard, skiq_rf_port_config_trx);
            }
            (void)skiq_write_rx_rf_port_for_hdl(activeCard, activeRxHdl, chosen);
            skiq_rf_port_t actual;
            if (skiq_read_rx_rf_port_for_hdl(activeCard, activeRxHdl, &actual) == 0) {
                if (actual != chosen) {
                    for (int i = 0; i < rfPorts.size(); ++i)
                        if (rfPorts.value(i) == actual) { rfPortIdx = i; saveRfPortChoice(); break; }
                    flog::warn("Sidekiq: RF port adjusted by device (requested {}, using {})",
                               (int)chosen, (int)actual);
                }
            }
        } else {
            flog::warn("Sidekiq: no RF ports available after enable; skipping port write");
        }

        // Gain range
        {
            uint8_t gmin = 0, gmax = 0;
            int32_t grc = skiq_read_rx_gain_index_range(activeCard, activeRxHdl, &gmin, &gmax);
            if (grc == 0 && gmax >= gmin) { gainMin = gmin; gainMax = gmax; }
            else { gainMin = 0; gainMax = 76; }
            if (gainIndex < (int)gainMin || gainIndex > (int)gainMax) {
                gainIndex = (int)gainMin + (int)((((int)gainMax - (int)gainMin) / 2));
            }
        }

        // IQ order
        (void)skiq_write_iq_order_mode(activeCard, skiq_iq_order_qi);

        // SR/BW
        if (useProfiles && nvProfilesEnabled) {
            uint32_t sr = (uint32_t)nvSelectedRateHz;
            uint32_t bw = (uint32_t)nvBwPresetHz;
            if (skiq_write_rx_sample_rate_and_bandwidth(activeCard, activeRxHdl, sr, bw) == 0) {
                sampleRate = (int)sr;
                bandwidth  = (int)bw;
                core::setInputSampleRate(sampleRate);
            }
        } else {
            (void)skiq_write_rx_sample_rate_and_bandwidth(activeCard, activeRxHdl,
                                                          (uint32_t)sampleRate, (uint32_t)bandwidth);
        }

        (void)skiq_write_rx_LO_freq(activeCard, activeRxHdl, centerFreq);
        (void)skiq_write_rx_gain_mode(activeCard, activeRxHdl, gainMode);
        if (gainMode == skiq_rx_gain_manual) {
            uint8_t gi8 = (uint8_t)std::clamp(gainIndex, (int)gainMin, (int)gainMax);
            (void)skiq_write_rx_gain(activeCard, activeRxHdl, gi8);
        }

        (void)skiq_write_rx_stream_mode(activeCard, streamMode);
        (void)skiq_set_rx_transfer_timeout(activeCard, 50);

        int32_t sst = skiq_start_rx_streaming(activeCard, activeRxHdl);
        if (sst != 0) {
            flog::error("Sidekiq: start_rx_streaming failed ({})", (int)sst);
            if (cardIsEnabled) { uint8_t list[1] = { activeCard }; (void)skiq_disable_cards(list, 1); cardIsEnabled = false; }
            return;
        }

        running.store(true);
        rxStreamingStarted = true;
        rxThread = std::thread(&SidekiqSourceModule::rxLoop, this);
        flog::info("SidekiqSource {}: Start", name);
    }

    void stopStream() {
        // Note time first so Refresh knows to cool down
        {
            std::lock_guard<std::recursive_mutex> lk(sdkMtx);
            lastStopOrUnplug_ = std::chrono::steady_clock::now();
        }

        if (!running.load()) {
            // Still exit SDK so any live mapping is dropped
            safeExitSdk();
            return;
        }

        running.store(false);
        if (rxThread.joinable()) rxThread.join();

        std::lock_guard<std::recursive_mutex> lk(sdkMtx);

        if (rxStreamingStarted) {
            (void)skiq_stop_rx_streaming(activeCard, activeRxHdl);
            rxStreamingStarted = false;
        }
        if (cardIsEnabled) {
            uint8_t list[1] = { activeCard };
            (void)skiq_disable_cards(list, 1);
            cardIsEnabled = false;
        }

        stream.clearWriteStop();
        flog::info("SidekiqSource {}: Stop", name);

        // reset state & fully exit SDK so next card starts from a blank slate
        activeCard   = 0xFF;
        activeRxHdl  = skiq_rx_hdl_A1;
        hadAnyDevice = false;

        safeExitSdk();
    }

    void rxLoop() {
        int32_t blk_bytes;
        {
            std::lock_guard<std::recursive_mutex> lk(sdkMtx);
            blk_bytes = skiq_read_rx_block_size(activeCard, streamMode);
        }
        if (blk_bytes <= 0) blk_bytes = 4096;

        const double scale = 1.0 / 2048.0; // 12-bit -> float
        std::vector<dsp::complex_t> out;

        while (running.load()) {
            skiq_rx_block_t* pblk = nullptr;
            skiq_rx_hdl_t    hdl  = activeRxHdl;
            uint32_t         len  = 0;

            skiq_rx_status_t st;
            {
                std::lock_guard<std::recursive_mutex> lk(sdkMtx);
                st = skiq_receive(activeCard, &hdl, &pblk, &len);
            }

            if (st == skiq_rx_status_success && pblk && len >= SKIQ_RX_HEADER_SIZE_IN_BYTES) {
                const uint32_t payload_bytes = len - SKIQ_RX_HEADER_SIZE_IN_BYTES;
                const uint32_t iq_pairs = payload_bytes / 4;
                out.resize(iq_pairs);

                volatile int16_t* data = pblk->data; // (Q,I) order
                for (uint32_t i = 0; i < iq_pairs; ++i) {
                    const int16_t qv = data[2*i + 0];
                    const int16_t iv = data[2*i + 1];
                    out[i].re = iv * scale;
                    out[i].im = qv * scale;
                }

                std::memcpy(stream.writeBuf, out.data(), iq_pairs * sizeof(dsp::complex_t));
                if (!stream.swap(iq_pairs)) break;
            } else if (st == skiq_rx_status_no_data) {
                continue;
            } else {
                flog::warn("Sidekiq: skiq_receive status={}", (int)st);
            }
        }
    }

    void safeExitSdk() {
        std::lock_guard<std::recursive_mutex> lk(sdkMtx);
        if (libInited.load()) {
            skiq_exit();
            libInited.store(false);
        }
    }

    static std::vector<uint8_t> toVec(const uint8_t* cards, uint8_t n) {
        return std::vector<uint8_t>(cards, cards + n);
    }

private:
    struct Profile { std::string name; uint32_t sample_rate; uint32_t bandwidth; };

    std::string name;
    SourceManager::SourceHandler handler;
    dsp::stream<dsp::complex_t> stream;

    // UI selections
    int deviceIdx = 0;
    int handleIdx = 0;
    int rfPortIdx = 0;
    int streamModeIdx = 0;

    OptionList<std::string, uint8_t>                 devices;
    OptionList<std::string, skiq_rx_hdl_t>           handles;
    OptionList<std::string, skiq_rf_port_t>          rfPorts;
    OptionList<std::string, skiq_rx_stream_mode_t>   modes;

    // Active
    uint8_t        activeCard = 0;
    skiq_rx_hdl_t  activeRxHdl = skiq_rx_hdl_A1;
    bool           cardIsEnabled = false;
    bool           rxStreamingStarted = false;

    // Settings
    int                    sampleRate;
    int                    bandwidth;
    uint64_t               centerFreq;
    skiq_rx_gain_t         gainMode;
    int                    gainIndex;
    skiq_rx_stream_mode_t  streamMode;
    uint8_t                gainMin;
    uint8_t                gainMax;

    // NV presets
    bool        useProfiles;
    bool        nvProfilesEnabled;
    uint32_t    nvSelectedRateHz;
    uint32_t    nvBwPresetHz;

    json rfPortByDevice;

    std::map<uint8_t, std::chrono::steady_clock::time_point> firstSeen;

    std::string activeSerial;
    std::map<uint8_t, std::string> knownSerialByCard;

    std::thread       rxThread;
    std::atomic<bool> running{false};
    std::atomic<bool> libInited{false};
    bool enabled = true;

    // SDK access serialization + teardown cooldown + last-topology
    std::recursive_mutex sdkMtx;
    bool hadAnyDevice = false;
    std::chrono::steady_clock::time_point lastStopOrUnplug_{};
    std::vector<uint8_t> lastCardSet_;

    void saveNvPresetConfig() {
        g_config.acquire();
        g_config.conf["useProfiles"]      = useProfiles;
        g_config.conf["nvSelectedRateHz"] = (int)nvSelectedRateHz;
        g_config.conf["nvBwPresetHz"]     = (int)nvBwPresetHz;
        g_config.release(true);
    }

    void saveRfPortChoice() {
        if (devices.empty() || rfPorts.empty()) return;
        std::string devKey = devices.key(deviceIdx);
        std::string portKey = rfPorts.key(rfPortIdx);
        rfPortByDevice[devKey] = portKey;
        g_config.acquire();
        g_config.conf["rfPortByDevice"] = rfPortByDevice;
        g_config.release(true);
    }
};

// ===== module glue =====
MOD_EXPORT void _INIT_() {
    json def = json({});
    def["device"]           = "";
    def["streamMode"]       = (int)skiq_rx_stream_mode_high_tput;
    def["sampleRate"]       = 10000000;
    def["bandwidth"]        = 10000000;
    def["gainMode"]         = 0;   // 0=manual, 1=auto
    def["gainIndex"]        = 30;
    def["useProfiles"]      = false;
    def["nvSelectedRateHz"] = 10000000;
    def["nvBwPresetHz"]     = 8000000; // 80%
    def["rfPortByDevice"]   = json::object();

    g_config.setPath(core::args["root"].s() + "/sidekiq_config.json");
    g_config.load(def);
    g_config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new SidekiqSourceModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (SidekiqSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    g_config.disableAutoSave();
    g_config.save();
}
