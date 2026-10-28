#include "check.h"
#include "Config.h"

#include <windows.h>
#include <cstdio>
#include <fstream>
#include <string>

namespace {

// Writes a legacy INI fixture (UTF-16 LE, as WritePrivateProfileString
// produces) into the temp folder and returns its path.
std::wstring WriteIniFixture(const std::wstring& body) {
    wchar_t dir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, dir);
    std::wstring path = std::wstring(dir) + L"audiolinker_test_" + std::to_wstring(GetCurrentProcessId()) + L".ini";
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"wb");
    if (!f) return L"";
    const wchar_t bom = 0xFEFF;
    fwrite(&bom, sizeof(bom), 1, f);
    fwrite(body.data(), sizeof(wchar_t), body.size(), f);
    fclose(f);
    return path;
}

const wchar_t* kDevA = L"{0.0.0.00000000}.{aaaaaaaa-0000-0000-0000-000000000001}";
const wchar_t* kDevB = L"{0.0.0.00000000}.{bbbbbbbb-0000-0000-0000-000000000002}";

} // namespace

TEST(json_roundtrip_preserves_everything) {
    AppConfig c;
    c.autostart = true;
    c.masterVolume = 73;
    c.micId = L"mic-1";
    c.cushionMs = 210;
    c.deviceFillMs = 40;
    c.lowLatency = true;
    c.theme = 2;
    c.previousDefault = L"prev";
    c.toggleMods = 6; c.toggleVk = 'K';
    c.setupDigitHotkeys = false;
    OutputSettings a;
    a.checked = true; a.latencyMs = 120; a.channelMode = 1; a.volumePercent = 80;
    a.eqDb = { -3, 0, 2, 4, 6 }; a.muted = true; a.order = 2;
    a.name = L"Chambre (BT)"; a.alias = L"Chambre « accent é »"; a.colorHex = L"e07a5f"; a.lastSeen = 1700000000;
    c.devices[kDevA] = a;
    Setup s;
    s.name = L"Soirée";
    s.micId = L"mic-1"; s.micVolume = 55; s.micMute = true;
    s.devices.emplace_back(kDevA, a);
    c.setups.push_back(s);
    c.activeSetup = L"Soirée";
    c.editingSetup = L"Soirée";
    c.routedApps = { L"Spotify.exe" };
    c.window = { 10, 20, 900, 700 };
    c.panels["outputs"] = { 260, 40, 620, 520, true, false };
    c.panels["journal"] = { 5, 6, 300, 200, false, true };
    c.automation.push_back({ kDevA, L"Soirée", L"stop" });

    std::string text = SerializeConfig(c);
    AppConfig back;
    CHECK(ParseConfig(text, back));
    CHECK_EQ(back.autostart, true);
    CHECK_EQ(back.masterVolume, 73);
    CHECK(back.micId == L"mic-1");
    CHECK_EQ(back.cushionMs, 210);
    CHECK_EQ(back.deviceFillMs, 40);
    CHECK_EQ(back.lowLatency, true);
    CHECK_EQ(back.theme, 2);
    CHECK(back.previousDefault == L"prev");
    CHECK_EQ(back.toggleMods, 6u);
    CHECK_EQ(back.toggleVk, unsigned('K'));
    CHECK_EQ(back.setupDigitHotkeys, false);
    CHECK_EQ(back.devices.size(), size_t(1));
    const auto& ba = back.devices[kDevA];
    CHECK_EQ(ba.latencyMs, 120);
    CHECK_EQ(ba.channelMode, 1);
    CHECK_EQ(ba.eqDb[4], 6);
    CHECK_EQ(ba.muted, true);
    CHECK_EQ(ba.order, 2);
    CHECK(ba.name == L"Chambre (BT)");
    CHECK(ba.alias == L"Chambre « accent é »");
    CHECK(ba.colorHex == L"e07a5f");
    CHECK_EQ(ba.lastSeen, 1700000000LL);
    CHECK_EQ(back.setups.size(), size_t(1));
    CHECK(back.setups[0].name == L"Soirée");
    CHECK_EQ(back.setups[0].micVolume, 55);
    CHECK_EQ(back.setups[0].micMute, true);
    CHECK_EQ(back.setups[0].devices.size(), size_t(1));
    CHECK(back.setups[0].devices[0].first == kDevA);
    CHECK_EQ(back.setups[0].devices[0].second.eqDb[0], -3);
    CHECK(back.activeSetup == L"Soirée");
    CHECK_EQ(back.routedApps.size(), size_t(1));
    CHECK_EQ(back.window.w, 900);
    CHECK_EQ(back.panels.size(), size_t(2));
    CHECK_EQ(back.panels["outputs"].x, 260);
    CHECK_EQ(back.panels["outputs"].h, 520);
    CHECK(back.panels["outputs"].open && !back.panels["outputs"].shaded);
    CHECK(!back.panels["journal"].open && back.panels["journal"].shaded);
    CHECK_EQ(back.automation.size(), size_t(1));
    CHECK(back.automation[0].onDisconnect == L"stop");
}

