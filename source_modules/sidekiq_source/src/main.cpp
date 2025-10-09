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

#define CONCAT_STR(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "sidekiq_source",
    /* Description:     */ "Epiq Sidekiq source module for SDR++",
    /* Author:          */ "Aaron",
    /* Version:         */ 0, 1, 7,
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
        // Valid RX gain index range for the active handle (queried at startStream)
        gainMin    = 0;
        gainMax    = 76;

        // Profiles / presets
        useProfiles        = false;      // UI toggle
        nvProfilesEnabled  = false;      // becomes true for NV100/NVM2
        nvSelectedRateHz   = 10'000'000; // default SR when NV presets are active
        nvBwPercent        = 50;         // default BW% when NV presets are active

        // Stream modes in UI
        modes.define("High Throughput", "High Throughput", skiq_rx_stream_mode_high_tput);
        modes.define("Low Latency",     "Low Latency",     skiq_rx_stream_mode_low_latency);
        modes.define("Balanced",        "Balanced",        skiq_rx_stream_mode_balanced);

        // Probe at startup (SDR++-like behavior), but with strong guards.
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
        if (g_config.conf.contains("sampleRate")) sampleRate = (int)g_config.conf["sampleRate"];
        if (g_config.conf.contains("bandwidth"))  bandwidth  = (int)g_config.conf["bandwidth"];
        if (g_config.conf.contains("gainMode"))   gainMode   = (skiq_rx_gain_t)((int)g_config.conf["gainMode"]);
        if (g_config.conf.contains("gainIndex"))  gainIndex  = (int)g_config.conf["gainIndex"];
        if (g_config.conf.contains("useProfiles"))      useProfiles       = (bool)g_config.conf["useProfiles"];
        if (g_config.conf.contains("nvSelectedRateHz")) nvSelectedRateHz  = (int)g_config.conf["nvSelectedRateHz"];
        if (g_config.conf.contains("nvBwPercent"))      nvBwPercent       = (int)g_config.conf["nvBwPercent"];
        // Persisted RF port (keyed by device key string)
        if (g_config.conf.contains("rfPortByDevice"))   rfPortByDevice = g_config.conf["rfPortByDevice"];
        g_config.release();

        core::setInputSampleRate(sampleRate);
        sigpath::sourceManager.registerSource("Sidekiq", &handler);
    }

    ~SidekiqSourceModule() {
        stopStream();
        sigpath::sourceManager.unregisterSource("Sidekiq");
        if (libInited.load()) { skiq_exit(); libInited.store(false); }
    }

    void postInit() {}

    void enable()  { enabled = true;  }
    void disable() { enabled = false; }
    bool isEnabled() { return enabled; }

    // Enumerate cards (prefer PCIe), but never crash if absent/unsupported.
    void refreshDevices() {
        // Lazy-init the library once for this process.
        if (!libInited.load()) {
            int32_t st_init = skiq_init_without_cards();
            if (st_init != 0) {
                flog::error("Sidekiq: skiq_init_without_cards failed ({})", (int)st_init);
                return;
            }
            libInited.store(true);
        }

        uint8_t cards[SKIQ_MAX_NUM_CARDS] = {0};
        uint8_t num = 0;

        auto try_get = [&](skiq_xport_type_t x) -> int32_t {
            std::memset(cards, 0, sizeof(cards));
            num = 0;
            int32_t rc = skiq_get_cards(x, &num, cards);
            flog::info("Sidekiq: get_cards(xport={}, rc={}, num={})", (int)x, (int)rc, (int)num);
            return rc;
        };

        // Prefer PCIe; if it errors OR finds zero, try AUTO.
        int32_t st = try_get(skiq_xport_type_pcie);
        if (st != 0 || num == 0) st = try_get(skiq_xport_type_auto);

        devices.clear();

        if (st != 0 || num == 0) {
            flog::warn("Sidekiq: no cards detected on PCIe/Auto (st={}, num={})", (int)st, (int)num);
            deviceIdx     = 0;
            handleIdx     = 0;
            rfPortIdx     = 0;
            streamModeIdx = std::min(streamModeIdx, std::max(0, modes.size()-1));
            return;
        }

        // Try BASIC to read serials, but ALWAYS list devices even if BASIC fails/EBUSY.
        bool basicEnabled = false;
        {
            int32_t st_en = skiq_enable_cards(cards, num, skiq_xport_init_level_basic);
            if (st_en == 0) {
                basicEnabled = true;
            } else {
                flog::warn("Sidekiq: enable_cards(BASIC) failed ({}); proceeding without serials", (int)st_en);
            }
        }

        std::unordered_set<std::string> seenKeys;
        for (uint8_t i = 0; i < num; ++i) {
            std::string key;
            if (basicEnabled) {
                char* serial = nullptr;
                if (skiq_read_serial_string(cards[i], &serial) == 0 && serial && serial[0]) {
                    key = serial;
                }
            }
            if (key.empty()) {
                char tmp[32]; std::snprintf(tmp, sizeof(tmp), "card-%u", cards[i]);
                key = tmp;
            }
            if (seenKeys.insert(key).second) {
                devices.define(key, key, cards[i]);
            }
        }

        if (basicEnabled) {
            (void)skiq_disable_cards(cards, num);
        }

        deviceIdx     = std::min(deviceIdx,     std::max(0, devices.size()-1));
        handleIdx     = std::min(handleIdx,     std::max(0, handles.size()-1));
        rfPortIdx     = std::min(rfPortIdx,     std::max(0, rfPorts.size()-1));
        streamModeIdx = std::min(streamModeIdx, std::max(0, modes.size()-1));
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
        if (s->running.load()) {
            (void)skiq_write_rx_LO_freq(s->activeCard, s->activeRxHdl, s->centerFreq);
        }
        flog::info("SidekiqSource {}: Tune {} Hz", s->name, (uint64_t)freq);
    }

    // ===== GUI =====
    static void onMenu(void* ctx) {
        auto* s = static_cast<SidekiqSourceModule*>(ctx);

        // Refresh devices (always enabled)
        SmGui::FillWidth();
        if (SmGui::Button(CONCAT_STR("Refresh##_sidekiq_refr_", s->name))) {
            s->refreshDevices();
        }

        // Device dropdown (LOCKED while running)
        if (s->running.load()) SmGui::BeginDisabled();
        SmGui::FillWidth();
        SmGui::ForceSync();

        if (s->devices.empty()) {
            SmGui::Text("No Sidekiq devices found. Plug in and click Refresh.");
        } else {
            if (SmGui::Combo(CONCAT_STR("Device##_sidekiq_dev_", s->name), &s->deviceIdx, s->devices.txt)) {
                // Device changed -> clear/refresh dependent lists
                g_config.acquire();
                g_config.conf["device"] = s->devices.key(s->deviceIdx);
                g_config.release(true);
                s->enumerateHandlesForCard(s->devices.value(s->deviceIdx));
            }
        }

        // RX handle (LOCKED while running)
        SmGui::FillWidth();
        if (!s->handles.empty()) {
            int prevHandleIdx = s->handleIdx;
            if (SmGui::Combo(CONCAT_STR("RX Handle##_sidekiq_hdl_", s->name), &s->handleIdx, s->handles.txt)) {
                if (prevHandleIdx != s->handleIdx) {
                    // Re-enumerate RF ports for new handle, preserving selection
                    s->enumerateRfPorts(s->devices.value(s->deviceIdx), s->handles.value(s->handleIdx));
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

        // ---- NV100/NVM2 presets (no reprogramming) ----
        s->detectNvSupport(); // set nvProfilesEnabled based on current device

        SmGui::LeftLabel("Use presets (NV100/NVM2)");
        SmGui::FillWidth();
        {
            bool up = s->useProfiles && s->nvProfilesEnabled;
            bool checkbox = up;
            if (SmGui::Checkbox(CONCAT_STR("##_sidekiq_useprof_", s->name), &checkbox)) {
                // Only allow enabling if device supports NV presets
                s->useProfiles = checkbox && s->nvProfilesEnabled;
                s->saveNvPresetConfig();
            }
            if (!s->nvProfilesEnabled) {
                SmGui::SameLine();
                SmGui::TextDisabled("(not supported for this device)");
            }
        }

        if (s->useProfiles && s->nvProfilesEnabled) {
            // Supported NV100/NVM2 sample rates (from SDK manual)
            static const uint32_t nvRatesHz[] = {
                250000, 541667, 740740, 750000, 1000000, 1920000, 2457600, 2500000, 2800000,
                3840000, 4000000, 4915200, 5000000, 5600000, 7680000, 9830400, 10000000, 11200000,
                15360000, 16000000, 20000000, 21666700, 22000000, 23040000, 30720000, 40000000, 61440000
            };
            // Build items string (Msps text)
            std::string rateItems;
            int rateIdx = 0, curIdx = 0;
            for (auto hz : nvRatesHz) {
                double msps = hz / 1e6;
                char tmp[32]; std::snprintf(tmp, sizeof(tmp), "%.6g Msps", msps);
                rateItems += tmp; rateItems.push_back('\0');
                if ((int)hz == (int)s->nvSelectedRateHz) curIdx = rateIdx;
                ++rateIdx;
            }
            rateItems.push_back('\0');
            SmGui::LeftLabel("Sample rate");
            SmGui::FillWidth();
            if (SmGui::Combo(CONCAT_STR("##_sidekiq_nv_sr_", s->name), &curIdx, rateItems.c_str())) {
                // map back to Hz
                s->nvSelectedRateHz = nvRatesHz[std::min<int>(curIdx, (int)(sizeof(nvRatesHz)/sizeof(nvRatesHz[0])-1))];
                s->saveNvPresetConfig();
            }

            // Bandwidth as % of sample rate (NV supports 5–80% in 0.5% steps; plus a few >80) — we’ll clamp to 3–99 to be safe
            SmGui::LeftLabel("Rx bandwidth (% of SR)");
            SmGui::FillWidth();
            int bwp = std::clamp((int)s->nvBwPercent, 3, 99);
            if (SmGui::SliderInt(CONCAT_STR("##_sidekiq_nv_bwpc_", s->name), &bwp, 3, 99)) {
                s->nvBwPercent = bwp;
                s->saveNvPresetConfig();
            }

            ImGui::Separator();
        }

        // Sample rate (hidden while NV presets are used)
        SmGui::LeftLabel("Sample rate (Hz)");
        SmGui::FillWidth();
        if (!(s->useProfiles && s->nvProfilesEnabled)) {
            int sr_tmp = (int)s->sampleRate;
            if (SmGui::InputInt(CONCAT_STR("##_sidekiq_sr_", s->name), &sr_tmp)) {
                s->sampleRate = std::max(100000, sr_tmp);
                core::setInputSampleRate(s->sampleRate);
                g_config.acquire();
                g_config.conf["sampleRate"] = (int)s->sampleRate;
                g_config.release(true);
            }
        }

        // Bandwidth (hidden while NV presets are used)
        SmGui::LeftLabel("Chan bandwidth (Hz)");
        SmGui::FillWidth();
        if (!(s->useProfiles && s->nvProfilesEnabled)) {
            int bw_tmp = (int)s->bandwidth;
            if (SmGui::InputInt(CONCAT_STR("##_sidekiq_bw_", s->name), &bw_tmp)) {
                s->bandwidth = std::max(100000, bw_tmp);
                g_config.acquire();
                g_config.conf["bandwidth"] = (int)s->bandwidth;
                g_config.release(true);
            }
        }

        // RF Port (LOCKED while running) — persist selection
        if (!s->rfPorts.empty()) {
            SmGui::LeftLabel("RF Port");
            SmGui::FillWidth();
            int prevIdx = s->rfPortIdx;
            if (SmGui::Combo(CONCAT_STR("##_sidekiq_rfport_", s->name), &s->rfPortIdx, s->rfPorts.txt)) {
                if (prevIdx != s->rfPortIdx) {
                    s->saveRfPortChoice();
                }
            }
        }
        if (s->running.load()) SmGui::EndDisabled();

        // ---- LIVE controls (enabled during run) ----

        // Gain mode (LIVE)
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
                if (s->running.load()) {
                    (void)skiq_write_rx_gain_mode(s->activeCard, s->activeRxHdl, s->gainMode);
                    if (s->gainMode == skiq_rx_gain_manual) {
                        uint8_t gi8 = (uint8_t)std::clamp(s->gainIndex, (int)s->gainMin, (int)s->gainMax);
                        int32_t rc = skiq_write_rx_gain(s->activeCard, s->activeRxHdl, gi8);
                        if (rc != 0) {
                            flog::error("Sidekiq: failed to write Rx gain (index={}, status={})",
                                        (unsigned)gi8, (int)rc);
                        }
                    }
                }
            }
        }

        // Gain index (LIVE when Manual)
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
                if (s->running.load() && s->gainMode == skiq_rx_gain_manual) {
                    uint8_t gi8 = (uint8_t)std::clamp(s->gainIndex, (int)s->gainMin, (int)s->gainMax);
                    int32_t rc = skiq_write_rx_gain(s->activeCard, s->activeRxHdl, gi8);
                    if (rc != 0) {
                        flog::error("Sidekiq: failed to write Rx gain (index={}, status={})",
                                    (unsigned)gi8, (int)rc);
                    } else {
                        flog::info("Sidekiq: Set Rx gain index to {} (range {}..{})",
                                   (unsigned)gi8, (unsigned)s->gainMin, (unsigned)s->gainMax);
                    }
                }
            }
            if (s->gainMode != skiq_rx_gain_manual) SmGui::EndDisabled();
        }
    }

    // ===== helpers =====
    void enumerateHandlesForCard(uint8_t card) {
        // preserve current selection when possible
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
        // restore selection if still present
        handleIdx = 0;
        for (int i = 0; i < handles.size(); ++i) {
            if (handles.value(i) == wanted) { handleIdx = i; break; }
        }

        // Also enumerate RF ports for the selected handle
        if (!handles.empty()) enumerateRfPorts(card, handles.value(handleIdx));
    }

    void addHandleUnique(const char* key, skiq_rx_hdl_t h) {
        if (!handles.keyExists(key)) handles.define(key, key, h);
    }

    void enumerateRfPorts(uint8_t card, skiq_rx_hdl_t hdl) {
        // preserve previous logical port selection by name for this device
        std::string devKey = devices.key(deviceIdx);
        std::string prevPortName;
        auto it = rfPortByDevice.find(devKey);
        if (it != rfPortByDevice.end()) prevPortName = (std::string)it->second;

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

        // Try to restore RF port by saved name; otherwise keep index 0
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

        // Enable card FULL
        uint8_t list[1] = { activeCard };
        int32_t st = skiq_enable_cards(list, 1, skiq_xport_init_level_full);
        if (st == 0) {
            cardIsEnabled = true;
        } else if (st == -EBUSY) {
            // Already enabled elsewhere; usable, but we won't disable on stop.
            cardIsEnabled = false;
        } else {
            flog::error("Sidekiq: enable_cards failed ({})", (int)st);
            return;
        }

        // Re-enumerate handles (post-FULL init)
        enumerateHandlesForCard(activeCard);
        if (handles.empty()) {
            flog::error("Sidekiq: no valid RX handles found on card {}", (unsigned)activeCard);
            if (cardIsEnabled) {
                (void)skiq_disable_cards(list, 1);
                cardIsEnabled = false;
            }
            return;
        }
        handleIdx = std::min(handleIdx, std::max(0, handles.size()-1));
        activeRxHdl = handles.value(handleIdx);

        // RF ports (preserve saved choice)
        enumerateRfPorts(activeCard, activeRxHdl);
        if (!rfPorts.empty()) {
            rfPortIdx = std::min(rfPortIdx, std::max(0, rfPorts.size()-1));
            skiq_rf_port_t p = rfPorts.value(rfPortIdx);
            (void)skiq_write_rx_rf_port_for_hdl(activeCard, activeRxHdl, p);
        }

        // Discover and cache the valid RX gain index range for this card/handle
        {
            uint8_t gmin = 0, gmax = 0;
            int32_t grc = skiq_read_rx_gain_index_range(activeCard, activeRxHdl, &gmin, &gmax);
            if (grc == 0 && gmax >= gmin) {
                gainMin = gmin;
                gainMax = gmax;
                flog::info("Sidekiq: RX gain index range for handle is {}..{}", (unsigned)gainMin, (unsigned)gainMax);
            } else {
                gainMin = 0;
                gainMax = 76;
                flog::warn("Sidekiq: read_rx_gain_index_range failed ({}), using fallback {}..{}", (int)grc, (unsigned)gainMin, (unsigned)gainMax);
            }
            if (gainIndex < (int)gainMin || gainIndex > (int)gainMax) {
                gainIndex = (int)gainMin + (int)((((int)gainMax - (int)gainMin) / 2));
                flog::info("Sidekiq: Adjusted initial Rx gain index to midpoint {} within {}..{}", (unsigned)gainIndex, (unsigned)gainMin, (unsigned)gainMax);
            }
        }

        // IQ order and RF setup
        (void)skiq_write_iq_order_mode(activeCard, skiq_iq_order_qi);

        // Apply SR/BW either from NV presets or from manual fields
        detectNvSupport();
        if (useProfiles && nvProfilesEnabled) {
            // NV100/NVM2: bandwidth is % of SR
            uint32_t sr = (uint32_t)nvSelectedRateHz;
            // Clamp BW% into supported-ish range; the firmware will quantize as needed
            double bw = (double)sr * (std::clamp(nvBwPercent, 3, 99) / 100.0);
            int32_t rc = skiq_write_rx_sample_rate_and_bandwidth(activeCard, activeRxHdl, sr, (uint32_t)bw);
            if (rc != 0) {
                flog::warn("Sidekiq: NV preset SR/BW write failed rc={}, falling back to previous SR/BW", (int)rc);
            } else {
                sampleRate = (int)sr;
                bandwidth  = (int)bw;
                core::setInputSampleRate(sampleRate);
                flog::info("Sidekiq: using NV preset (SR={} Hz, BW≈{} Hz)", sr, (uint32_t)bw);
            }
        } else {
            int32_t rc_srbw =
                skiq_write_rx_sample_rate_and_bandwidth(activeCard, activeRxHdl,
                                                        (uint32_t)sampleRate, (uint32_t)bandwidth);
            if (rc_srbw != 0) {
                flog::warn("Sidekiq: write_rx_sample_rate_and_bandwidth({}, {}) failed rc={} "
                           "(device may require a fixed profile; leaving existing SR/BW)",
                           (uint32_t)sampleRate, (uint32_t)bandwidth, (int)rc_srbw);
            }
        }

        (void)skiq_write_rx_LO_freq(activeCard, activeRxHdl, centerFreq);
        (void)skiq_write_rx_gain_mode(activeCard, activeRxHdl, gainMode);
        if (gainMode == skiq_rx_gain_manual) {
            uint8_t gi8 = (uint8_t)std::clamp(gainIndex, (int)gainMin, (int)gainMax);
            int32_t rc = skiq_write_rx_gain(activeCard, activeRxHdl, gi8);
            if (rc != 0) {
                flog::error("Sidekiq: failed to write initial Rx gain (index={}, status={})", (unsigned)gi8, (int)rc);
            } else {
                flog::info("Sidekiq: Initial Rx gain index set to {} (range {}..{})", (unsigned)gi8, (unsigned)gainMin, (unsigned)gainMax);
            }
        }

        // Stream mode is card-scope
        (void)skiq_write_rx_stream_mode(activeCard, streamMode);
        (void)skiq_set_rx_transfer_timeout(activeCard, 50); // ms

        int32_t sst = skiq_start_rx_streaming(activeCard, activeRxHdl);
        if (sst != 0) {
            flog::error("Sidekiq: start_rx_streaming failed ({})", (int)sst);
            if (cardIsEnabled) {
                (void)skiq_disable_cards(list, 1);
                cardIsEnabled = false;
            }
            return;
        }

        running.store(true);
        rxThread = std::thread(&SidekiqSourceModule::rxLoop, this);
        flog::info("SidekiqSource {}: Start", name);
    }

    void stopStream() {
        if (!running.load()) return;
        running.store(false);
        if (rxThread.joinable()) rxThread.join();

        (void)skiq_stop_rx_streaming(activeCard, activeRxHdl);

        if (cardIsEnabled) {
            uint8_t list[1] = { activeCard };
            (void)skiq_disable_cards(list, 1);
            cardIsEnabled = false;
        }

        stream.clearWriteStop();
        flog::info("SidekiqSource {}: Stop", name);
    }

    void rxLoop() {
        int32_t blk_bytes = skiq_read_rx_block_size(activeCard, streamMode);
        if (blk_bytes <= 0) blk_bytes = 4096;

        const double scale = 1.0 / 2048.0; // 12-bit -> float
        std::vector<dsp::complex_t> out;

        while (running.load()) {
            skiq_rx_block_t* pblk = nullptr;
            skiq_rx_hdl_t    hdl  = activeRxHdl;
            uint32_t         len  = 0;

            skiq_rx_status_t st = skiq_receive(activeCard, &hdl, &pblk, &len);
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

private:
    // Detect if current device supports NV presets (NV100 / NVM2)
    void detectNvSupport() {
        nvProfilesEnabled = false;
        if (devices.empty()) return;
        uint8_t card = devices.value(deviceIdx);

        skiq_param_t param{};
        if (skiq_read_parameters(card, &param) == 0) {
            auto pt = param.card_param.part_type;
            if (pt == skiq_nv100 || pt == skiq_nvm2) {
                nvProfilesEnabled = true;
            }
        }
    }

    void saveNvPresetConfig() {
        g_config.acquire();
        g_config.conf["useProfiles"]      = useProfiles;
        g_config.conf["nvSelectedRateHz"] = (int)nvSelectedRateHz;
        g_config.conf["nvBwPercent"]      = (int)nvBwPercent;
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

private:
    struct Profile {
        std::string name;
        uint32_t    sample_rate;
        uint32_t    bandwidth;
    };

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
    bool           cardIsEnabled = false; // only disable if we enabled

    // Settings
    int                    sampleRate;
    int                    bandwidth;
    uint64_t               centerFreq;
    skiq_rx_gain_t         gainMode;
    int                    gainIndex;
    skiq_rx_stream_mode_t  streamMode;
    // Cached legal RX gain index range for the active handle
    uint8_t                gainMin;
    uint8_t                gainMax;

    // NV presets state
    bool        useProfiles;       // user toggle
    bool        nvProfilesEnabled; // detected capability
    uint32_t    nvSelectedRateHz;
    int         nvBwPercent;

    // Persist RF port per device (by name key, e.g., "J1")
    json rfPortByDevice;

    std::thread       rxThread;
    std::atomic<bool> running{false};
    std::atomic<bool> libInited{false};
    bool enabled = true;
};

// ===== module glue =====
MOD_EXPORT void _INIT_() {
    json def = json({});
    def["device"]          = "";
    def["streamMode"]      = (int)skiq_rx_stream_mode_high_tput;
    def["sampleRate"]      = 10000000;
    def["bandwidth"]       = 10000000;
    def["gainMode"]        = 0;   // 0=manual, 1=auto
    def["gainIndex"]       = 30;
    def["useProfiles"]     = false;
    def["nvSelectedRateHz"]= 10000000;
    def["nvBwPercent"]     = 50;
    def["rfPortByDevice"]  = json::object();

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
