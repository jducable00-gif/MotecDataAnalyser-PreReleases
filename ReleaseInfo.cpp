//---------------------------------------------------------------------------
// ReleaseInfo.cpp - Implementation du fetch WinHTTP + parse JSON GitHub.
// Voir ReleaseInfo.h pour le contrat (fetch async, repo prive via credentials
// Windows, cache INI gere par la form).
//---------------------------------------------------------------------------

// Pas de vcl.h ici : ce fichier n'utilise que WinHTTP + std (pas de types VCL).
// L'ordre winsock2 -> windows -> winhttp est garanti par ReleaseInfo.h.
#pragma hdrstop

#include "ReleaseInfo.h"   // winsock2/windows/winhttp + pragma comment(lib, winhttp)
#include "RuleConfig.h"    // JsonParser / JsonValue

#include <cctype>          // isdigit (versionSegments)
#include <vector>

namespace {

std::wstring toWideUtf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, NULL, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    while (!w.empty() && w.back() == L'\0') w.pop_back();   // le -1 incluait l'NUL final
    return w;
}

std::string toUtf8(const std::wstring& s) {                // inverse de toWideUtf8 (header Location -> utf8)
    if (s.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, NULL, 0, NULL, NULL);
    if (n <= 0) return std::string();
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, &out[0], n, NULL, NULL);
    while (!out.empty() && out.back() == '\0') out.pop_back();   // le -1 incluait l'NUL final
    return out;
}

// Normalise les fins de ligne d'un corps de release notes vers CRLF : l'API GitHub
// renvoie des \n seuls et VCL (TStrings/TMemo) ne coupe la ligne que sur CR+LF —
// sans cette normalisation, les notes s'afficheraient en une seule ligne continue.
static std::string normalizeLineEndings(const std::string& in) {
    std::string out; out.reserve(in.size());   // un \n devient 2 octets (\r\n), reallocation eventuelle sans consequence
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\r') { out.push_back('\r'); if (i + 1 < in.size() && in[i+1] == '\n') ++i; }
        else if (in[i] == '\n') { out += "\r\n"; }
        else out.push_back(in[i]);
    }
    return out;
}

// Lambda-like découpe d'une URL absolue "https://host[:port]/path" en host / port / path.
static bool splitHttpUrl(const std::string& url, std::wstring& outHost, int& outPort, std::wstring& outPath) {
    const size_t se = url.find("://");                      // schéma obligatoire (https://...)
    if (se == std::string::npos || se + 3 >= url.size()) return false;

    const size_t ps = url.find('/', se + 3);                // premier '/' après le host -> fin du "host[:port]"
    const std::wstring wHostFull = toWideUtf8(url.substr(se + 3, (ps != std::string::npos) ? ps - (se + 3) : std::string::npos));
    if (wHostFull.empty()) return false;

    outPort = 443;                                          // HTTPS par défaut ; "host:port" optionnel
    const size_t colon = wHostFull.find(':');               // ':' -> port numérique strictement requis, sinon échec
    if (colon != std::wstring::npos) {
        int p = 0; bool digits = false;                     // pas de _wtoi : parse manuel sans <cstdlib>
        for (size_t k = colon + 1; k < wHostFull.size(); ++k) {
            const wchar_t c = wHostFull[k];
            if (c < L'0' || c > L'9') return false;         // caractère non-chiffre dans le port -> URL invalide ici
            p = p * 10 + (c - L'0'); digits = true;
        }
        outPort = digits ? p : 443;                         // "host:" sans chiffre -> on retombe sur 443
    }

    const size_t hostEnd = (colon == std::wstring::npos) ? wHostFull.size() : colon;   // host seul, port retiré
    if (hostEnd == 0) return false;                                                    // host vide ("https://:443/...") -> échec net.
                                                                                       // ATTENTION : sans ':' , hostEnd==size est le CAS NORMAL ("https://github.com") —
                                                                                       // l'ancienne condition "hostEnd >= size" rejetait TOUTES les URLs sans port explicite
                                                                                       // et tuait le self-update à la première ligne de downloadReleaseAsset.
    outHost = wHostFull.substr(0, hostEnd);

    outPath = (ps != std::string::npos) ? toWideUtf8(url.substr(ps)) : L"/";           // path avec '/' initial, query incluse
    return true;
}

