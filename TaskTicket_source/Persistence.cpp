#include "TaskTicket.h"
#include <shlobj.h>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cctype>

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Ole32.lib")

//===========================================================================
// Path helpers
//===========================================================================

std::wstring GetAppDataDirectory()
{
    PWSTR pathTmp = nullptr;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &pathTmp);
    if (FAILED(hr) || pathTmp == nullptr)
    {
        if (pathTmp) CoTaskMemFree(pathTmp);
        return L"";
    }

    std::wstring dir = pathTmp;
    CoTaskMemFree(pathTmp);
    dir += L"\\TaskTicket";

    if (!CreateDirectoryW(dir.c_str(), nullptr))
    {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS)
        {
            return L"";
        }
    }
    return dir;
}

std::wstring GetTasksFilePath()
{
    std::wstring dir = GetAppDataDirectory();
    if (dir.empty()) return L"";
    return dir + L"\\tasks.json";
}

std::wstring GetTodayDateString()
{
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[16];
    swprintf_s(buf, L"%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
    return buf;
}

//===========================================================================
// UTF-8 <-> UTF-16 helpers
//===========================================================================

static std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), out.data(), len, nullptr, nullptr);
    return out;
}

static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), len);
    return out;
}

//===========================================================================
// Minimal JSON writer (schema is fixed and small, so a hand-rolled writer
// keeps the app dependency-free and tiny, per the "no third-party
// frameworks unless truly necessary" design goal).
//===========================================================================

static void AppendEscapedJsonString(std::string& out, const std::string& raw)
{
    out += '"';
    for (unsigned char c : raw)
    {
        switch (c)
        {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (c < 0x20)
            {
                char buf[8];
                sprintf_s(buf, "\\u%04x", c);
                out += buf;
            }
            else
            {
                out += (char)c;
            }
        }
    }
    out += '"';
}

