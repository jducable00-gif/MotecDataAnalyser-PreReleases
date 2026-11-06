//---------------------------------------------------------------------------
// ReleaseInfo.h — Version de l'app + release notes GitHub (header-only).
//
// Deux sources d'information, combinées dans la barre de statut / boîte À propos :
//   1. kAppVersion : constante compilée ici (bumpée par scripts/release.ps1 à
//      chaque tag v5.x) — toujours disponible, même hors-ligne.
//   2. Release notes du dernier GitHub release, récupérées via WinHTTP sur
//      api.github.com/repos/<owner>/<repo>/releases/latest :
//        - fetch ASYNCHRONE (std::thread) pour ne jamais bloquer l'UI ;
//        - repo PRIVÉ sans token : WinHTTP délègue les credentials Windows de
//          l'utilisateur connecté (le compte doit avoir accès au dépôt — c'est le
//          cas en pratique puisque git push fonctionne déjà avec eux) ;
//        - résultat mis en cache par la form dans MotecDataAnalyser.ini
//          ([ReleaseInfo]) : hors-ligne, les dernières notes restent affichées.
//
// Dépendances : winhttp.lib (pragma comment), JsonParser de RuleConfig.h pour le
// parse du JSON GitHub — aucun autre ajout au projet.
//---------------------------------------------------------------------------

#ifndef RELEASEINFO_H
#define RELEASEINFO_H

#include <winsock2.h>   // avant windows.h (ordre d'inclusion standard du projet)
#include <windows.h>    // DWORD/BOOL/LPCWSTR ; HINTERNET et INTERNET_PORT y sont deja tires

// NB : on n'inclut PAS <wininet.h>/<winhttp.h>. Dans le contexte VCL, ces headers
// entrent en conflit avec Winapi.WinInet.hpp (PCH) — redefinition de typedefs SDK
// (URL_COMPONENTSW / INTERNET_SCHEME), bug connu d'Embarcadero. Les 9 fonctions
// utilisees sont declarees manuellement ci-dessous : signatures __stdcall stables,
// liees via winhttp.lib sans jamais charger les headers.
typedef LPVOID HINTERNET;   // identique au typedef du SDK (wininet.h) — redefinition legale si present

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#pragma comment(lib, "winhttp")   // link winhttp.lib (pas besoin du header)

// --- Constantes WinHTTP/WinInet (valeurs extraites des headers SDK Embarcadero) --
#ifndef INTERNET_DEFAULT_HTTPS_PORT
#define INTERNET_DEFAULT_HTTPS_PORT 443u
#endif
#ifndef WINHTTP_ACCESS_TYPE_DEFAULT_PROXY
#define WINHTTP_ACCESS_TYPE_DEFAULT_PROXY 0u
#endif
#ifndef WINHTTP_FLAG_SECURE
#define WINHTTP_FLAG_SECURE 0x800000u   // HTTPS/SSL
#endif

// --- Prototypes WinHTTP minimaux (signatures du SDK winhttp.h) ------------------
extern "C" {
    HINTERNET WINAPI WinHttpOpen(LPCWSTR lpszUserAgent, DWORD dwAccessType,
                                 LPCWSTR lpszProxyName, LPCWSTR lpszProxyBypass, DWORD dwFlags);
    HINTERNET WINAPI WinHttpConnect(HINTERNET hSession, LPCWSTR lpServerName,
                                    USHORT nServerPort, DWORD dwReserved);   // INTERNET_PORT = unsigned short
    HINTERNET WINAPI WinHttpOpenRequest(HINTERNET hConnect, LPCWSTR lpszVerb, LPCWSTR lpszUrlPath,
                                        LPCWSTR lpszReferrer, LPVOID lpszAcceptTypes,
                                        const LPCWSTR* pLpszAcceptTypes, DWORD dwFlags);
    BOOL      WINAPI WinHttpSetTimeouts(HINTERNET hInternetHandle, DWORD nResolveTimeout,
                                        DWORD nConnectTimeout, DWORD nSendTimeout, DWORD nReceiveTimeout);
    BOOL      WINAPI WinHttpSendRequest(HINTERNET hRequest, LPCWSTR lpszHeaders, DWORD dwHeaderLength,
                                        LPVOID lpOptional, DWORD dwOptionalLength,
                                        DWORD dwTotalLength, DWORD_PTR dwContext);
    BOOL      WINAPI WinHttpReceiveResponse(HINTERNET hRequest, LPVOID lpReserved);
    BOOL      WINAPI WinHttpQueryHeaders(HINTERNET hHandle, DWORD dwInfoLevel, LPCWSTR pwszName,
                                         LPVOID lpBuffer, LPDWORD lpdwBufferLength, LPDWORD pdwIndex);
    BOOL      WINAPI WinHttpReadData(HINTERNET hRequest, LPVOID lpBuffer,
                                     DWORD dwNumberOfBytesToRead, LPDWORD lpdwNumberOfBytesRead);
    BOOL      WINAPI WinHttpCloseHandle(HINTERNET hInternet);
}