// GET https://api.github.com/repos/<owner>/<repo>/releases/latest via WinHTTP.
// Repo prive sans token : WINHTTP_ACCESS_TYPE_DEFAULT_PROXY delegue les
// credentials Windows du compte connecte (memes mecanismes que git push ici).
bool httpGetLatestRelease(const std::string& owner, const std::string& repo,
                          int timeoutMs, AppRelease& out) {
    if (owner.empty() || repo.empty()) { ReleaseInfoLog("detection: empty owner/repo"); return false; }

    ReleaseInfoLog(("detection begin: /repos/" + owner + "/" + repo + "/releases/latest").c_str());

    const std::wstring ua = toWideUtf8(std::string("MotecDataAnalyser/") + kAppVersion + " (+release-check)");
    HINTERNET session = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!session) { ReleaseInfoLog(("WinHttpOpen FAILED gle=" + std::to_string(GetLastError())).c_str()); return false; }

    bool ok = false;
    do {
        std::wstring wPath = toWideUtf8("/repos/" + owner + "/" + repo + "/releases/latest");
        HINTERNET connect = WinHttpConnect(session, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!connect) { ReleaseInfoLog(("WinHttpConnect FAILED gle=" + std::to_string(GetLastError())).c_str()); break; }

        // lpszReferrer / acceptTypes : NULL (equivalents de WINHTTP_NO_REFERER /
        // WINHTTP_DEFAULT_ACCEPT_TYPES) — le header winhttp.h n'est pas inclus.
        HINTERNET request = WinHttpOpenRequest(connect, L"GET", wPath.c_str(), NULL,
                                               NULL, NULL, WINHTTP_FLAG_SECURE);
        if (!request) { ReleaseInfoLog(("WinHttpOpenRequest FAILED gle=" + std::to_string(GetLastError())).c_str()); WinHttpCloseHandle(connect); break; }

        // Timeouts reels (ms) : resolve / connect / send / receive.
        const DWORD t = timeoutMs > 0 ? static_cast<DWORD>(timeoutMs) : 8000u;
        WinHttpSetTimeouts(request, t, t, t, t);

        std::wstring headers = L"Accept: application/vnd.github+json\r\n";
        if (!WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(headers.size()), NULL, 0, 0, 0)) {
            ReleaseInfoLog(("WinHttpSendRequest FAILED gle=" + std::to_string(GetLastError())).c_str());
            WinHttpCloseHandle(request); WinHttpCloseHandle(connect); break;
        }

        DWORD status = 404; DWORD sz = sizeof(status);
        if (!WinHttpReceiveResponse(request, NULL) || !WinHttpQueryHeaders(
                request, RELEASEINFO_QRY_STATUS,   // STATUS_CODE | FLAG_NUMBER
                NULL /*WINHTTP_HEADER_NAME_BY_INDEX*/, &status, &sz, NULL)) {
            ReleaseInfoLog(("WinHttpReceiveResponse/QueryHeaders FAILED gle=" + std::to_string(GetLastError())
                           + " status=" + std::to_string(status)).c_str());
            WinHttpCloseHandle(request); WinHttpCloseHandle(connect); break;
        }

        if (status < 200 || status >= 300) {   // 401/403 = pas d'acces au repo prive ; 404 = aucun release
            ReleaseInfoLog(("HTTP non-2xx status=" + std::to_string(status)).c_str());
            WinHttpCloseHandle(request); WinHttpCloseHandle(connect); break;
        }

        std::string body;
        char buf[8192];
        for (;;) {
            DWORD bytes = 0;
            if (!WinHttpReadData(request, buf, sizeof(buf), &bytes) || bytes == 0) break;
            body.append(buf, bytes);
        }

        ReleaseInfoLog(("HTTP 2xx OK, body=" + std::to_string(body.size()) + " bytes").c_str());
        ok = parseLatestReleaseJson(body, out);   // defini plus bas dans ce fichier
        if (!ok) {
            ReleaseInfoLog("parseLatestReleaseJson FAILED (bad JSON or missing tag_name/assets)");
        } else {
            ReleaseInfoLog(("detection OK: tag=" + out.tagName).c_str());
        }
    } while (false);

    WinHttpCloseHandle(session);   // ferme egalement les handles connect/request enfants.
    return ok;
}

