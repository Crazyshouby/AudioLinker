// Automatic latency optimizer: steps the ring cushion down while playback
// stays clean, backs off at the first glitch.

#include "App.h"

namespace al {

// --- Automatic latency optimizer ---

// Total underruns/drops across every active output right now -- the signal
// the optimizer watches. Rising during a window means the current cushion is
// too low for this machine's scheduling.
uint64_t TotalGlitches() {
    uint64_t total = 0;
    for (const auto& st : g_engine.getStatus()) {
        total += st.underruns + st.driftCorrections;
    }
    return total;
}

void ApplyOptimizerCushion(int ms) {
    g_baseCushionMs = ClampCushionMs(ms);
    g_engine.setBaseCushionMs(g_baseCushionMs);
    g_engine.resyncAllOutputs();
}

void StopOptimizer(HWND hwnd, const std::wstring& infoMsg) {
    KillTimer(hwnd, IDT_OPT_TIMER);
    g_optimizing = false;
    SaveConfig();
    SendState();
    if (!infoMsg.empty()) SendInfo(infoMsg);
}

// One measurement window. Steps the cushion down while clean; on the first
// glitchy window it backs off (raises the cushion) and stops once clean
// again, landing on the lowest stable value.
void OptimizerTick(HWND hwnd) {
    if (!g_engine.isRunning()) { StopOptimizer(hwnd, L"Optimisation interrompue : le groupe s'est arrêté."); return; }
    if (++g_optTicks > kOptMaxTicks) { StopOptimizer(hwnd, L"Optimisation terminée."); return; }

    if (g_optSettle) {
        // A cushion change just landed; ignore its transient and start a
        // fresh clean-window measurement from here.
        g_optSettle = false;
        g_optBaselineGlitches = TotalGlitches();
        return;
    }

    bool glitched = TotalGlitches() > g_optBaselineGlitches;
    if (glitched) {
        if (g_baseCushionMs >= kMaxBaseCushionMs) {
            StopOptimizer(hwnd, L"Optimisation : craquements persistants même à la marge maximale. "
                                L"C'est probablement le lien Bluetooth, pas AudioLinker.");
            return;
        }
        ApplyOptimizerCushion(g_baseCushionMs + kOptStepMs);
        g_optBackingOff = true;
        g_optSettle = true;
        SendState(); // cushion slider follows live
    } else {
        if (g_optBackingOff) {
            StopOptimizer(hwnd, L"Optimisation terminée : marge stable trouvée à " +
                                std::to_wstring(g_baseCushionMs) + L" ms.");
            return;
        }
        if (g_baseCushionMs <= kMinBaseCushionMs) {
            StopOptimizer(hwnd, L"Optimisation terminée : latence minimale (" +
                                std::to_wstring(g_baseCushionMs) + L" ms) atteinte sans craquement.");
            return;
        }
        ApplyOptimizerCushion(g_baseCushionMs - kOptStepMs);
        g_optSettle = true;
        SendState();
    }
}

void StartOptimizer(HWND hwnd) {
    if (g_optimizing) return;
    if (!g_engine.isRunning()) {
        SendError(L"Démarrez le groupe avant de lancer l'optimisation de la latence.");
        return;
    }
    g_optimizing = true;
    g_optBackingOff = false;
    g_optSettle = true; // first window just seeds the baseline
    g_optTicks = 0;
    g_optBaselineGlitches = TotalGlitches();
    SetTimer(hwnd, IDT_OPT_TIMER, kOptWindowMs, nullptr);
    SendInfo(L"Optimisation en cours… la marge est réduite progressivement, "
             L"gardez une lecture audio active pendant la mesure.");
    SendState();
}

} // namespace al
