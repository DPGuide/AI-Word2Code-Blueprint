#include <windows.h>
#include <commctrl.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <chrono>
#include <fstream>
#include <thread>
#include <mutex>
#include <memory>
#include <array>
#include <richedit.h>
// Externe Bibliotheken (MÜSSEN verlinkt sein: -lcurl)
#include <curl/curl.h>
#include "json.hpp" // nlohmann/json
//#include "bastion_engine.h" <------- Private because of NEXTGEN
//#include "schneider_lang.h" 
#pragma comment(lib, "comctl32.lib")
using json = nlohmann::json;
// ============================================
// Ressourcen und Konstanten
// ============================================
#define ID_EDIT_INPUT       101
#define ID_EDIT_CODE        102
#define ID_EDIT_LOG         103
#define ID_BUTTON_COMPILE   104
#define ID_TRACKBAR_REPAIR  105
#define ID_LABEL_REPAIR     106
#define ID_BUTTON_SAVE      107 
#define WM_LLM_DONE         (WM_USER + 10) 
#define ID_BUTTON_BASTION 108
#define ID_BUTTON_RUN 109
#define ID_BUTTON_OPEN      110
#define ID_MENU_BLANK   2001
#define ID_MENU_INCLUDE 2002
#define ID_MENU_DEFINE  2003
#define ID_MENU_VAR     2004
#define ID_MENU_IF      2005
#define ID_MENU_CLASS   2006
#ifndef TBS_HORZ
#define TBS_HORZ            0x0000
#define TBS_AUTOTICKS       0x0001
#define TBM_GETPOS          (WM_USER)
#define TBM_SETRANGE        (WM_USER + 6)
#define TBM_SETPOS          (WM_USER + 5)
#endif
// ============================================
// HILFSFUNKTION: Windows-Umlaute (ANSI) in KI-UTF8 übersetzen
// ============================================
std::string ANSI_to_UTF8(const std::string& ansiStr) {
    if (ansiStr.empty()) return "";
    int wSize = MultiByteToWideChar(CP_ACP, 0, ansiStr.c_str(), -1, NULL, 0);
    std::wstring wStr(wSize, 0);
    MultiByteToWideChar(CP_ACP, 0, ansiStr.c_str(), -1, &wStr[0], wSize);
    int uSize = WideCharToMultiByte(CP_UTF8, 0, wStr.c_str(), -1, NULL, 0, NULL, NULL);
    std::string utf8Str(uSize, 0);
    WideCharToMultiByte(CP_UTF8, 0, wStr.c_str(), -1, &utf8Str[0], uSize, NULL, NULL);
    while (!utf8Str.empty() && utf8Str.back() == '\0') utf8Str.pop_back();
    return utf8Str;
}
const COLORREF COLOR_BUTTON_OMNI = RGB(138, 43, 226);
const COLORREF COLOR_BG_DARK     = RGB(16, 16, 16);
const COLORREF COLOR_PANEL_BG    = RGB(30, 41, 59);
const COLORREF COLOR_EDIT_BG     = RGB(30, 41, 59);
const COLORREF COLOR_BUTTON_BG   = RGB(37, 99, 235);
const COLORREF COLOR_BUTTON_SAVE = RGB(16, 185, 129); 
const COLORREF COLOR_TEXT_WHITE  = RGB(255, 255, 255);
const COLORREF COLOR_TEXT_GRAY   = RGB(156, 163, 175);
// Kamera-Variablen (Infinite Canvas)
int g_cameraX = 0;
int g_cameraY = 0;
int g_lastMouseX = 0;
int g_lastMouseY = 0;
float g_zoom = 1.0f;
bool g_isPanning = false;
POINT g_panStart = { 0, 0 };
float g_menuSpawnX = 0;
float g_menuSpawnY = 0;
// ============================================
// ENUMS & STRUKTUREN
// ============================================
enum class ColorMode { NEUTRAL, ERROR_RED, GOLD_LEARNED };
// --- NEU: Welche Art von Baustein ist das? ---
enum class NodeType {
    INCLUDE,    // Darf nur ganz oben an den Start
    GLOBAL,     // Variablen und Defines
    FUNCTION,   // Funktionen und Klassen
    LOGIC,      // If, For, While (Code innerhalb von Funktionen)
    REROUTE     // <--- Dein Knotenpunkt! Hat keinen Code, leitet nur Kabel weiter
};
// --- UPGRADE: Das Blueprint-Kästchen ---
struct SmartNode {
    std::string id;
    std::string code;
    int x, y;
    uint8_t bitVal;
    ColorMode status;
    NodeType type; // Was bin ich?
    // NEU: Der Kabelbaum!
    // Speichert die IDs aller Kästchen, in die der Schlauch von hier aus reinfließt
    std::vector<std::string> outputs; 
    SmartNode() : x(0), y(0), bitVal(0), status(ColorMode::NEUTRAL), type(NodeType::FUNCTION) {}
};
// ==========================================
// --- DIE BLUEPRINT-REGELN (Der Türsteher) ---
// ==========================================
bool CanConnect(NodeType from, NodeType to) {
    // 1. Reroute (Knotenpunkte) sind Joker. Sie dürfen Strom von überall nehmen und überall hinleiten.
    if (from == NodeType::REROUTE || to == NodeType::REROUTE) return true;
    // 2. INCLUDE: Darf nur in andere Includes, Globals oder Funktionen fließen.
    if (from == NodeType::INCLUDE) {
        return (to == NodeType::INCLUDE || to == NodeType::GLOBAL || to == NodeType::FUNCTION);
    }
    // 3. GLOBAL (Variablen/Defines): Dürfen in andere Globals oder Funktionen fließen.
    if (from == NodeType::GLOBAL) {
        return (to == NodeType::GLOBAL || to == NodeType::FUNCTION);
    }
    // 4. FUNCTION (Klassen/Funktionen): Dürfen in andere Funktionen fließen, oder in lokale Logik.
    if (from == NodeType::FUNCTION) {
        return (to == NodeType::FUNCTION || to == NodeType::LOGIC);
    }
    // 5. LOGIC (If/While/For): Darf nur innerhalb von Logik-Ketten fließen!
    if (from == NodeType::LOGIC) {
        return (to == NodeType::LOGIC);
    }
    return false; // Alles andere wird gnadenlos blockiert!
}
// NEU: Globale Variablen für das Ziehen von Schläuchen
std::string g_draggingWireFromID = ""; // Von welchem Kästchen ziehen wir gerade?
int g_wireMouseX = 0;                  // Wo ist die Maus gerade?
int g_wireMouseY = 0;
// Strukturen für den Validator (mit FAILED statt ERROR)
struct NodeStatus { static const int OK = 0; static const int FAILED = 1; };
struct Feedback { int status; std::string title; std::string detail; };
struct ValidationResult { bool success; std::vector<Feedback> feedbacks; };
struct AiResult {
    std::string code;
    double duration; // Zeit in Sekunden
    std::string modelName;
    bool success;
};
struct ModelStats {
    int calls = 0;
    double avgTime = 0;
    long totalChars = 0;
};
std::map<std::string, ModelStats> g_leaderboard;
void UpdateLeaderboard(AiResult res) {
    auto& s = g_leaderboard[res.modelName];
    s.calls++;
    s.totalChars += res.code.length();
    // Gleitender Durchschnitt für die Zeit
    s.avgTime = (s.avgTime * (s.calls - 1) + res.duration) / s.calls;
}
// ============================================
// GLOBALE VARIABLEN
// ============================================
HINSTANCE g_hInst;
HWND g_hWndMain, g_hEditInput, g_hEditCode, g_hEditLog, g_hButton, g_hButtonSave, g_hButtonRun, g_hButtonBastion, g_hTrackbar, g_hLabelRepair;
HFONT g_hFontUI, g_hFontCode;
HWND hBar;
HWND hStatus;
std::map<std::string, SmartNode> g_allNodes;
std::string g_dragNodeID = "";
std::string g_activeNodeID = "";
bool g_isDragging = false;
POINT g_dragOffset;
// ============================================
// FUNKTIONEN (UI & Grafik)
// ============================================
void LogMessage(const std::string& msg) {
    if (!g_hEditLog) return;
    std::string currentText;
    int len = GetWindowTextLengthA(g_hEditLog);
    if (len > 0) {
        std::vector<char> buffer(len + 2);
        GetWindowTextA(g_hEditLog, buffer.data(), len + 1);
        currentText = buffer.data();
        currentText += "\r\n";
    }
    currentText += msg;
    SetWindowTextA(g_hEditLog, currentText.c_str());
    SendMessageA(g_hEditLog, EM_SETSEL, 0, -1);
    SendMessageA(g_hEditLog, EM_SETSEL, -1, -1);
    SendMessageA(g_hEditLog, EM_SCROLLCARET, 0, 0);
}
void ShowBestAI() {
    std::string out = "--- AI LEADERBOARD ---\r\n";
    for(auto const& [name, s] : g_leaderboard) {
        double cps = (s.avgTime > 0) ? (s.totalChars / s.avgTime / s.calls) : 0;
        out += name + ": " + std::to_string((int)cps) + " Zeichen/Sek (Schnitt: " + std::to_string(s.avgTime).substr(0,4) + "s)\r\n";
    }
    LogMessage(out);
}
void UpdateCodeView(const std::string& code) {
    if (g_hEditCode) SetWindowTextA(g_hEditCode, code.c_str());
}
void SaveProjectFiles() {
    std::ofstream cppFile("generated_logic.cpp");
    cppFile << "// Automatisch generiert durch Word2CodePro KI\n#include \"generated_logic.h\"\n\n";
    for (const auto& pair : g_allNodes) {
        cppFile << "// Node: " << pair.first << "\n" << pair.second.code << "\n\n";
    }
    cppFile.close();
    std::ofstream hFile("generated_logic.h");
    hFile << "// Automatisch generierter Header\n#pragma once\n#include <iostream>\n\n";
    hFile.close();
    LogMessage("[SYSTEM] Erfolg! 'generated_logic.cpp' und '.h' gespeichert.");
}
void DrawNode(HDC hdc, const SmartNode& node) {
    // 1. Position UND Größe dynamisch mit g_zoom skalieren
    int drawX = (int)((node.x + g_cameraX) * g_zoom);
    int drawY = (int)((node.y + g_cameraY) * g_zoom);
    int scaledW = (int)(150 * g_zoom);
    int scaledH = (int)(55 * g_zoom);
    COLORREF frameColor = RGB(200, 200, 200);
    if (node.status == ColorMode::ERROR_RED) frameColor = RGB(255, 0, 0);
    if (node.status == ColorMode::GOLD_LEARNED) frameColor = RGB(255, 215, 0);
	if (node.id == g_activeNodeID) frameColor = RGB(0, 255, 255);
    // Rahmendicke skaliert mit (mindestens 1 Pixel)
    int penWidth = (int)(2 * g_zoom);
    if (penWidth < 1) penWidth = 1;
    HPEN hPen = CreatePen(PS_SOLID, penWidth, frameColor);
    HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
    HBRUSH hBg = CreateSolidBrush(COLOR_PANEL_BG);
    // FIX: Wir nutzen jetzt scaledW und scaledH statt fester 150 und 55
    RECT rect = { drawX, drawY, drawX + scaledW, drawY + scaledH };
    FillRect(hdc, &rect, hBg);
    FrameRect(hdc, &rect, (HBRUSH)GetStockObject(WHITE_BRUSH));
    // 2. Die kleinen Lampen (Bits) mit-skalieren
    for (int i = 0; i < 4; i++) {
        HBRUSH hLamp = (node.bitVal > i) ? CreateSolidBrush(RGB(255, 215, 0)) : CreateSolidBrush(RGB(50, 50, 50));
        // Position und Größe der Lämpchen skalieren
        int lampX = drawX + (int)((120 + (i % 2) * 12) * g_zoom);
        int lampY = drawY + (int)((10 + (i / 2) * 12) * g_zoom);
        int lampSize = (int)(10 * g_zoom);
        if (lampSize < 2) lampSize = 2; // Mindestgröße, damit sie nicht verschwinden
        RECT r = { lampX, lampY, lampX + lampSize, lampY + lampSize };
        FillRect(hdc, &r, hLamp);
        DeleteObject(hLamp);
    }
    // 3. Den Text mit-skalieren
    int fontSize = (int)(14 * g_zoom);
    if (fontSize < 6) fontSize = 6; // Unter 6 Pixeln kann man eh nichts mehr lesen
    // Dynamisch eine passende Schriftart für die Zoom-Stufe generieren
    HFONT hScaledFont = CreateFontA(fontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    HFONT hOldFont = (HFONT)SelectObject(hdc, hScaledFont);
    SetTextColor(hdc, (node.status == ColorMode::ERROR_RED) ? RGB(255, 100, 100) : RGB(255, 255, 255));
    SetBkMode(hdc, TRANSPARENT);
    std::string info = node.id + ": " + node.code.substr(0, 15);
    // Text Position ebenfalls skalieren
    TextOutA(hdc, drawX + (int)(10 * g_zoom), drawY + (int)(30 * g_zoom), info.c_str(), (int)info.length());
    // Aufräumen (Memory Leaks verhindern!)
    SelectObject(hdc, hOldFont);
    DeleteObject(hScaledFont);
    SelectObject(hdc, hOldPen);
    DeleteObject(hPen);
    DeleteObject(hBg);
}
// ============================================
// CODE GENERATOR (Dummy-Fallback, falls KI down ist)
// ============================================
class CodeGenerator {
public:
    std::string generateFromPrompt(const std::string& prompt) {
        if (prompt.find("Hello") != std::string::npos || prompt.find("hello") != std::string::npos) {
            return R"(#include <iostream>
int main() {
    std::cout << "Hello World!" << std::endl;
    return 0;
})";
        }
        else if (prompt.find("Timer") != std::string::npos || prompt.find("timer") != std::string::npos) {
            return R"(#include <iostream>
#include <chrono>
#include <thread>
class SimpleTimer {
private:
    std::chrono::steady_clock::time_point startTime;
    bool running = false;
public:
    void start() {
        startTime = std::chrono::steady_clock::now();
        running = true;
    }
    double elapsed() const {
        if (!running) return 0.0;
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(now - startTime).count();
    }
    void stop() { running = false; }
};
int main() {
    SimpleTimer timer;
    timer.start();
    std::cout << "Timer gestartet...\n";
    return 0;
})";
        }
        // FIX: Macht Eingaben mit Zeilenumbrüchen compiler-sicher!
        std::string cleanPrompt = prompt;
        for (char& c : cleanPrompt) {
            if (c == '\n' || c == '\r' || c == '"') c = ' ';
        }
        std::string subPrompt = (cleanPrompt.length() > 30) ? cleanPrompt.substr(0, 30) : cleanPrompt;
        return "#include <iostream>\nint main() {\n    std::cout << \"Code fuer: " + subPrompt + "...\\n\";\n    return 0;\n}";
    }
};
// ============================================
// OLLAMA CLIENT & VALIDATOR (Die echte KI Logik)
// ============================================
class OllamaClient {
private:
    std::string baseUrl;
    std::mutex curlMutex;
    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
        ((std::string*)userp)->append((char*)contents, size * nmemb);
        return size * nmemb;
    }