const char* kMonthsFr[12] = { "janv.", "fevr.", "mars", "avr.", "mai", "juin",
                              "juil.", "aout", "sept.", "oct.", "nov.", "dec." };
// NB : sans accents volontairement (litteraux CP1252 bcc32c, style du projet).

const char* kMonthsEn[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

} // namespace

// ===============================
// Comparaison de versions. "V5.6" / "v5.10-rc1" -> segments [5,6] et [5,10].
// Insensible au préfixe v/V ; comparaison numérique segment par segment ; un
// segment manquant vaut 0 ("5.6" == "5.6.0"). Retourne -1 / 0 / +1.
// ===============================

static std::vector<int> versionSegments(const std::string& s) {
    // Sauter le préfixe (v/V/espaces...) jusqu'au premier chiffre.
    size_t i = 0;
    while (i < s.size() && !isdigit(static_cast<unsigned char>(s[i]))) ++i;

    std::vector<int> segs;
    int cur = -1;   // -1 = pas de segment en cours ; sinon valeur accumulée
    for (; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (isdigit(c)) {
            // Premier chiffre du segment -> valeur initiale ; sinon accumulation.
            cur = (cur == -1) ? (c - '0') : cur * 10 + (c - '0');
        } else if (c == '.' && cur != -1) {
            segs.push_back(cur);   // séparateur de segments ("5.6" -> [5, 6])
            cur = -1;
        } else {
            // Premier caractère non-chiffre après un chiffre ("-rc", " ") -> fin de la version.
            if (cur != -1) segs.push_back(cur);
            break;
        }
    }
    if (cur != -1) segs.push_back(cur);
    return segs;
}

int compareVersions(const std::string& a, const std::string& b) {
    const std::vector<int> sa = versionSegments(a);
    const std::vector<int> sb = versionSegments(b);
    const size_t n = (sa.size() > sb.size()) ? sa.size() : sb.size();
    for (size_t i = 0; i < n; ++i) {
        const int va = (i < sa.size()) ? sa[i] : 0;   // segment manquant = 0
        const int vb = (i < sb.size()) ? sb[i] : 0;
        if (va != vb) return (va < vb) ? -1 : +1;
    }
    return 0;
}

// true si le release porte un asset .exe téléchargeable (assetUrl non vide).
static bool isExeAssetPresent(const AppRelease& r) { return !r.assetUrl.empty(); }

bool isUpdateAvailable(const AppRelease& release) {
    if (!isExeAssetPresent(release)) return false;   // pas de binaire .exe -> rien à installer, même si le tag est plus récent
    return compareVersions(kAppVersion, release.tagName) < 0;   // tag strictement plus récent que la version compilée
}