bool SaveState(const PersistedState& state)
{
    std::wstring finalPath = GetTasksFilePath();
    if (finalPath.empty()) return false;

    std::string json;
    json.reserve(256 + state.tasks.size() * 96);
    json += "{\n";

    json += "  \"lastDate\": ";
    AppendEscapedJsonString(json, WideToUtf8(state.lastDate));
    json += ",\n";

    json += "  \"windowX\": " + std::to_string(state.windowX) + ",\n";
    json += "  \"windowY\": " + std::to_string(state.windowY) + ",\n";
    json += "  \"nextId\": " + std::to_string(state.nextId) + ",\n";

    json += "  \"tasks\": [\n";
    for (size_t i = 0; i < state.tasks.size(); ++i)
    {
        const Task& t = state.tasks[i];
        json += "    {\n";
        json += "      \"id\": " + std::to_string(t.id) + ",\n";
        json += "      \"text\": ";
        AppendEscapedJsonString(json, WideToUtf8(t.text));
        json += ",\n";
        json += std::string("      \"checked\": ") + (t.checked ? "true" : "false") + ",\n";
        json += std::string("      \"recurring\": ") + (t.recurring ? "true" : "false") + "\n";
        json += "    }";
        if (i + 1 < state.tasks.size()) json += ",";
        json += "\n";
    }
    json += "  ]\n";
    json += "}\n";

    // Atomic-ish write: write to a temp file first, flush, then replace the
    // real file. This means a crash or power loss mid-write can never
    // leave tasks.json half-written / corrupted.
    std::wstring tmpPath = finalPath + L".tmp";

    HANDLE hFile = CreateFileW(tmpPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    BOOL ok = WriteFile(hFile, json.data(), (DWORD)json.size(), &written, nullptr);
    if (ok) FlushFileBuffers(hFile);
    CloseHandle(hFile);

    if (!ok || written != json.size())
    {
        DeleteFileW(tmpPath.c_str());
        return false;
    }

    if (!MoveFileExW(tmpPath.c_str(), finalPath.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        DeleteFileW(tmpPath.c_str());
        return false;
    }

    return true;
}

//===========================================================================
// Minimal JSON reader - just enough to parse the specific shape we write
// above. Any malformed input throws std::runtime_error, which LoadState()
// catches and turns into "start fresh with defaults" behaviour.
//===========================================================================

namespace
{
    struct JsonCursor
    {
        const std::string& s;
        size_t pos = 0;

        explicit JsonCursor(const std::string& str) : s(str) {}

        char Peek() const
        {
            if (pos >= s.size()) throw std::runtime_error("Unexpected end of JSON");
            return s[pos];
        }

        char Next()
        {
            char c = Peek();
            ++pos;
            return c;
        }

        void SkipWhitespace()
        {
            while (pos < s.size() &&
                   (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r'))
            {
                ++pos;
            }
        }

        void Expect(char c)
        {
            SkipWhitespace();
            if (Peek() != c)
            {
                throw std::runtime_error("Expected character in JSON");
            }
            ++pos;
        }

        bool TryConsume(char c)
        {
            SkipWhitespace();
            if (pos < s.size() && s[pos] == c)
            {
                ++pos;
                return true;
            }
            return false;
        }

        std::string ParseString()
        {
            SkipWhitespace();
            Expect('"');
            std::string out;
            while (true)
            {
                char c = Next();
                if (c == '"') break;
                if (c == '\\')
                {
                    char esc = Next();
                    switch (esc)
                    {
                    case '"':  out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/'; break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case 'u':
                    {
                        if (pos + 4 > s.size()) throw std::runtime_error("Bad \\u escape");
                        std::string hex = s.substr(pos, 4);
                        pos += 4;
                        unsigned int cp = (unsigned int)strtoul(hex.c_str(), nullptr, 16);
                        // Re-encode this one code point back to UTF-8. (Full
                        // surrogate-pair handling isn't needed here because
                        // our own writer never emits \u escapes above 0x20,
                        // but we handle the common BMP case defensively.)
                        if (cp <= 0x7F)
                        {
                            out += (char)cp;
                        }
                        else if (cp <= 0x7FF)
                        {
                            out += (char)(0xC0 | (cp >> 6));
                            out += (char)(0x80 | (cp & 0x3F));
                        }
                        else
                        {
                            out += (char)(0xE0 | (cp >> 12));
                            out += (char)(0x80 | ((cp >> 6) & 0x3F));
                            out += (char)(0x80 | (cp & 0x3F));
                        }
                        break;
                    }
                    default:
                        throw std::runtime_error("Bad escape in JSON string");
                    }
                }
                else
                {
                    out += c;
                }
            }
            return out;
        }

        long long ParseInt()
        {
            SkipWhitespace();
            size_t start = pos;
            if (pos < s.size() && (s[pos] == '-' || s[pos] == '+')) ++pos;
            while (pos < s.size() && isdigit((unsigned char)s[pos])) ++pos;
            if (pos == start) throw std::runtime_error("Expected number in JSON");
            return strtoll(s.substr(start, pos - start).c_str(), nullptr, 10);
        }

        bool ParseBool()
        {
            SkipWhitespace();
            if (s.compare(pos, 4, "true") == 0) { pos += 4; return true; }
            if (s.compare(pos, 5, "false") == 0) { pos += 5; return false; }
            throw std::runtime_error("Expected boolean in JSON");
        }

        // Skips over any JSON value we don't care about (used for forward
        // compatibility, in case future versions add fields).
        void SkipValue()
        {
            SkipWhitespace();
            char c = Peek();
            if (c == '"') { ParseString(); return; }
            if (c == '{')
            {
                ++pos;
                SkipWhitespace();
                if (TryConsume('}')) return;
                while (true)
                {
                    ParseString();
                    Expect(':');
                    SkipValue();
                    SkipWhitespace();
                    if (TryConsume(',')) continue;
                    Expect('}');
                    break;
                }
                return;
            }
            if (c == '[')
            {
                ++pos;
                SkipWhitespace();
                if (TryConsume(']')) return;
                while (true)
                {
                    SkipValue();
                    SkipWhitespace();
                    if (TryConsume(',')) continue;
                    Expect(']');
                    break;
                }
                return;
            }
            if (c == 't' || c == 'f') { ParseBool(); return; }
            if (c == '-' || isdigit((unsigned char)c)) { ParseInt(); return; }
            throw std::runtime_error("Unexpected token in JSON");
        }
    };

    Task ParseTaskObject(JsonCursor& c)
    {
        Task t;
        c.Expect('{');
        c.SkipWhitespace();
        if (c.TryConsume('}')) return t; // empty object - keep defaults

        while (true)
        {
            std::string key = c.ParseString();
            c.Expect(':');

            if (key == "id")             t.id = (int)c.ParseInt();
            else if (key == "text")      t.text = Utf8ToWide(c.ParseString());
            else if (key == "checked")   t.checked = c.ParseBool();
            else if (key == "recurring") t.recurring = c.ParseBool();
            else                          c.SkipValue();

            c.SkipWhitespace();
            if (c.TryConsume(',')) continue;
            c.Expect('}');
            break;
        }
        return t;
    }

    PersistedState ParseRoot(const std::string& content)
    {
        PersistedState state;
        JsonCursor c(content);
        c.Expect('{');
        c.SkipWhitespace();
        if (c.TryConsume('}')) return state;

        while (true)
        {
            std::string key = c.ParseString();
            c.Expect(':');

            if (key == "lastDate")
            {
                state.lastDate = Utf8ToWide(c.ParseString());
            }
            else if (key == "windowX")
            {
                state.windowX = (int)c.ParseInt();
            }
            else if (key == "windowY")
            {
                state.windowY = (int)c.ParseInt();
            }
            else if (key == "nextId")
            {
                state.nextId = (int)c.ParseInt();
            }
            else if (key == "tasks")
            {
                c.SkipWhitespace();
                c.Expect('[');
                c.SkipWhitespace();
                if (!c.TryConsume(']'))
                {
                    while (true)
                    {
                        state.tasks.push_back(ParseTaskObject(c));
                        c.SkipWhitespace();
                        if (c.TryConsume(',')) continue;
                        c.Expect(']');
                        break;
                    }
                }
            }
            else
            {
                c.SkipValue();
            }

            c.SkipWhitespace();
            if (c.TryConsume(',')) continue;
            c.Expect('}');
            break;
        }
        return state;
    }
} // namespace

PersistedState LoadState()
{
    PersistedState defaultState;
    defaultState.lastDate = GetTodayDateString();

    std::wstring path = GetTasksFilePath();
    if (path.empty()) return defaultState;

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return defaultState; // first run - no file yet

    std::ostringstream ss;
    ss << file.rdbuf();
    std::string content = ss.str();
    file.close();

    if (content.empty()) return defaultState;

    try
    {
        PersistedState loaded = ParseRoot(content);
        if (loaded.lastDate.empty()) loaded.lastDate = GetTodayDateString();

        // Make sure nextId is always ahead of every loaded task id, in case
        // an older/foreign file didn't track it correctly.
        int maxId = 0;
        for (const Task& t : loaded.tasks) maxId = std::max(maxId, t.id);
        if (loaded.nextId <= maxId) loaded.nextId = maxId + 1;

        return loaded;
    }
    catch (const std::exception&)
    {
        // Corrupted file - don't crash, don't wipe the file (the user can
        // still recover it manually); just start this session fresh.
        return defaultState;
    }
}