TEST(parse_clamps_out_of_range_and_drops_dangling_setup_names) {
    std::string text = R"({
      "group": {"masterVolume": 400, "cushionMs": 5, "theme": 9},
      "setups": {"active": "nope", "list": []},
      "devices": {"x": {"volume": -5, "latency": 9999, "channel": 7, "eq": [50, -50, 0, 0, 0]}}
    })";
    AppConfig c;
    CHECK(ParseConfig(text, c));
    CHECK_EQ(c.masterVolume, 100);
    CHECK_EQ(c.cushionMs, kMinBaseCushionMs);
    CHECK_EQ(c.theme, 2);
    CHECK(c.activeSetup.empty());
    CHECK_EQ(c.devices[L"x"].volumePercent, 0);
    CHECK_EQ(c.devices[L"x"].latencyMs, 500);
    CHECK_EQ(c.devices[L"x"].channelMode, 2);
    CHECK_EQ(c.devices[L"x"].eqDb[0], 12);
    CHECK_EQ(c.devices[L"x"].eqDb[1], -12);
}

TEST(parse_rejects_garbage) {
    AppConfig c;
    CHECK(!ParseConfig("not json", c));
    CHECK(!ParseConfig("[1,2,3]", c));
}

TEST(legacy_ini_migrates_devices_setups_and_old_field_counts) {
    std::wstring body =
        L"[group]\r\nautostart=1\r\nmastervolume=90\r\ncushionms=200\r\ntheme=1\r\n"
        L"[hotkeys]\r\ntogglemods=3\r\ntogglevk=71\r\nsetupdigits=0\r\n"
        L"[" + std::wstring(kDevA) + L"]\r\nchecked=1\r\nlatency=150\r\nchannel=1\r\nvolume=85\r\n"
        L"eq0=-2\r\neq1=1\r\neq2=0\r\neq3=3\r\neq4=5\r\nmuted=0\r\norder=1\r\nname=Chambre\r\nalias=Ma chambre\r\ncolor=3ddc84\r\n"
        L"[" + std::wstring(kDevB) + L"]\r\nchecked=0\r\nbass=4\r\ntreble=-4\r\n"
        L"[setups]\r\ncount=3\r\nactive=Deux\r\nediting=Deux\r\n"
        // 7 fields (pre-muted era), 8 fields (pre-5-band), 11 fields (current).
        L"[setup_0]\r\nname=Vieux\r\ndevcount=1\r\ndev0=" + std::wstring(kDevA) + L"|1|0|70|100|2|-2\r\n"
        L"[setup_1]\r\nname=Deux\r\ndevcount=2\r\ndev0=" + std::wstring(kDevA) + L"|1|2|60|50|1|-1|1\r\n"
        L"dev1=" + std::wstring(kDevB) + L"|0|0|100|0|0|0|0\r\n"
        L"[setup_2]\r\nname=Complet\r\nmic=mic-x\r\nmicvol=40\r\nmicmute=1\r\ndevcount=1\r\n"
        L"dev0=" + std::wstring(kDevA) + L"|1|1|80|120|-3|6|0|0|2|4\r\n"
        L"[routing]\r\napps=Spotify.exe;chrome.exe\r\n"
        L"[window]\r\nx=5\r\ny=6\r\nw=800\r\nh=600\r\n";
    std::wstring path = WriteIniFixture(body);
    CHECK(!path.empty());
    AppConfig c;
    CHECK(ParseLegacyIni(path, c));
    DeleteFileW(path.c_str());

    CHECK_EQ(c.autostart, true);
    CHECK_EQ(c.masterVolume, 90);
    CHECK_EQ(c.cushionMs, 200);
    CHECK_EQ(c.theme, 1);
    CHECK_EQ(c.toggleMods, 3u);
    CHECK_EQ(c.setupDigitHotkeys, false);

    CHECK_EQ(c.devices.size(), size_t(2));
    const auto& a = c.devices[kDevA];
    CHECK_EQ(a.checked, true);
    CHECK_EQ(a.latencyMs, 150);
    CHECK_EQ(a.eqDb[3], 3);
    CHECK(a.name == L"Chambre");
    CHECK(a.alias == L"Ma chambre");
    CHECK(a.colorHex == L"3ddc84");
    CHECK(a.lastSeen > 0);
    const auto& b = c.devices[kDevB];
    CHECK_EQ(b.eqDb[0], 4);  // legacy bass -> low shelf
    CHECK_EQ(b.eqDb[4], -4); // legacy treble -> high shelf

    CHECK_EQ(c.setups.size(), size_t(3));
    CHECK(c.setups[0].name == L"Vieux");
    CHECK_EQ(c.setups[0].devices[0].second.eqDb[0], 2);
    CHECK_EQ(c.setups[0].devices[0].second.eqDb[4], -2);
    CHECK_EQ(c.setups[0].devices[0].second.muted, false);
    CHECK_EQ(c.setups[1].devices.size(), size_t(2));
    CHECK_EQ(c.setups[1].devices[0].second.muted, true);
    CHECK_EQ(c.setups[1].devices[0].second.channelMode, 2);
    CHECK(c.setups[2].micId == L"mic-x");
    CHECK_EQ(c.setups[2].micVolume, 40);
    CHECK_EQ(c.setups[2].micMute, true);
    CHECK_EQ(c.setups[2].devices[0].second.eqDb[1], 0);
    CHECK_EQ(c.setups[2].devices[0].second.eqDb[2], 2);
    CHECK_EQ(c.setups[2].devices[0].second.eqDb[3], 4);
    CHECK(c.activeSetup == L"Deux");

    CHECK_EQ(c.routedApps.size(), size_t(2));
    CHECK(c.routedApps[1] == L"chrome.exe");
    CHECK_EQ(c.window.w, 800);
}