// ===============================
bool parseLatestReleaseJson(const std::string& json, AppRelease& out) {
    JsonValue root;
    std::string err;
    if (!JsonParser::parse(json, root, err)) return false;

    const JsonValue* tag = root.find("tag_name");
    if (!tag || tag->type != JsonValue::String || tag->str.empty()) return false;

    out.tagName = tag->str;
    if (const JsonValue* v = root.find("html_url"))     if (v->type == JsonValue::String)  out.htmlUrl = v->str;
    if (const JsonValue* v = root.find("published_at")) if (v->type == JsonValue::String)  out.publishedAt = v->str;
    // Normalise les fins de ligne en CRLF : l'API GitHub renvoie des \n seuls et VCL
    // (TStrings/TMemo) ne coupe la ligne que sur CR+LF — sans cela, notes en une seule ligne.
    if (const JsonValue* v = root.find("body"))         if (v->type == JsonValue::String)  out.body = normalizeLineEndings(v->str);

    // assets[] : premier asset dont le nom se termine par ".exe" -> browser_download_url.
    if (const JsonValue* arr = root.find("assets"); arr && arr->type == JsonValue::Array) {
        for (size_t i = 0; i < arr->array.size(); ++i) {
            const JsonValue& a = arr->array[i];   // chaque asset est un objet JSON
            if (!a.type || !a.find("name")) continue;
            const std::string name = a.find("name")->str;
            size_t dot = name.rfind('.');
            if (dot == std::string::npos) continue;
            std::string ext = name.substr(dot + 1);   // extension après le dernier point
            for (size_t k = 0; k < ext.size(); ++k) ext[k] = static_cast<char>(tolower(ext[k]));
            if (ext != "exe") continue;               // on ne télécharge que les binaires .exe

            const JsonValue* dl = a.find("browser_download_url");
            if (dl && dl->type == JsonValue::String) { out.assetUrl = dl->str; break; }
        }
    }

    // mapping.json : dérivé de html_url -> raw.githubusercontent.com/{owner}/{repo}/{tag}/MotecDataAnalyser.mapping.json
    // Pas besoin d'asset supplémentaire sur le release — le fichier est dans l'arbre dist.
    if (!out.htmlUrl.empty() && !out.tagName.empty()) {
        const std::string prefix = "https://github.com/";
        size_t start = out.htmlUrl.find(prefix);
        if (start != std::string::npos) {
            start += prefix.size();   // après "https://github.com/"
            size_t slash1 = out.htmlUrl.find('/', start);  // fin de owner
            if (slash1 != std::string::npos) {
                size_t slash2 = out.htmlUrl.find('/', slash1 + 1);  // fin de repo
                if (slash2 != std::string::npos && slash2 > slash1 + 1) {
                    std::string ownerRepo = out.htmlUrl.substr(start, slash2 - start);
                    out.mappingUrl = "https://raw.githubusercontent.com/" + ownerRepo + "/" + out.tagName + "/MotecDataAnalyser.mapping.json";
                }
            }
        }
    }

    return true;
}

// ===============================
std::string formatReleaseDate(const std::string& isoUtc, bool french) {
    // "2026-10-24T18:30:00Z" -> jour + mois abrege + annee. Le reste est ignore.
    if (isoUtc.size() < 10) return isoUtc;   // pas assez long pour YYYY-MM-DD
    int year = 0, month = 0, day = 0;
    for (int i = 0; i < 4 && isoUtc[i] >= '0' && isoUtc[i] <= '9'; ++i)
        year = year * 10 + (isoUtc[i] - '0');
    if (isoUtc[4] != '-') return isoUtc;
    for (int i = 5; i < 7 && isoUtc[i] >= '0' && isoUtc[i] <= '9'; ++i)
        month = month * 10 + (isoUtc[i] - '0');
    if (isoUtc[7] != '-') return isoUtc;
    for (int i = 8; i < 10 && isoUtc[i] >= '0' && isoUtc[i] <= '9'; ++i)
        day = day * 10 + (isoUtc[i] - '0');
    if (year < 2000 || month < 1 || month > 12 || day < 1 || day > 31) return isoUtc;

    const char* mname = french ? kMonthsFr[month - 1] : kMonthsEn[month - 1];
    std::string s;
    if (french) {
        // "24 oct. 2026"
        s.append(std::to_string(day)).append(" ").append(mname).append(" ")
           .append(std::to_string(year));
    } else {
        // "Oct 24, 2026"
        s.append(mname).append(" ").append(std::to_string(day))
          .append(", ").append(std::to_string(year));
    }
    return s;
}

// ===============================
bool fetchLatestReleaseSync(const std::string& owner, const std::string& repo,
                            AppRelease& out, int timeoutMs) {
    // Le host est code en dur dans httpGetLatestRelease (api.github.com).
    return httpGetLatestRelease(owner, repo, timeoutMs, out);
}

// ===============================
// État global du module : le worker écrit ici, la form lit via takePendingRelease().
// Statiques de portée fichier -> vivent toute la durée du processus ; un thread
// détaché ne peut donc jamais référencer de mémoire libérée (aucun objet VCL).
namespace {
    std::mutex          g_releaseMutex;      // protège uniquement les variables ci-dessous (jamais de VCL)
    AppRelease          g_best{};            // meilleur release de la dernière vague (merge multi-repos)
    bool                g_hasPending = false;// true si un résultat attend la form (takePendingRelease)