public:
    OllamaClient(const std::string& url = "http://localhost:11434") : baseUrl(url) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }
    ~OllamaClient() {
        // curl_global_cleanup(); 
    }
    std::string generate(const std::string& model, const std::string& prompt, int timeoutSec = 600) {
        std::lock_guard<std::mutex> lock(curlMutex);
        CURL* curl = curl_easy_init();
        std::string response;
        if (!curl) return "ERROR";
        json req = { 
            {"model", model}, 
            {"prompt", prompt}, 
            {"stream", false}, 
            {"options", {{"temperature", 0.3}, {"num_predict", 4096}}} 
        };
        std::string jsonStr = req.dump();
        std::string url = baseUrl + "/api/generate";
        struct curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonStr.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSec);
        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        if (res != CURLE_OK) return "ERROR: Timeout bei curl_easy_perform";
        try {
            json j = json::parse(response);
            // NEU: Wenn Ollama einen Fehler meldet, fangen wir ihn ab!
            if (j.contains("error")) {
                return "ERROR: Ollama meldet: " + j["error"].get<std::string>();
            }
            // Wenn kein Fehler, holen wir die Antwort (oder zeigen das rohe JSON zur Diagnose)
            return j.value("response", "ERROR: Unerwartetes JSON: " + response);
            
        } catch (...) { return "ERROR: Parse Error. Ollama sagte: " + response; }
    }
    bool isServerAvailable() {
        CURL* curl = curl_easy_init();
        if (!curl) return false;
        std::string dummy;
        curl_easy_setopt(curl, CURLOPT_URL, (baseUrl + "/api/tags").c_str());
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 2L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &dummy);
        bool ok = (curl_easy_perform(curl) == CURLE_OK);
        curl_easy_cleanup(curl);
        return ok;
    }
    std::string extractCleanCode(const std::string& rawResponse) {
        size_t start = rawResponse.find("```");
        if (start == std::string::npos) return rawResponse;
        size_t codeStart = rawResponse.find("\n", start);
        if (codeStart == std::string::npos) codeStart = start + 3;
        else codeStart++;
        size_t end = rawResponse.find("```", codeStart);
        if (end == std::string::npos) return rawResponse.substr(codeStart);
        return rawResponse.substr(codeStart, end - codeStart);
    }
};
class GeminiClient {
private:
    std::string apiKey;
    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
        ((std::string*)userp)->append((char*)contents, size * nmemb);
        return size * nmemb;
    }