TEST(purge_drops_old_unreferenced_devices_only) {
    AppConfig c;
    const long long now = 2000000000LL;
    const long long old = now - 100LL * 24 * 3600;
    OutputSettings fresh; fresh.lastSeen = now - 10;
    OutputSettings stale; stale.lastSeen = old;
    OutputSettings never; never.lastSeen = 0;
    c.devices[L"fresh"] = fresh;
    c.devices[L"stale"] = stale;
    c.devices[L"stale-but-in-setup"] = stale;
    c.devices[L"never"] = never;
    Setup s; s.name = L"S"; s.devices.emplace_back(L"stale-but-in-setup", stale);
    c.setups.push_back(s);
    CHECK_EQ(PurgeStaleDevices(c, now), 2);
    CHECK_EQ(c.devices.count(L"fresh"), size_t(1));
    CHECK_EQ(c.devices.count(L"stale"), size_t(0));
    CHECK_EQ(c.devices.count(L"stale-but-in-setup"), size_t(1));
    CHECK_EQ(c.devices.count(L"never"), size_t(0));
}

TEST(split_pipes_keeps_empty_fields) {
    auto p = SplitPipes(L"a||c|");
    CHECK_EQ(p.size(), size_t(4));
    CHECK(p[1].empty());
    CHECK(p[3].empty());
}

TEST_MAIN()