    // --- Etat de la "vague" en cours : 1 appel fetchLatestReleasesAsync() = 1 vague, 1 slot par repo.
    std::atomic<int>        g_waveSeq{0};      // incrémenté à chaque nouvelle vague ; les workers d'une vague remplacée s'ignorent
    std::vector<AppRelease> g_waveResults;     // sous mutex, indexé sur l'ordre de repos[] au lancement (index = priorité)
    std::atomic<int>        g_workersLeft{0};  // décompte atomique : un repo en échec ne bloque pas les autres

    int startWave(const int repoCount) {
        const int seq = g_waveSeq.fetch_add(1) + 1;   // séquence d'abord (sans lock), puis état sous mutex -> aucun worker n'existe encore, pas de course possible sur la création
        std::lock_guard<std::mutex> lk(g_releaseMutex);   // remplace proprement une vague précédente non récupérée / en cours
        g_hasPending = false;
        g_best = AppRelease{};
        g_waveResults.assign(repoCount, AppRelease{});
        return seq;
    }

    void mergeWaveResults() {
        std::lock_guard<std::mutex> lk(g_releaseMutex);   // appelé par le dernier worker terminé (décompte atomique) -> fusion sous mutex
        AppRelease best{}; bool haveInstallable = false;
        for (const auto& r : g_waveResults) {             // la release INSTALLABLE la plus récente gagne ; à version égale, l'ordre des feeds tranchera (pre-release d'abord pour un testeur)
            if (!r.empty() && isUpdateAvailable(r)) {
                if (!haveInstallable || compareVersions(best.tagName, r.tagName) < 0) { best = r; haveInstallable = true; }
            }
        }
        if (!haveInstallable)                             // aucun installable -> on garde quand même la première trouvée (la form affichera "à jour")
            for (const auto& r : g_waveResults) { if (!r.empty()) { best = r; break; } }
        g_best = std::move(best);
        g_hasPending = !g_best.empty();   // vague totalement en échec -> pas de pending (comportement identique à l'ancien mono-repo)
    }
}

void fetchLatestReleasesAsync(const std::string& owner, const std::vector<std::string>& repos) {
    if (owner.empty() || repos.empty()) return;

    // Un thread détaché par dépôt : ne touche QUE l'état global du module ci-dessus. Si la form est détruite pendant le fetch
    // (ex: fermeture de l'app), aucun pointeur vers un objet VCL n'est référencé -> pas d'UAF, pas de crash au shutdown.
    const int seq = startWave(static_cast<int>(repos.size()));
    g_workersLeft.store(static_cast<int>(repos.size()));   // AVANT tout spawn : les workers ne peuvent pas décompter avant ce store

    for (int i = 0; i < static_cast<int>(repos.size()); ++i) {
        const std::string repo = repos[i];                 // copie par valeur dans la lambda -> safe sur thread détaché
        try {
            std::thread([owner, repo, i, seq]() mutable {
                AppRelease rel;
                const bool ok = fetchLatestReleaseSync(owner, repo, rel);   // échec réseau / pas de release : slot vide -> ne bloque personne

                if (ok && seq == g_waveSeq.load())    // vague remplacée entre-temps ? résultat obsolète, on l'abandonne sans toucher à la nouvelle
                    { std::lock_guard<std::mutex> lk(g_releaseMutex); g_waveResults[i] = std::move(rel); }

                if (g_workersLeft.fetch_sub(1) > 1) return;   // décompte TOUJOURS exécuté (même worker obsolète/échec), sinon la fusion n'arriverait jamais
                mergeWaveResults();                            // "moi" suis le dernier : publication de g_best/g_hasPending sous mutex
            }).detach();
        } catch (...) { /* thread creation failed (ressources) : ce repo est ignoré, la vague continue sans lui */ }
    }
}

bool takePendingRelease(AppRelease& out) {
    std::lock_guard<std::mutex> lk(g_releaseMutex);
    if (!g_hasPending) return false;   // rien de nouveau depuis le dernier appel (ou fetch en cours / échec réseau)
    g_hasPending = false;              // on consomme : la form re-pollera au prochain tick du timer
    out = g_best;                      // copie : petit struct de strings, une fois par vague -> coût négligeable sur le thread UI
    return true;
}