public:
    GeminiClient(const std::string& key) : apiKey(key) {}
    std::string generate(const std::string& prompt) {
        CURL* curl = curl_easy_init();
        if (!curl) return "ERROR: CURL_INIT_FAILED";
        std::string response;
        std::string url = "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.0-flash:generateContent?key=" + apiKey;
        // JSON Payload für Gemini
        json req = {
            {"contents", {{
                {"parts", {{
                    {"text", "Schreibe C++ Code für: " + prompt + ". NUR CODE, kein Text drumherum."}
                }}}
            }}}
        };
        std::string jsonStr = req.dump();
        struct curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonStr.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        // ========================================================
        // 🛡️ DER WINDOWS/MSYS2 HTTPS-FIX:
        // Verhindert, dass cURL die SSL-Zertifikate blockiert!
        // ========================================================
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        if (res != CURLE_OK) return "ERROR: Cloud-Server nicht erreichbar. cURL Error-Code: " + std::to_string(res);
        try {
            json j = json::parse(response);
            // 1. RÖNTGENBLICK: Hat Google einen API-Fehler geschickt?
            if (j.contains("error")) {
                return "ERROR: Gemini API meldet: " + j["error"]["message"].get<std::string>();
            }
            // 2. CHECK: Ist die Antwort leer? (Sicherheits-Filter von Google)
            if (!j.contains("candidates") || j["candidates"].empty()) {
                return "ERROR: Keine Antwort generiert. Rohes JSON von Google:\n" + response;
            }
            // 3. ERFOLG: Code auslesen
            return j["candidates"][0]["content"]["parts"][0]["text"].get<std::string>();
            
        } catch (...) { 
            // 4. TOTALAUSFALL: Wenn gar nichts mehr geht, zeig die rohe Google-Antwort
            return "ERROR: Parse Fehler. Rohe Antwort von Google:\n" + response; 
        }
    }
};
class IntelligentValidator {
public:
    ValidationResult validateCode(const std::string& sourceCode) {
        ValidationResult result;
        result.success = true;
        // NEUER SANFTER FILTER: Verhindert, dass Fehlermeldungen kompiliert werden
        if (sourceCode.find("ERROR") != std::string::npos) {
            result.success = false;
            result.feedbacks.push_back({1, "KI oder API Fehler", "Der Code enthält eine Ollama-Fehlermeldung und wird nicht kompiliert.\nDetails: " + sourceCode});
            return result; 
        }
        // 1. MANUELLE BLACKLIST-PRÜFUNG (Vorab-Check)
        std::vector<std::string> blacklist = {
            "glfw3.h", "glew.h", "glut.h", "opengl", 
            "sudo apt-get", "system(\"sudo", "system(\"apt"
        };
        for (const auto& forbidden : blacklist) {
            if (sourceCode.find(forbidden) != std::string::npos) {
                result.success = false;
                std::string msg = "VERBOTENE BIBLIOTHEK/BEFEHL GEFUNDEN: " + forbidden;
                result.feedbacks.push_back({
                    1, // NodeStatus::FAILED
                    "Sicherheits- & Kompatibilitäts-Stopp", 
                    msg + "\nBitte nutze nur Standard-Header und GDI+ für Windows!"
                });
                return result; // Sofort abbrechen und Reparatur starten
            }
        }
        // 2. COMPILER-CHECK (Dein bisheriger Code)
        std::string tempFile = ".temp_compile.cpp";
        std::ofstream out(tempFile);
        out << sourceCode;
        out.close();
        std::string cmd = "g++ -std=c++17 -fsyntax-only " + tempFile + " 2>&1";
        std::array<char, 128> buffer;
        std::string errors;
        std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
        if (pipe) {
            while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
                errors += buffer.data();
            }
        }
        if (!errors.empty()) {
            result.success = false;
            if (errors.length() > 10000) errors = errors.substr(0, 9950) + "...";
            result.feedbacks.push_back({1, "Kompilierfehler entdeckt!", errors});
        } else {
            result.feedbacks.push_back({0, "Syntax-Check bestanden", "g++ meldet keine Fehler"});
        }
        return result;
    }
};
// ============================================
// WINDOW PROCEDURE (REPARIERT)
// ============================================
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            INITCOMMONCONTROLSEX iccex = { sizeof(INITCOMMONCONTROLSEX), ICC_BAR_CLASSES };
            InitCommonControlsEx(&iccex);
            CreateWindowA("STATIC", "Word2Code KI-Interface", WS_CHILD | WS_VISIBLE | SS_LEFT, 10, 10, 200, 20, hWnd, NULL, g_hInst, NULL);
            g_hEditInput = CreateWindowExA(WS_EX_CLIENTEDGE, "RichEdit20A", "", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL,
                10, 35, 380, 100, hWnd, (HMENU)ID_EDIT_INPUT, g_hInst, NULL);
			SendMessageA(g_hEditCode, EM_EXLIMITTEXT, 0, 5000000);
            SendMessageA(g_hEditInput, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
            hBar = CreateWindowExA(0, PROGRESS_CLASS, NULL, 
                        WS_CHILD | WS_VISIBLE | PBS_MARQUEE, 
                        10, 280, 300, 20, hWnd, NULL, NULL, NULL);
            g_hTrackbar = CreateWindowExA(0, TRACKBAR_CLASS, "", WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS,
                10, 145, 200, 30, hWnd, (HMENU)ID_TRACKBAR_REPAIR, g_hInst, NULL);
            SendMessageA(g_hTrackbar, TBM_SETRANGE, TRUE, MAKELPARAM(1, 10));
            SendMessageA(g_hTrackbar, TBM_SETPOS, TRUE, 3);
            g_hLabelRepair = CreateWindowA("STATIC", "3 Max Reparatur-Versuche", WS_CHILD | WS_VISIBLE | SS_LEFT, 220, 150, 170, 20, hWnd, NULL, g_hInst, NULL);
            // REIHE 1: Programmieren, Speichern, Öffnen (jeweils 120px breit)
            g_hButton = CreateWindowA("BUTTON", "Programmieren", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW,
                10, 185, 120, 40, hWnd, (HMENU)ID_BUTTON_COMPILE, g_hInst, NULL);
            g_hButtonSave = CreateWindowA("BUTTON", "Speichern", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW,
                140, 185, 120, 40, hWnd, (HMENU)ID_BUTTON_SAVE, g_hInst, NULL);
            HWND g_hButtonOpen = CreateWindowA("BUTTON", "Datei Öffnen", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW,
                270, 185, 120, 40, hWnd, (HMENU)ID_BUTTON_OPEN, g_hInst, NULL);
            // REIHE 2: Bastion und Starten (etwas breiter)
            g_hButtonBastion = CreateWindowA("BUTTON", "BastionFS Packen", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW,
                10, 235, 185, 40, hWnd, (HMENU)ID_BUTTON_BASTION, g_hInst, NULL);
            g_hButtonRun = CreateWindowA("BUTTON", "Spiel Starten", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW,
                205, 235, 185, 40, hWnd, (HMENU)ID_BUTTON_RUN, g_hInst, NULL);
            CreateWindowA("STATIC", "System Log (Live):", WS_CHILD | WS_VISIBLE | SS_LEFT, 10, 285, 200, 20, hWnd, NULL, g_hInst, NULL);
            g_hEditLog = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "System bereit.", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                10, 305, 380, 250, hWnd, (HMENU)ID_EDIT_LOG, g_hInst, NULL);
            SendMessageA(g_hEditLog, WM_SETFONT, (WPARAM)g_hFontCode, TRUE);
            CreateWindowA("STATIC", "Generierter Code (Automatisch validiert)", WS_CHILD | WS_VISIBLE | SS_LEFT, 410, 10, 350, 20, hWnd, NULL, g_hInst, NULL);
            g_hEditCode = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "// Dein Code erscheint hier...\r\n", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_WANTRETURN,
				410, 35, 560, 250, hWnd, (HMENU)ID_EDIT_CODE, g_hInst, NULL);
            SendMessageA(g_hEditCode, WM_SETFONT, (WPARAM)g_hFontCode, TRUE);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        // ==========================================
        // --- TASTATUR-STEUERUNG (VAPORIZER) ---
        // ==========================================
        case WM_KEYDOWN: {
            if (wParam == VK_DELETE) { // Wenn ENTF gedrückt wird
                if (!g_activeNodeID.empty() && g_allNodes.count(g_activeNodeID)) {
                    LogMessage("[SYSTEM] Node vaporisiert: " + g_activeNodeID);
                    // 1. Node löschen
                    g_allNodes.erase(g_activeNodeID);
                    // 2. WICHTIG: Wenn die Maus das Kästchen noch festhält, auch loslassen!
                    if (g_dragNodeID == g_activeNodeID) g_dragNodeID = "";
                    // 3. Fokus aufheben
                    g_activeNodeID = ""; 
                    SetWindowTextA(g_hEditCode, "// Node wurde entfernt.");
                    InvalidateRect(hWnd, NULL, FALSE);
                }
            }
            return 0;
        }
        // FIX 4: Echter "Mouse-Centric" Zoom
        case WM_MOUSEWHEEL: {
            POINT pt;
            pt.x = (short)LOWORD(lParam); // Bildschirmkoordinaten
            pt.y = (short)HIWORD(lParam);
            ScreenToClient(hWnd, &pt);    // Umrechnen aufs Fenster
            int zDelta = GET_WHEEL_DELTA_WPARAM(wParam);
            if (LOWORD(wParam) & MK_CONTROL) {
                float oldZoom = g_zoom;
                if (zDelta > 0) g_zoom *= 1.1f;
                else g_zoom /= 1.1f;
                if (g_zoom < 0.1f) g_zoom = 0.1f;
                if (g_zoom > 5.0f) g_zoom = 5.0f;
                // Kamera anpassen, damit die Maus beim Zoomen der Mittelpunkt bleibt
                if (pt.x >= 400) { 
                    g_cameraX += (int)(pt.x / g_zoom - pt.x / oldZoom);
                    g_cameraY += (int)(pt.y / g_zoom - pt.y / oldZoom);
                }
            } else {
                g_cameraY += zDelta / 2; // Normales Scrollen
            }
            InvalidateRect(hWnd, NULL, FALSE);
            return 0;
        }
        case WM_LLM_DONE: { 
            // ... (Dein LLM Code bleibt unangetastet, da war alles richtig)
            bool success = (bool)wParam;
            std::string* payloadPtr = (std::string*)lParam;
            std::string codeOrError = *payloadPtr;
            delete payloadPtr;
            if (success) {
                UpdateCodeView(codeOrError);
                if (!g_activeNodeID.empty() && g_allNodes.count(g_activeNodeID)) {
                    // 1. UPDATE: Wenn ein Kästchen ausgewählt ist, überschreibe NUR dieses!
                    g_allNodes[g_activeNodeID].code = codeOrError;
                    g_allNodes[g_activeNodeID].status = ColorMode::GOLD_LEARNED; // Mach es Gold!
                    LogMessage("[ERFOLG] KI hat " + g_activeNodeID + " erfolgreich aktualisiert!");
                } else {
                // 1. WICHTIG: Den großen Editor absolut in Ruhe lassen! 
                // Dein Code bleibt sicher.
                // 2. Den Fehler stattdessen lautstark ins Log schießen
                LogMessage("=====================================");
                LogMessage("[KI-ABSTURZ] " + codeOrError);
                LogMessage("=====================================");
                // 3. Wenn du gerade ein Kästchen bearbeitest, mach es ROT!
                if (!g_activeNodeID.empty() && g_allNodes.count(g_activeNodeID)) {
                    g_allNodes[g_activeNodeID].status = ColorMode::ERROR_RED;
                    InvalidateRect(hWnd, NULL, FALSE);
                }
            }
            } else {
                LogMessage("[FEHLER] Compiler hat den Code abgelehnt! (Nodes werden nicht gezeichnet)");
                UpdateCodeView(codeOrError); 
            }
            InvalidateRect(hWnd, NULL, FALSE);
            return 0;
        }
        // FIX 2: Doppel-Case Syntaxt-Fehler beseitigt, saubere Klick-Erkennung
        case WM_RBUTTONUP: {
            int mx = LOWORD(lParam); 
            int my = HIWORD(lParam);
            if (mx > 400) { 
                // Prüfen ob wir auf eine Node geklickt haben
                bool clickedOnNode = false;
                for (auto& pair : g_allNodes) {
                    SmartNode& node = pair.second;
                    int realX = (int)((node.x + g_cameraX) * g_zoom);
                    int realY = (int)((node.y + g_cameraY) * g_zoom);
                    int realW = (int)(150 * g_zoom);
                    int realH = (int)(55 * g_zoom);
                    if (mx >= realX && mx <= realX + realW && my >= realY && my <= realY + realH) {
                        clickedOnNode = true;
                        break;
                    }
                }
                if (!clickedOnNode) {
                    // 1. Wir merken uns die Welt-Koordinaten für den Spawn!
                    g_menuSpawnX = (static_cast<float>(mx) / g_zoom) - static_cast<float>(g_cameraX);
                    g_menuSpawnY = (static_cast<float>(my) / g_zoom) - static_cast<float>(g_cameraY);
                    // 2. Wir bauen das Kontext-Menü
                    HMENU hMenu = CreatePopupMenu();
                    AppendMenuA(hMenu, MF_STRING, ID_MENU_BLANK, "[+] Leeres Modul");
                    AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
                    AppendMenuA(hMenu, MF_STRING, ID_MENU_INCLUDE, "#include Header");
                    AppendMenuA(hMenu, MF_STRING, ID_MENU_DEFINE, "#define Konstante");
                    AppendMenuA(hMenu, MF_STRING, ID_MENU_VAR, "Variable (int/string)");
                    AppendMenuA(hMenu, MF_STRING, ID_MENU_IF, "If-Bedingung");
                    AppendMenuA(hMenu, MF_STRING, ID_MENU_CLASS, "C++ Klasse");
                    // 3. Menü auf dem Bildschirm anzeigen
                    POINT pt;
                    GetCursorPos(&pt);
                    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
                    DestroyMenu(hMenu);
                }
            }
            return 0;
        }
        case WM_MBUTTONDOWN: { // Kamera verschieben
            int mx = LOWORD(lParam); int my = HIWORD(lParam);
            if (mx >= 400) {
                g_isPanning = true;
                g_panStart = { mx - g_cameraX, my - g_cameraY };
                SetCapture(hWnd);
            }
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            int mx = LOWORD(lParam); int my = HIWORD(lParam);
            if (mx < 400) return 0; // Menü ignorieren
            for (auto& pair : g_allNodes) {
                SmartNode& node = pair.second;
                int realX = (int)((node.x + g_cameraX) * g_zoom);
                int realY = (int)((node.y + g_cameraY) * g_zoom);
                int realW = (int)(150 * g_zoom);
                int realH = (int)(55 * g_zoom);
                if (mx >= realX && mx <= realX + realW && my >= realY && my <= realY + realH) {
                    g_activeNodeID = pair.first;
                    UpdateCodeView(node.code);
                    InvalidateRect(hWnd, NULL, FALSE); 
                    break;
                }
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            int mx = LOWORD(lParam); int my = HIWORD(lParam);
            if (mx < 400) return 0; // Klick im Menü ignorieren
            bool clickedNode = false;
            for (auto& pair : g_allNodes) {
                SmartNode& node = pair.second;
                int realX = (int)((node.x + g_cameraX) * g_zoom);
                int realY = (int)((node.y + g_cameraY) * g_zoom);
                int realW = (int)(150 * g_zoom);
                int realH = (int)(55 * g_zoom);
                // Haben wir ein Kästchen getroffen?
                if (mx >= realX && mx <= realX + realW && my >= realY && my <= realY + realH) {
                    clickedNode = true;
                    // NEU: Haben wir den "Stecker" (ganz rechts) getroffen?
                    if (mx >= realX + realW - 20) {
                        g_draggingWireFromID = pair.first; // Schlauch ziehen starten!
                        g_wireMouseX = mx;
                        g_wireMouseY = my;
                    } else {
                        g_dragNodeID = pair.first; // Normales Verschieben
                    }
                    g_isPanning = false;
                    g_activeNodeID = pair.first;
                    UpdateCodeView(node.code);
                    InvalidateRect(hWnd, NULL, FALSE);
                    break;
                }
            }
            if (!clickedNode) {
                g_dragNodeID = "";
                g_draggingWireFromID = "";
                g_isPanning = true;
            }
            g_lastMouseX = mx; g_lastMouseY = my;
            SetCapture(hWnd);
            return 0;
        }
        case WM_MOUSEMOVE: {
            int mx = LOWORD(lParam); int my = HIWORD(lParam);
            // AKTION A: Schlauch ziehen
            if (!g_draggingWireFromID.empty() && (wParam & MK_LBUTTON)) {
                g_wireMouseX = mx;
                g_wireMouseY = my;
                InvalidateRect(hWnd, NULL, FALSE);
            }
            // AKTION B: Kästchen verschieben
            else if (!g_dragNodeID.empty() && (wParam & MK_LBUTTON)) {
                g_allNodes[g_dragNodeID].x += (mx - g_lastMouseX) / g_zoom;
                g_allNodes[g_dragNodeID].y += (my - g_lastMouseY) / g_zoom;
                g_lastMouseX = mx; g_lastMouseY = my;
                InvalidateRect(hWnd, NULL, FALSE);
            }
            // AKTION C: Hintergrund ziehen
            else if (g_isPanning && (wParam & MK_LBUTTON)) {
                g_cameraX += (mx - g_lastMouseX) / g_zoom;
                g_cameraY += (my - g_lastMouseY) / g_zoom;
                g_lastMouseX = mx; g_lastMouseY = my;
                InvalidateRect(hWnd, NULL, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            int mx = LOWORD(lParam); int my = HIWORD(lParam);
            // Haben wir gerade einen Schlauch gezogen?
            if (!g_draggingWireFromID.empty()) {
                // Prüfen, ob wir ihn über einem anderen Kästchen losgelassen haben
                for (auto& pair : g_allNodes) {
                    SmartNode& targetNode = pair.second;
                    int realX = (int)((targetNode.x + g_cameraX) * g_zoom);
                    int realY = (int)((targetNode.y + g_cameraY) * g_zoom);
                    int realW = (int)(150 * g_zoom);
                    int realH = (int)(55 * g_zoom);
                    if (mx >= realX && mx <= realX + realW && my >= realY && my <= realY + realH) {
                        // BINGO! Schlauch einstecken! (Aber nicht in sich selbst)
                        if (g_draggingWireFromID != pair.first) {
                            // 1. Ausweise kontrollieren (Genetik abfragen)
                            NodeType srcType = g_allNodes[g_draggingWireFromID].type;
                            NodeType dstType = pair.second.type;
                            // 2. Den Türsteher fragen
                            if (CanConnect(srcType, dstType)) {
                                auto& outputs = g_allNodes[g_draggingWireFromID].outputs;
                                if (std::find(outputs.begin(), outputs.end(), pair.first) == outputs.end()) {
                                    outputs.push_back(pair.first); 
                                    LogMessage("[SYSTEM] Schlauch erfolgreich verbunden!");
                                }
                            } else {
                                // 3. VERBINDUNG VERWEIGERT!
                                LogMessage("[TÜRSTEHER] Blockiert! Du kannst diese Typen nicht verbinden.");
                                MessageBeep(MB_ICONERROR); // Macht einen schönen Windows-Fehlersound
                            }
                        }
                        break;
                    }
                }
                g_draggingWireFromID = ""; // Schlauch loslassen
            }
            if (!g_dragNodeID.empty() && g_allNodes.count(g_dragNodeID)) {
                g_allNodes[g_dragNodeID].x = (g_allNodes[g_dragNodeID].x / 8) * 8; 
                g_allNodes[g_dragNodeID].y = (g_allNodes[g_dragNodeID].y / 8) * 8;
            }
            g_isPanning = false;
            g_isDragging = false;
            g_dragNodeID = "";
            ReleaseCapture();
            InvalidateRect(hWnd, NULL, FALSE);
            return 0;
        }
        case WM_RBUTTONDOWN: { // Rechtsklick (Gold)
            int mx = LOWORD(lParam); int my = HIWORD(lParam);
            if (mx < 400) return 0;
            for (auto& pair : g_allNodes) {
                SmartNode& node = pair.second;
                int realX = (int)((node.x + g_cameraX) * g_zoom);
                int realY = (int)((node.y + g_cameraY) * g_zoom);
                // FIX: Die Hitbox muss beim Zoomen mitskalieren!
                int realW = (int)(150 * g_zoom); 
                int realH = (int)(55 * g_zoom);
                if (mx >= realX && mx <= realX + realW && my >= realY && my <= realY + realH) {
                    node.status = ColorMode::GOLD_LEARNED;
                    node.bitVal = 3;
                    LogMessage("[KI] Verbesserung angewandt! 'var' zu " + node.id + " hinzugefügt.");
                    InvalidateRect(hWnd, NULL, FALSE);
                    break;
                }
            }
            return 0;
        }
        case WM_PAINT: { // Wald, Raster & Kästchen zeichnen
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc; GetClientRect(hWnd, &rc);
            int width = rc.right; int height = rc.bottom;
            // 1. UI-Elemente ausstanzen (damit sie nicht flackern)
            HWND hChild = GetWindow(hWnd, GW_CHILD);
            while (hChild) {
                if (IsWindowVisible(hChild)) {
                    RECT childRc;
                    GetWindowRect(hChild, &childRc);
                    MapWindowPoints(NULL, hWnd, (LPPOINT)&childRc, 2);
                    ExcludeClipRect(hdc, childRc.left, childRc.top, childRc.right, childRc.bottom);
                }
                hChild = GetWindow(hChild, GW_HWNDNEXT);
            }
            // GDI+ Background
            Gdiplus::Graphics graphics(hdc); 
            graphics.Clear(Gdiplus::Color(20, 20, 20));
            // Double Buffering für flüssiges Zeichnen
            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBitmap = CreateCompatibleBitmap(hdc, width, height);
            HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, memBitmap);
            HBRUSH hBg = CreateSolidBrush(COLOR_BG_DARK);
            FillRect(memDC, &rc, hBg); DeleteObject(hBg);
            RECT forestRect = {400, height / 2 + 20, width, height};
            HBRUSH hForestBg = CreateSolidBrush(RGB(10, 10, 10));
            FillRect(memDC, &forestRect, hForestBg); DeleteObject(hForestBg);
            // Raster zeichnen
            HPEN hGridPen = CreatePen(PS_SOLID, 1, RGB(25, 25, 30));
            HPEN hOldGridPen = (HPEN)SelectObject(memDC, hGridPen);
            int startX = 400; 
            int offsetX = (int)(g_cameraX * g_zoom) % (int)(32 * g_zoom);
            int offsetY = (int)(g_cameraY * g_zoom) % (int)(32 * g_zoom);
            for (int x = startX + offsetX; x < width; x += (int)(32*g_zoom)) { MoveToEx(memDC, x, height / 2 + 20, NULL); LineTo(memDC, x, height); }
            for (int y = height / 2 + 20 + offsetY; y < height; y += (int)(32*g_zoom)) { MoveToEx(memDC, startX, y, NULL); LineTo(memDC, width, y); }
            SelectObject(memDC, hOldGridPen); DeleteObject(hGridPen);
            // ==========================================
            // --- NEUES SCHLAUCH-SYSTEM (GRAPH) ---
            // ==========================================
            HPEN hLinePen = CreatePen(PS_SOLID, (int)(3 * g_zoom) < 1 ? 1 : (int)(3 * g_zoom), RGB(100, 116, 139));
            HPEN hOldPen = (HPEN)SelectObject(memDC, hLinePen);
            // Alle echten Kabelverbindungen zeichnen
            for (const auto& pair : g_allNodes) {
                const SmartNode& source = pair.second;
                for (const std::string& targetID : source.outputs) {
                    if (g_allNodes.count(targetID)) {
                        const SmartNode& target = g_allNodes[targetID];
                        int x1 = (int)((source.x + g_cameraX + 150) * g_zoom); 
                        int y1 = (int)((source.y + g_cameraY + 27) * g_zoom); 
                        int x2 = (int)((target.x + g_cameraX) * g_zoom); 
                        int y2 = (int)((target.y + g_cameraY + 27) * g_zoom);
                        int curveOffset = (int)(60 * g_zoom);
                        POINT pts[4] = { {x1, y1}, {x1 + curveOffset, y1}, {x2 - curveOffset, y2}, {x2, y2} };
                        PolyBezier(memDC, pts, 4);
                    }
                }
            }
            // Den Live-Schlauch an der Maus zeichnen
            if (!g_draggingWireFromID.empty() && g_allNodes.count(g_draggingWireFromID)) {
                const SmartNode& source = g_allNodes[g_draggingWireFromID];
                int x1 = (int)((source.x + g_cameraX + 150) * g_zoom); 
                int y1 = (int)((source.y + g_cameraY + 27) * g_zoom);
                int x2 = g_wireMouseX;
                int y2 = g_wireMouseY;
                int curveOffset = (int)(60 * g_zoom);
                POINT pts[4] = { {x1, y1}, {x1 + curveOffset, y1}, {x2 - curveOffset, y2}, {x2, y2} };
                HPEN hLivePen = CreatePen(PS_SOLID, (int)(4 * g_zoom) < 1 ? 1 : (int)(4 * g_zoom), RGB(255, 200, 0));
                SelectObject(memDC, hLivePen);
                PolyBezier(memDC, pts, 4);
                DeleteObject(hLivePen);
                SelectObject(memDC, hLinePen); 
            }
            SelectObject(memDC, hOldPen); 
            DeleteObject(hLinePen);
            // Nodes zeichnen
            for (const auto& pair : g_allNodes) if (pair.first != g_dragNodeID) DrawNode(memDC, pair.second);
            if (!g_dragNodeID.empty() && g_allNodes.count(g_dragNodeID)) DrawNode(memDC, g_allNodes[g_dragNodeID]);
            // Auf den echten Bildschirm kopieren und aufräumen
            BitBlt(hdc, 0, 0, width, height, memDC, 0, 0, SRCCOPY);
            SelectObject(memDC, oldBitmap); DeleteObject(memBitmap); DeleteDC(memDC);
            EndPaint(hWnd, &ps);
            return 0;
        }
        case WM_DRAWITEM: {
            LPDRAWITEMSTRUCT pdis = (LPDRAWITEMSTRUCT)lParam;
            if (pdis->CtlID == ID_BUTTON_COMPILE || pdis->CtlID == ID_BUTTON_SAVE || 
                pdis->CtlID == ID_BUTTON_BASTION || pdis->CtlID == ID_BUTTON_RUN || pdis->CtlID == ID_BUTTON_OPEN) {
                COLORREF btnColor = COLOR_BUTTON_BG;
                if (pdis->CtlID == ID_BUTTON_SAVE) btnColor = COLOR_BUTTON_SAVE;
                if (pdis->CtlID == ID_BUTTON_BASTION) btnColor = COLOR_BUTTON_OMNI; 
                if (pdis->CtlID == ID_BUTTON_RUN) btnColor = RGB(220, 38, 38); // Rot
                if (pdis->CtlID == ID_BUTTON_OPEN) btnColor = RGB(245, 158, 11); // Bernstein/Orange für "Öffnen"
                HBRUSH hBrush = CreateSolidBrush(btnColor);
                FillRect(pdis->hDC, &pdis->rcItem, hBrush); DeleteObject(hBrush);
                SetTextColor(pdis->hDC, COLOR_TEXT_WHITE); SetBkMode(pdis->hDC, TRANSPARENT);
                LPCSTR text = "Programmieren";
                if (pdis->CtlID == ID_BUTTON_SAVE) text = "Speichern (.cpp/.h)";
                if (pdis->CtlID == ID_BUTTON_BASTION) text = "BastionFS";
                if (pdis->CtlID == ID_BUTTON_RUN) text = "Starten (EXE)";
                if (pdis->CtlID == ID_BUTTON_OPEN) text = "Datei Öffnen";
                DrawTextA(pdis->hDC, text, -1, &pdis->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            }
            return FALSE;
        }
        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wParam; SetTextColor(hdc, COLOR_TEXT_GRAY); SetBkColor(hdc, COLOR_BG_DARK);
            return (INT_PTR)GetStockObject(DC_BRUSH);
        }
        case WM_CTLCOLOREDIT: {
            HDC hdc = (HDC)wParam; SetTextColor(hdc, COLOR_TEXT_WHITE); SetBkColor(hdc, COLOR_EDIT_BG);
            return (INT_PTR)CreateSolidBrush(COLOR_EDIT_BG);
        }
        case WM_HSCROLL: {
            if ((HWND)lParam == g_hTrackbar) {
                int pos = (int)SendMessage(g_hTrackbar, TBM_GETPOS, 0, 0);
                std::string valStr = std::to_string(pos) + " Max";
                SetWindowTextA(g_hLabelRepair, valStr.c_str());
            }
            return 0;
        }
        case WM_COMMAND: {
            // ==========================================
            // --- 1. KONTEXT-MENÜ: NODE SPAWNER ---
            // ==========================================
            if (LOWORD(wParam) >= ID_MENU_BLANK && LOWORD(wParam) <= ID_MENU_CLASS) {
                char idBuf[32];
                snprintf(idBuf, sizeof(idBuf), "USR_%03d", (int)(g_allNodes.size() + 1));
                std::string newID = idBuf;
                SmartNode n;
                n.id = newID;
                n.x = static_cast<int>(g_menuSpawnX) - 75; 
                n.y = static_cast<int>(g_menuSpawnY) - 27; 
                n.status = ColorMode::NEUTRAL;
                n.bitVal = 3;
                switch (LOWORD(wParam)) {
                    case ID_MENU_BLANK:   n.code = "// Neues Modul\n"; n.type = NodeType::FUNCTION; break;
                    case ID_MENU_INCLUDE: n.code = "#include <iostream>\n"; n.type = NodeType::INCLUDE; break;
                    case ID_MENU_DEFINE:  n.code = "#define WERT 100\n"; n.type = NodeType::GLOBAL; break;
                    case ID_MENU_VAR:     n.code = "int meineZahl = 0;\nstd::string meinText = \"\";\n"; n.type = NodeType::GLOBAL; break;
                    case ID_MENU_IF:      n.code = "if (bedingung) {\n    // Aktion\n}\n"; n.type = NodeType::LOGIC; break;
                    case ID_MENU_CLASS:   n.code = "class MeineKlasse {\npublic:\n    MeineKlasse() {}\n};\n"; n.type = NodeType::FUNCTION; break;
                }
                g_allNodes[newID] = n;
                LogMessage("[SYSTEM] Node gespawnt: " + newID);
                InvalidateRect(hWnd, NULL, FALSE);
                return 0;
            }
            // --- KI CODE GENERATION ---
			if (LOWORD(wParam) == ID_BUTTON_COMPILE) {
                int pLevel = (int)SendMessage(g_hTrackbar, TBM_GETPOS, 0, 0);
                std::string selectedModel;
                if (pLevel <= 2)      selectedModel = "qwen2.5:1.5b";
                else if (pLevel <= 4) selectedModel = "llama3.2:1b";
                else if (pLevel <= 6) selectedModel = "phi3:mini";
                else if (pLevel <= 8) selectedModel = "gemma3:4b";
                else                  selectedModel = "gemma3:12b";
                // 1. Hole deine Anweisung (Linkes Feld)
                char buffer[4096] = {0};
                GetWindowTextA(g_hEditInput, buffer, sizeof(buffer) - 1);
                std::string userInstruction = ANSI_to_UTF8(buffer);
                // 2. NEU: Hole den Code (Rechtes großes Feld)
                int codeLen = GetWindowTextLengthA(g_hEditCode);
                std::string existingCode = "";
                if (codeLen > 0) {
                    std::vector<char> codeBuf(codeLen + 1);
                    GetWindowTextA(g_hEditCode, codeBuf.data(), codeLen + 1);
                    existingCode = ANSI_to_UTF8(codeBuf.data()); // <--- HIER AUCH!
                }
                if (!userInstruction.empty()) {
                    // 3. NEU: Wir basteln den perfekten KI-Prompt zusammen!
                    std::string safePrompt = "Hier ist der aktuelle C++ Code:\n\n" + existingCode + 
                         "\n\n--- AUFGABE ---\n" + userInstruction + 
                         "\n\nREGELN: 1. NUTZE NUR <iostream>, <vector>, <string>. "
                         "2. KEIN OpenGL/GLFW. 3. GIB DEN KOMPLETTEN CODE ZURÜCK! Behalte alle alten Funktionen bei und füge deine Änderungen nur hinzu.4. Antworte NUR mit reinem C++ Code, ohne Markdown oder Erklärungen.";
                    SendMessage(hBar, PBM_SETMARQUEE, 1, 30);
					if (pLevel == 10) {
						LogMessage("[SYSTEM] Stufe 10: Aktiviere Titanium-Cloud (Gemini)...");
					} else {
						LogMessage("[AI-CORE] Zünde " + selectedModel + " auf Stufe " + std::to_string(pLevel));
					}
					// THREAD START
					std::thread([safePrompt, hWnd, selectedModel, pLevel]() {
						std::string currentPrompt = safePrompt;
						std::string rawResponse;
						ValidationResult val;
						bool success = false;
						int maxRetries = 5; // 5 Reparatur-Versuche
						int attempt = 1;
						// DIE AUTONOME SCHLEIFE
						while (attempt <= maxRetries && !success) {
							// --- 1. KI GENERIERT CODE ---
							if (pLevel == 10) {
								GeminiClient cloud("AIzaSyAxnpiTDgTTxfqL2QDdd94C9WjrPck_zC0");
								rawResponse = cloud.generate(currentPrompt);
							} else {
								OllamaClient ai;
								if (ai.isServerAvailable()) {
									rawResponse = ai.generate(selectedModel, currentPrompt);
								} else {
									rawResponse = "// ERROR: Ollama Server nicht erreichbar!";
									break; // Abbruch bei Offline-Server
								}
							}
							// ==========================================
							// 🛡️ NEUER SCHUTZSCHILD
							// ==========================================
							if (rawResponse.find("ERROR:") != std::string::npos || rawResponse.find("ERROR") != std::string::npos) {
								success = false;
								val.success = false;
								val.feedbacks.clear(); // Alte Fehler löschen
								val.feedbacks.push_back({1, "SYSTEM ABSTURZ", rawResponse});
								break; // Verlässt die 5x-Schleife SOFORT. Kein Compiler-Wahnsinn mehr!
							}
							// --- 2. BEREINIGEN ---
							OllamaClient helper;
							rawResponse = helper.extractCleanCode(rawResponse);
							// --- 3. VALIDIEREN (g++ Check) ---
							IntelligentValidator validator;
							val = validator.validateCode(rawResponse);
							if (val.success) {
								success = true; // ERFOLG! Wir brechen aus der Schleife aus.
							} else {
								// --- 4. SELF-HEALING (Auto-Reparatur) ---
								if (attempt < maxRetries) {
									std::string errorDetail = val.feedbacks.empty() ? "Unbekannter Syntax-Fehler" : val.feedbacks[0].detail;
									// Wir überschreiben den Prompt für den nächsten Versuch!
									currentPrompt = "Der folgende C++ Code von dir hat beim Kompilieren diesen Fehler erzeugt:\n\n"
													"FEHLER:\n" + errorDetail + "\n\n"
													"FEHLERHAFTER CODE:\n" + rawResponse + "\n\n"
													"Bitte analysiere den Fehler, korrigiere den Code und antworte NUR mit dem reparierten C++ Code.";
									attempt++;
								} else {
									break;
								}
							}
						}
						std::string* resultPayload = success ? 
                            new std::string(rawResponse) : 
                            new std::string("KOMPILIERFEHLER:\r\n" + (val.feedbacks.empty() ? "Unbekannter Fehler" : val.feedbacks[0].detail));

						PostMessage(hWnd, WM_LLM_DONE, (WPARAM)success, (LPARAM)resultPayload);
					}).detach();
				}
			}
			else if (LOWORD(wParam) == ID_BUTTON_SAVE) {
                // 1. Prüfen, ob überhaupt Code da ist
                int len = GetWindowTextLengthA(g_hEditCode);
                if (len == 0) {
                    MessageBoxA(hWnd, "Das Code-Fenster ist leer! Nichts zu speichern.", "Fehler", MB_ICONWARNING | MB_OK);
                    return 0; // Abbruch
                }
                // 2. Den Windows "Speichern unter..." Dialog vorbereiten
                OPENFILENAMEA ofn;
                char szFile[260] = { 0 };
                ZeroMemory(&ofn, sizeof(ofn));
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = hWnd;
                ofn.lpstrFile = szFile;
                ofn.nMaxFile = sizeof(szFile);
                ofn.lpstrFilter = "C++ Files\0*.cpp;*.h\0Text Files\0*.txt\0All Files\0*.*\0";
                ofn.nFilterIndex = 1;
                ofn.lpstrDefExt = "cpp"; // Wenn man keine Endung eintippt, wird es automatisch .cpp
                ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
                // 3. Dialog öffnen und Speichern
                if (GetSaveFileNameA(&ofn) == TRUE) {
                    std::vector<char> buffer(len + 1);
                    GetWindowTextA(g_hEditCode, buffer.data(), len + 1);
                    std::ofstream outfile(ofn.lpstrFile);
                    if (outfile.is_open()) {
                        outfile << buffer.data();
                        outfile.close();
                        LogMessage("[SYSTEM] Datei erfolgreich gespeichert unter: " + std::string(ofn.lpstrFile));
                        // Wenn wir gerade ein Kästchen bearbeiten, machen wir es zur Feier des Tages grün!
                        if (!g_activeNodeID.empty() && g_allNodes.count(g_activeNodeID)) {
                             g_allNodes[g_activeNodeID].status = ColorMode::NEUTRAL;
                             InvalidateRect(hWnd, NULL, FALSE);
                        }
                    } else {
                        LogMessage("[FEHLER] Keine Schreibrechte! Datei konnte nicht gespeichert werden.");
                    }
                }
            }
            else if (LOWORD(wParam) == ID_BUTTON_BASTION) {
                LogMessage("[OMNI] BASTION ENGINE INITIALISIERT...");
                SmartNode bastionNode;
                bastionNode.id = "BASTION_CORE";
                bastionNode.code = "// OMNI-BACKUP: SYNCHRONIZING ETHER...\nBastionEngine::Process(84);";
                bastionNode.x = 450 - g_cameraX + 200; 
                bastionNode.y = 150 - g_cameraY;
                bastionNode.bitVal = 15; 
                bastionNode.status = ColorMode::GOLD_LEARNED;
                g_allNodes[bastionNode.id] = bastionNode;
                InvalidateRect(hWnd, NULL, FALSE);
                // 2. Deine Titanium-Engine zünden
                std::thread([]() {
                    std::string targetFile = "generated_logic.cpp";
//                    BastionEngine engine; 
//                    engine.isRunning = true;
//                    engine.Process(targetFile, 84);
                }).detach(); // <-- Thread endet hier
            }
            // --- 3. RUN ENGINE ---
            else if (LOWORD(wParam) == ID_BUTTON_RUN) {
                LogMessage("[SYSTEM] Finaler Build: Klebe Code zusammen...");
                // 1. Code aus allen Kästchen holen und in der richtigen Reihenfolge zusammensetzen
                std::string finalCode = "// --- Word2Code Pro Build ---\n\n";
                for (auto const& [id, node] : g_allNodes) {
                    finalCode += node.code + "\n\n";
                }
                // 2. In eine ECHTE Build-Datei speichern (nicht in die temporäre Datei)
                std::ofstream outfile("build_game.cpp");
                outfile << finalCode;
                outfile.close();
                LogMessage("[SYSTEM] Kompiliere build_game.cpp...");
                // 3. Kompilieren (Mit Stealth-Pipe, um echte Fehler zu sehen!)
                std::thread([]() {
                    // WICHTIG: Das "2>&1" am Ende sorgt dafür, dass wir die echten Compiler-Fehler abfangen können
                    std::string cmd = "g++ -std=c++17 build_game.cpp -o DiceGame.exe -lgdiplus -lgdi32 -lwinmm 2>&1";
                    FILE* pipe = _popen(cmd.c_str(), "r");
                    if (pipe) {
                        char buffer[256];
                        std::string compilerOutput = "";
                        while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                            compilerOutput += buffer;
                        }
                        int result = _pclose(pipe);
                        if (result == 0) {
                            LogMessage("[ERFOLG] Build komplett! Starte DiceGame.exe...");
                            system("start DiceGame.exe");
                        } else {
                            // Wenn jetzt was schiefgeht, sehen wir im Log GANZ GENAU warum!
                            LogMessage("[LINKER-FEHLER] Der Compiler sagt:\r\n" + compilerOutput);
                        }
                    }
                }).detach();
            }
            // --- DATEI ÖFFNEN (MIT AUTO-SECTION SPLITTER!) ---
            else if (LOWORD(wParam) == ID_BUTTON_OPEN) {
                OPENFILENAMEA ofn;
                char szFile[260] = { 0 };
                ZeroMemory(&ofn, sizeof(ofn));
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = hWnd;
                ofn.lpstrFile = szFile;
                ofn.nMaxFile = sizeof(szFile);
                ofn.lpstrFilter = "C++ Files\0*.cpp;*.h;*.c\0All Files\0*.*\0";
                ofn.nFilterIndex = 1;
                ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
                if (GetOpenFileNameA(&ofn) == TRUE) {
                    std::ifstream infile(ofn.lpstrFile);
                    if (infile.is_open()) {
                        std::string line;
                        std::vector<std::pair<std::string, std::string>> sections;
                        std::string currentName = "Includes_und_Globals";
                        std::string currentCode = "// Geladene Datei: " + std::string(ofn.lpstrFile) + "\n\n";
                        int braceCount = 0;
                        bool inFunction = false;
                        // ===================================================
                        // 1. DATEI INTELLIGENT ZERSCHNEIDEN (Lexer-Logik)
                        // ===================================================
                        while (std::getline(infile, line)) {
                            // Klammer-Ebenen zählen (Scope Tracking)
                            for (char c : line) {
                                if (c == '{') braceCount++;
                                if (c == '}') braceCount--;
                            }
                            // Suchen nach Funktions-Start (auf Ebene 0)
                            if (braceCount == 0 && !line.empty()) {
                                // Heuristik: Hat Klammern, aber kein Semikolon am Ende = Start einer Funktion
                                if (line.find("(") != std::string::npos && line.find(")") != std::string::npos && line.find(";") == std::string::npos) {
                                    // Vorherigen Block abspeichern (falls er Code enthält)
                                    if (currentCode.find_first_not_of(" \n\r\t") != std::string::npos) {
                                        sections.push_back({currentName, currentCode});
                                    }
                                    // Funktionsnamen extrahieren (damit das Kästchen schön benannt wird)
                                    currentName = "Function";
                                    size_t pPos = line.find("(");
                                    if (pPos != std::string::npos) {
                                        size_t sPos = line.rfind(" ", pPos - 1);
                                        currentName = (sPos != std::string::npos) ? line.substr(sPos + 1, pPos - sPos - 1) : line.substr(0, pPos);
                                    }
                                    currentCode = ""; 
                                    inFunction = true;
                                }
                            }
                            currentCode += line + "\n";
                            // Funktions-Ende erkennen
                            if (inFunction && braceCount == 0 && line.find("}") != std::string::npos) {
                                 sections.push_back({currentName, currentCode});
                                 currentName = "Globals";
                                 currentCode = "";
                                 inFunction = false;
                            }
                        }
                        // Den Rest (letzter Block) speichern
                        if (currentCode.find_first_not_of(" \n\r\t") != std::string::npos) {
                            sections.push_back({currentName, currentCode});
                        }
                        infile.close();
                        // ===================================================
                        // 2. KÄSTCHEN-KETTE ERSCHAFFEN!
                        // ===================================================
                        int startY = 50;
                        for (size_t i = 0; i < sections.size(); ++i) {
                            // WICHTIG: Nullen auffüllen (SEC_001, SEC_002), 
                            // damit die Map sie richtig sortiert und die Linien passen!
                            char idBuf[32];
                            snprintf(idBuf, sizeof(idBuf), "SEC_%03d", (int)(g_allNodes.size() + 1));
                            std::string newID = idBuf;
                            SmartNode n;
                            n.id = newID;
                            n.x = 450 - g_cameraX; 
                            n.y = startY - g_cameraY; // Jedes Kästchen 120 Pixel tiefer platzieren!
                            n.code = "// --- " + sections[i].first + " ---\n" + sections[i].second;
                            n.status = ColorMode::NEUTRAL;
                            n.bitVal = 3;
                            g_allNodes[newID] = n;
                            startY += 50; // Abstand zum nächsten Kästchen
                        }
                        // UI aktualisieren
                        SetWindowTextA(g_hEditCode, ("// Datei erfolgreich zerschnitten in " + std::to_string(sections.size()) + " Bausteine!\n// Klicke auf ein Kästchen, um den Code zu bearbeiten.").c_str());
                        LogMessage("[SYSTEM] Auto-Splitter: Datei in " + std::to_string(sections.size()) + " Nodes zerlegt!");
                        InvalidateRect(hWnd, NULL, FALSE);
                        
                    } else {
                        LogMessage("[FEHLER] Konnte die Datei nicht lesen!");
                    }
                }
            }

            return 0;
        }
        case WM_SIZE: {
            int width = LOWORD(lParam); int height = HIWORD(lParam);
            if (g_hEditInput) SetWindowPos(g_hEditInput, NULL, 10, 35, 380, 100, SWP_NOZORDER);
            if (g_hTrackbar) SetWindowPos(g_hTrackbar, NULL, 10, 145, 200, 30, SWP_NOZORDER);
            if (g_hLabelRepair) SetWindowPos(g_hLabelRepair, NULL, 220, 150, 170, 20, SWP_NOZORDER);
            // Reihe 1
            if (g_hButton) SetWindowPos(g_hButton, NULL, 10, 185, 120, 40, SWP_NOZORDER);
            if (g_hButtonSave) SetWindowPos(g_hButtonSave, NULL, 140, 185, 120, 40, SWP_NOZORDER);
            HWND hBtnOpen = GetDlgItem(hWnd, ID_BUTTON_OPEN);
            if (hBtnOpen) SetWindowPos(hBtnOpen, NULL, 270, 185, 120, 40, SWP_NOZORDER);
            // Reihe 2
            if (g_hButtonBastion) SetWindowPos(g_hButtonBastion, NULL, 10, 235, 185, 40, SWP_NOZORDER);
            if (g_hButtonRun) SetWindowPos(g_hButtonRun, NULL, 205, 235, 185, 40, SWP_NOZORDER);
            if (g_hEditLog) SetWindowPos(g_hEditLog, NULL, 10, 305, 380, height - 320, SWP_NOZORDER);
            if (g_hEditCode) SetWindowPos(g_hEditCode, NULL, 410, 35, width - 420, height / 2 - 50, SWP_NOZORDER);
            InvalidateRect(hWnd, NULL, FALSE);
            return 0;
        }
        case WM_DESTROY: { 
            PostQuitMessage(0); 
            return 0; 
        }
    }
    return DefWindowProcA(hWnd, message, wParam, lParam);
}
// ============================================
// WINMAIN
// ============================================
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    g_hInst = hInstance;
    LoadLibraryA("Riched20.dll"); 
    WNDCLASSEXA wcex = {0};
    g_hInst = hInstance;
    wcex.cbSize = sizeof(WNDCLASSEXA); wcex.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wcex.lpfnWndProc = WndProc; wcex.hInstance = hInstance;
    wcex.hIcon = LoadIcon(NULL, IDI_APPLICATION); wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH); wcex.lpszClassName = "Word2CodePro";
    if (!RegisterClassExA(&wcex)) return 1;
    g_hFontUI = CreateFontA(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    g_hFontCode = CreateFontA(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_MODERN, "Consolas");
    g_hWndMain = CreateWindowExA(0, "Word2CodePro", "Word2Code Pro - Autonomous AI Developer", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1000, 600, NULL, NULL, hInstance, NULL);
    if (!g_hWndMain) return 1;
    ShowWindow(g_hWndMain, nCmdShow); UpdateWindow(g_hWndMain);
    SendMessageA(g_hWndMain, WM_SIZE, 0, MAKELPARAM(1000, 600));
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    DeleteObject(g_hFontUI); DeleteObject(g_hFontCode);
    return (int)msg.wParam;
}