// Niveaux de requete (winhttp.h) : WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER.
#define RELEASEINFO_QRY_STATUS  (19u | 0x20000000u)

// Version affichée dans la barre de statut. Bumpée par scripts/release.ps1 lors
// de chaque release (le script met aussi à jour ce fichier et le commit).
static const char* kAppVersion = "V5.6.20";

// Diagnostic self-update : append-only log texte UTF-8 dans <dirExe>\MotecDataAnalyser.update.log.
// Thread-safe (mutex interne) ; utilisé par downloadReleaseAsset + startSelfUpdate pour tracer
// chaque point d'échec avec GetLastError() — indispensable car le chemin de téléchargement est
// sinon 100% silencieux (aucun retour utilisateur si WinHttpSendRequest/ReceiveResponse/CreateFileW/
// MoveFileExW/CreateProcessW échoue).
void ReleaseInfoLog(const char* msg);

struct AppRelease {
    std::string tagName;      // ex: "v5.6"
    std::string htmlUrl;      // page GitHub du release (lien cliquable)
    std::string publishedAt;  // ISO-8601 brut ("2026-10-24T...Z") — formaté à l'affichage
    std::string body;         // notes de version markdown, UTF-8
    std::string assetUrl;     // browser_download_url du binaire .exe ("" si absent)
    std::string mappingUrl;   // raw.githubusercontent.com URL de MotecDataAnalyser.mapping.json ("" si dérivable impossible)

    bool empty() const { return tagName.empty(); }
};

// ===============================
// Comparaison de versions.
// compareVersions("V5.6", "v5.7") -> -1 : insensible au préfixe v/V et aux
// majuscules, comparaison numérique segment par segment (5.10 > 5.9).
// Retourne -1 si a < b, 0 si égaux, +1 si a > b ; segments manquants = 0.
// ===============================
int compareVersions(const std::string& a, const std::string& b);

// true si le tag GitHub est strictement plus récent que kAppVersion compilée.
bool isUpdateAvailable(const AppRelease& release);

// ===============================
// Parse du JSON "latest release" (GitHub API).
// Retourne false si le document est invalide ou ne contient pas tag_name.
// Testé par la section J du harness test_comparison_engine.cpp.
// ===============================
bool parseLatestReleaseJson(const std::string& json, AppRelease& out);

// Formatte "2026-10-24T18:30:00Z" en date lisible localisée :
//   FR -> "24 oct. 2026", EN -> "Oct 24, 2026". Date invalide -> texte d'origine.
std::string formatReleaseDate(const std::string& isoUtc, bool french);

// ===============================
// Fetch WinHTTP (synchron — à appeler depuis un thread worker).
// token vide = credentials Windows du compte connecté (repo privé).
// Retourne true si une release a été trouvée et parsée.
// ===============================
bool fetchLatestReleaseSync(const std::string& owner, const std::string& repo,
                            AppRelease& out, int timeoutMs = 8000);

// Version asynchrone multi-repos (même owner) : un thread détaché par dépôt, en
// parallèle ; quand TOUS ont terminé, on garde la meilleure version installable
// (tag strictement supérieur à kAppVersion + asset .exe présent). Un repo sans
// release ne bloque pas les autres. Exemple d'usage : testeur pre-release ->
// {kPreReleaseRepo, kStableRepo} pour voir aussi les versions stables ; user
// stable -> {kStableRepo}. Ecrit dans l'état global du module (jamais de VCL) ->
// safe même si la form est détruite pendant le fetch. La form récupère via takePendingRelease().
void fetchLatestReleasesAsync(const std::string& owner, const std::vector<std::string>& repos);

// Récupération du dernier release produit par les workers depuis l'appel précédent :
// true + out rempli si une nouvelle donnée est disponible (sinon false).
bool takePendingRelease(AppRelease& out);

// ===============================
// Anti-boucle self-update. Quand WinMain detecte "/updated" dans la ligne de commande,
// c'est que CETTE instance vient d'etre installee par le self-update : son kAppVersion
// compile est encore l ancienne (le bump n arrive qu a la prochaine release), donc sans
// ce flag elle re-detecterait "nouvelle version", re-proposerait le popup et tenterait un
// 2e swap sur son propre chemin .new.exe -> CreateProcess gle=2. WinMain appelle
// markJustUpdated() ; promptForAvailableUpdate() doit alors rester muet pour la session.
void markJustUpdated();

// true si cette instance a ete relancee via /updated (self-update termine) : le popup de mise a jour est interdit.
bool wasJustUpdated();


// ===============================
// Téléchargement d'un asset de release (self-update) — thread worker uniquement.
// onProgress(bytesReçus, totalBytes; total = -1 si inconnu) peut être vide ; il est
// appelé depuis le THREAD WORKER : ne jamais y toucher à du VCL (atomiques seulement).
// Retourne true si le fichier a été écrit intégralement (HTTP 200 + lecture complète).
// ===============================
bool downloadReleaseAsset(const std::string& url, const std::wstring& destPath,
                          const std::function<void(long long receivedBytes, long long totalBytes)> onProgress = nullptr);

#endif // RELEASEINFO_H