// ===============================
// Anti-boucle self-update (voir markJustUpdated dans ReleaseInfo.h).
namespace {
    std::atomic<bool> g_justUpdated{false};
}
void markJustUpdated()   { g_justUpdated.store(true);  ReleaseInfoLog("markJustUpdated: /updated session — update popup suppressed for this run"); }
bool wasJustUpdated()    { return g_justUpdated.load(); }

// ===============================
namespace {
    std::mutex g_updateLogMutex;
}
void ReleaseInfoLog(const char* msg) {
    try {
        wchar_t buf[MAX_PATH]{}; DWORD n = GetModuleFileNameW(NULL, buf, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return;
        std::wstring dir(buf, n); size_t sl = dir.find_last_of(L"\\/");
        if (sl != std::wstring::npos && sl > 2) dir.resize(sl);   // <dirExe>
        const std::wstring path = dir + L"\\MotecDataAnalyser.update.log";   // double backslash : \M serait une séquence d'échappement invalide (W2218) et casserait le chemin.
        HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) return;   // log best-effort : ne jamais faire échouer l'update pour ça
        SYSTEMTIME st{}; GetLocalTime(&st);
        char line[1024];
        int len = snprintf(line, sizeof(line), "[%04d-%02d-%02d %02d:%02d:%02d] %s\r\n",
                           st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, msg);
        if (len > 0) { DWORD wr = 0; WriteFile(h, line, static_cast<DWORD>(len), &wr, NULL); }
        CloseHandle(h);
    } catch (...) {}
}

// ===============================
// Téléchargement d'un asset (self-update). Thread worker uniquement — jamais de VCL ici,
// y compris dans onProgress() : la form ne lit que des atomiques via le callback.
// Les assets GitHub répondent par un redirect 302 vers un CDN ; WinHTTP ne suit PAS les
// redirects automatiquement -> boucle manuelle (max 5 sauts), URLs absolues uniquement.
bool downloadReleaseAsset(const std::string& urlUtf8, const std::wstring& destPath,
                          const std::function<void(long long receivedBytes, long long totalBytes)> onProgress) {
    // host / port / path courants — re-découpés à chaque saut de redirect.
    std::wstring wHost; int port = 443; std::wstring wPath = L"/";

    ReleaseInfoLog((std::string("downloadReleaseAsset START url=") + urlUtf8 + " dest=" + toUtf8(destPath)).c_str());
    HINTERNET session = WinHttpOpen(toWideUtf8(std::string("MotecDataAnalyser/") + kAppVersion).c_str(),
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!session) { ReleaseInfoLog(("WinHttpOpen FAILED gle=" + std::to_string(GetLastError())).c_str()); return false; }

    bool ok = false;
    do {
        if (!splitHttpUrl(urlUtf8, wHost, port, wPath)) { ReleaseInfoLog(("initial URL parse FAILED: " + urlUtf8).c_str()); break; }   // URL initiale invalide -> abandon immédiat

        HINTERNET connect = WinHttpConnect(session, wHost.c_str(), static_cast<USHORT>(port), 0);   // re-créé à chaque changement de host
        if (!connect) { ReleaseInfoLog(("WinHttpConnect FAILED gle=" + std::to_string(GetLastError()) + " host=" + toUtf8(wHost)).c_str()); break; }
        bool connectClosedInLoop = false;                      // true si le redirect a déjà fermé ce handle (pas de double-close en fin de bloc)

        for (int hop = 0; ; ++hop) {                           // boucle de redirects (max 5 sauts)
            if (hop > 4) break;                                // anti-boucle infinie CDN/proxy

            HINTERNET request = WinHttpOpenRequest(connect, L"GET", wPath.c_str(), NULL, NULL, NULL, WINHTTP_FLAG_SECURE);
            if (!request) { ReleaseInfoLog(("WinHttpOpenRequest FAILED gle=" + std::to_string(GetLastError()) + " hop=" + std::to_string(hop)).c_str()); connectClosedInLoop = true; break; }   // fermeture unique en fin de bloc (voir ci-dessous)

            const DWORD t = 15000u;                            // download : timeouts plus larges que le check (8 s)
            WinHttpSetTimeouts(request, t, t, t, t);

            std::wstring headers = L"Accept: */*\r\n";         // assets GitHub -> redirect CDN ; Accept libre
            if (!WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(headers.size()), NULL, 0, 0, 0)) {
                ReleaseInfoLog(("WinHttpSendRequest FAILED gle=" + std::to_string(GetLastError())
                    + " hop=" + std::to_string(hop) + " host=" + toUtf8(wHost)).c_str());   // proxy/SSL/DNS : cause n°1 d'échec silencieux
                WinHttpCloseHandle(request); break; }
            if (!WinHttpReceiveResponse(request, NULL)) {
                ReleaseInfoLog(("WinHttpReceiveResponse FAILED gle=" + std::to_string(GetLastError())
                    + " hop=" + std::to_string(hop) + " host=" + toUtf8(wHost)).c_str());   // timeout 15 s / reset TLS
                WinHttpCloseHandle(request); break; }

            DWORD status = 599; DWORD sz = sizeof(status);     // WINHTTP_QUERY_STATUS_CODE | FLAG_NUMBER
            WinHttpQueryHeaders(request, RELEASEINFO_QRY_STATUS, NULL /*WINHTTP_HEADER_NAME_BY_INDEX*/, &status, &sz, NULL);

            if (status == 301 || status == 302 || status == 307) {   // redirect -> header Location
                DWORD locLen = 0;                                  // WINHTTP_QUERY_LOCATION : taille requise d'abord (-1 + len dans *lpdwBufferLength)
                WinHttpQueryHeaders(request, 33u /*WINHTTP_QUERY_LOCATION*/, NULL, NULL, &locLen, NULL);   // SDK winhttp.h : LOCATION=33 (4 = CONTENT_DESCRIPTION — bug v5.6.13)
                if (locLen < 2) { ReleaseInfoLog(("redirect hop=" + std::to_string(hop) + ": Location header absent/empty").c_str()); WinHttpCloseHandle(request); break; }   // pas de Location exploitable

                std::wstring location(locLen - 1, L'\0');          // locLen incluait le NUL final
                if (!WinHttpQueryHeaders(request, 33u /*WINHTTP_QUERY_LOCATION*/, NULL, &location[0], &locLen, NULL)) { ReleaseInfoLog(("redirect hop=" + std::to_string(hop) + ": WinHttpQueryHeaders(LOCATION) FAILED gle=" + std::to_string(GetLastError())).c_str()); WinHttpCloseHandle(request); break; }

                const std::string nextUrl = toUtf8(location);      // helper wide->utf8 ci-dessus (namespace anonyme)
                std::wstring nHost; int nPort = 443; std::wstring nPath = L"/";
                if (!splitHttpUrl(nextUrl, nHost, nPort, nPath))   // URLs absolues uniquement ; relatif -> abandon propre
                    { ReleaseInfoLog(("redirect target parse FAILED: " + nextUrl).c_str()); WinHttpCloseHandle(request); break; }

                wPath = nPath;                                     // nouveau GET vers le CDN au tour suivant
                const bool sameTarget = (nHost == wHost && nPort == port);   // même host/port : l'handle connect reste valide tel quel
                if (!sameTarget) {                                 // changement de host -> WinHttpConnect doit être re-créé sur le nouveau serveur
                    wHost = nHost; port = nPort;                   // mémorisés pour la comparaison du saut suivant éventuel
                    WinHttpCloseHandle(connect); connectClosedInLoop = true;   // fermé ici : pas de double-close en fin de bloc (le handle est remplacé ci-dessous)
                }

                HINTERNET newConnect = sameTarget ? connect
                                                  : WinHttpConnect(session, wHost.c_str(), static_cast<USHORT>(port), 0);   // nouveau serveur CDN
                if (!newConnect) {
                    ReleaseInfoLog(("WinHttpConnect FAILED on redirect target gle=" + std::to_string(GetLastError())
                        + " host=" + toUtf8(wHost)).c_str());   // échec de connexion -> abandon propre (request fermée explicitement ici avant le break)
                    WinHttpCloseHandle(request); break; }

                connect = newConnect;                                // l'ancien handle a déjà été fermé dans ce cas ; sinon même valeur
                connectClosedInLoop = false;                         // le handle courant est à nouveau ouvert : il sera refermé en fin de bloc si la boucle s'y termine
                WinHttpCloseHandle(request);                         // request du saut précédent terminée -> boucle sur le nouveau GET
            }

            if (status != 200) {
                ReleaseInfoLog(("unexpected HTTP status=" + std::to_string(status)
                    + " hop=" + std::to_string(hop)).c_str());   // 403/5xx -> échec net, pas de fichier partiel
                WinHttpCloseHandle(request); break; }

            // --- Taille totale via Content-Length (-1 si absent / chunked) --------
            long long total = -1;
            char lenBuf[32]; DWORD lenSz = sizeof(lenBuf);        // WINHTTP_QUERY_CONTENT_LENGTH (texte décimal)
            if (WinHttpQueryHeaders(request, 5u /*WINHTTP_QUERY_CONTENT_LENGTH*/, NULL, lenBuf, &lenSz, NULL)) {   // WINHTTP_HEADER_NAME_BY_INDEX
                for (DWORD k = 0; k < lenSz && lenBuf[k] >= '0' && lenBuf[k] <= '9'; ++k)
                    total = total * 10 + (lenBuf[k] - '0');       // "5348864" -> 5348864 ; reste du buffer ignoré
            }

            // --- Écriture du fichier destination ----------------------------------
            HANDLE hFile = CreateFileW(destPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL,
                                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);   // écrase un .new.exe éventuel d'un essai précédent
            if (!hFile || hFile == INVALID_HANDLE_VALUE) {
                ReleaseInfoLog(("CreateFileW FAILED gle=" + std::to_string(GetLastError())
                    + " dest=" + toUtf8(destPath)).c_str());   // verrou AV / disque plein / chemin invalide
                WinHttpCloseHandle(request); break; }

            char buf[65536];                                      // blocs de 64 Ko : ~80 écritures pour un exe de 5 Mo
            long long received = 0; bool ioOk = true, streamDone = false;
            for (;;) {
                DWORD bytes = 0;
                if (!WinHttpReadData(request, buf, sizeof(buf), &bytes)) break;   // erreur réseau -> flux interrompu
                if (bytes == 0) { streamDone = true; break; }                     // fin du corps HTTP
                if (!WriteFile(hFile, buf, bytes, NULL, NULL)) { ioOk = false; break; }   // WriteFile fichier local : tout ou rien
                received += static_cast<long long>(bytes);
                if (onProgress) onProgress(received, total);     // callback worker -> la form ne lit que des atomiques via le pont
            }

            CloseHandle(hFile);
            WinHttpCloseHandle(request); request = NULL;         // pas de double-close en fin de bloc
            ok = ioOk && streamDone && received > 0;              // flux terminé normalement + au moins 1 octet écrit
            if (ok) ReleaseInfoLog(("download OK: " + std::to_string(received)
                + " bytes -> " + toUtf8(destPath)).c_str());
            else     ReleaseInfoLog(("read loop aborted: ioOk=" + std::string(ioOk ? "1":"0")
                + " streamDone=" + std::string(streamDone?"1":"0")
                + " received=" + std::to_string(received)
                + " gle=" + std::to_string(GetLastError())).c_str());   // coupure réseau en cours de flux
            break;                                               // téléchargement achevé -> sortie définitive de la boucle (sans ça : re-GET du même URL)
        }   // boucle de redirects / lecture

        WinHttpCloseHandle(connect);                             // ferme aussi la request enfant éventuelle.
    } while (false);

    if (!ok) { ReleaseInfoLog("downloadReleaseAsset FAILED (partial file deleted)"); DeleteFileW(destPath.c_str()); }   // pas de fichier partiel laissé derrière
    WinHttpCloseHandle(session);                                  // ferme les handles enfants restants.
    return ok;
}
