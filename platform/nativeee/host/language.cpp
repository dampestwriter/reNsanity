#include "te_language.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

#include "engine/cdvd/iso_fs.h"
#include "engine/core/config/settings_file.h"
#include "te_host.h"
#include "te_services.h"

namespace te::language {

namespace {

constexpr uint32_t kGlobalLanguagesArray = 0x002E6F00;
constexpr uint32_t kMaxSlots = 16;
constexpr size_t kProbeBytes = 2048;

const char* const kPalSlots[] = {"English", "French", "German", "Spanish", "Italian"};

int32_t CodeOfSlotName(const std::string& name) {
    if (name == "French") return 2;
    if (name == "Spanish") return 3;
    if (name == "German") return 4;
    if (name == "Italian") return 5;
    return 1;
}

std::string Lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

std::string LanguageOfSlotName(const std::string& name) {
    const std::string l = Lower(name);
    return l == "american" ? "english" : l;
}

struct Tell { const char* language; const char* const* words; size_t count; };
const char* const kEnglish[] = {"new game", "load game", "graphic options", "sound options", "game options", "widescreen"};
const char* const kPortuguese[] = {"novo jogo", "carregar jogo", "op", "idioma", "desativar", "ativar", "voltar", "continuar"};
const char* const kFrench[] = {"nouvelle partie", "charger", "langue", "options graphiques", "options sonores", "retour", "continuer"};
const char* const kGerman[] = {"neues spiel", "spiel laden", "optionen", "sprache", "grafik-optionen", "sound-optionen", "aus", "ein", "nein", "ja"};
const char* const kItalian[] = {"nuova partita", "carica partita", "opzioni", "lingua", "opzioni grafiche", "opzioni audio", "indietro", "continua"};
const char* const kSpanish[] = {"nuevo juego", "nueva partida", "cargar", "opciones", "idioma", "opciones gr", "atr", "volver", "continuar"};
const char* const kDutch[] = {"nieuw spel", "spel laden", "opties", "taal", "terug", "verder", "uit", "aan", "nee"};
const Tell kTells[] = {
    {"english", kEnglish, sizeof kEnglish / sizeof *kEnglish},
    {"portuguese", kPortuguese, sizeof kPortuguese / sizeof *kPortuguese},
    {"french", kFrench, sizeof kFrench / sizeof *kFrench},
    {"german", kGerman, sizeof kGerman / sizeof *kGerman},
    {"italian", kItalian, sizeof kItalian / sizeof *kItalian},
    {"spanish", kSpanish, sizeof kSpanish / sizeof *kSpanish},
    {"dutch", kDutch, sizeof kDutch / sizeof *kDutch},
};

std::vector<std::string> Lines(const std::string& text, size_t at_most) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        if (c == '\r' || c == '\n') {
            if (!cur.empty()) { out.push_back(Lower(cur)); cur.clear(); if (out.size() >= at_most) break; }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty() && out.size() < at_most) out.push_back(Lower(cur));
    return out;
}

Table g_table;
bool g_resolved = false;

std::vector<std::string> SlotNamesFromImage() {
    std::vector<std::string> names;
    for (uint32_t i = 0; i < kMaxSlots; ++i) {
        const uint8_t* p = GuestPointer(kGlobalLanguagesArray + i * 4, 4);
        if (!p) break;
        uint32_t ptr = 0;
        std::memcpy(&ptr, p, 4);
        if (ptr == 0) break;
        const char* s = GuestString(ptr, 32);
        if (!s || !*s) break;

        bool letters = true;
        for (const char* c = s; *c; ++c) letters = letters && ((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z'));
        if (!letters) break;
        names.emplace_back(s);
    }
    return names;
}

const Bank* BankOfSlot(const std::vector<Bank>& banks, int32_t slot) {
    for (const Bank& b : banks) if (int32_t(b.slot) == slot) return &b;
    return nullptr;
}

Language RawLanguage(const std::vector<Bank>& banks, int32_t slot, const std::string& why) {
    Language l;
    l.raw = true;
    l.text_slot = l.voice_slot = slot;
    const Bank* b = BankOfSlot(banks, slot);
    l.name = b ? b->content : "slot " + std::to_string(slot);
    l.text_language = l.name;
    l.voice_language = b ? b->speech_language : "";
    l.label = "slot " + std::to_string(slot) + (b ? " " + Lower(b->name) : "") + " (" + why + ")";
    return l;
}

void Select(Table& t) {
    const Settings& s = GetSettings();
    t.requested = Lower(s.language_name.empty() ? "english" : s.language_name);
    t.selected = -1;
    if (t.banks.empty()) return;

    if (s.language >= 0 && s.language_name.empty()) {
        int32_t slot = 0;
        for (const Bank& b : t.banks) if (b.code == s.language && b.code != 1) slot = int32_t(b.slot);
        t.languages.push_back(RawLanguage(t.banks, slot, "console code " + std::to_string(s.language)));
        t.selected = int32_t(t.languages.size()) - 1;
        return;
    }

    for (size_t i = 0; i < t.languages.size(); ++i)
        if (t.languages[i].name == t.requested) { t.selected = int32_t(i); return; }

    for (size_t i = 0; i < t.languages.size(); ++i)
        if (t.languages[i].name == "english") { t.selected = int32_t(i); return; }
    if (!t.languages.empty()) t.selected = 0;
}

}

std::string RecognizeContent(const std::string& text, std::string* probe) {
    const std::vector<std::string> lines = Lines(text, 40);
    if (probe) *probe = lines.size() > 9 ? lines[9] : "";
    const char* best = nullptr;
    int best_score = 0;
    for (const Tell& tell : kTells) {
        int score = 0;
        for (const std::string& line : lines)
            for (size_t w = 0; w < tell.count; ++w)
                if (line == tell.words[w] || (std::strlen(tell.words[w]) >= 6 && line.rfind(tell.words[w], 0) == 0)) { ++score; break; }
        if (score > best_score) { best_score = score; best = tell.language; }
    }

    return best && best_score >= 2 ? best : "";
}

bool EnumerateFromDisc(const std::string& iso_path, const std::vector<std::string>& slot_names_in, std::vector<Bank>& out,
                       std::string* error) {
    out.clear();
    std::vector<std::string> slot_names = slot_names_in;
    if (slot_names.empty()) slot_names.assign(kPalSlots, kPalSlots + sizeof kPalSlots / sizeof *kPalSlots);
    IsoFs iso;
    if (!iso.Open(iso_path, error)) return false;
    IsoFs::Entry bh, bd;
    if (!iso.Find("CRASH6/CRASH.BH", bh) || !iso.Find("CRASH6/CRASH.BD", bd) || bh.size < 8 || bh.size > (1u << 20)) {
        if (error) *error = "CRASH6/CRASH.BH or CRASH.BD not on the disc";
        return false;
    }
    std::vector<uint8_t> index(bh.size);
    if (iso.ReadBytes(uint64_t(bh.lba) * IsoFs::kSectorSize, bh.size, index.data()) != bh.size) {
        if (error) *error = "CRASH.BH did not read whole";
        return false;
    }

    struct Hit { uint32_t offset, size; };
    std::vector<std::pair<std::string, Hit>> code_banks;
    size_t at = 4;
    while (at + 4 <= index.size()) {
        uint32_t len = 0;
        std::memcpy(&len, &index[at], 4);
        at += 4;
        if (len > 512 || at + len + 8 > index.size()) break;
        std::string name(reinterpret_cast<const char*>(&index[at]), len);
        at += len;
        Hit h;
        std::memcpy(&h.offset, &index[at], 4);
        std::memcpy(&h.size, &index[at + 4], 4);
        at += 8;
        const std::string prefix = "Language\\Code\\";
        if (name.size() > prefix.size() + 4 && name.compare(0, prefix.size(), prefix) == 0 && name.compare(name.size() - 4, 4, ".txt") == 0)
            code_banks.emplace_back(name.substr(prefix.size(), name.size() - prefix.size() - 4), h);
    }
    for (uint32_t slot = 0; slot < slot_names.size(); ++slot) {
        const std::string& name = slot_names[slot];
        auto it = std::find_if(code_banks.begin(), code_banks.end(), [&](const auto& b) { return Lower(b.first) == Lower(name); });
        if (it == code_banks.end()) continue;
        Bank bank;
        bank.slot = slot;
        bank.name = name;
        bank.code = CodeOfSlotName(name);
        const size_t want = std::min<size_t>(kProbeBytes, it->second.size);
        std::string text(want, '\0');
        const size_t got = iso.ReadBytes(uint64_t(bd.lba) * IsoFs::kSectorSize + it->second.offset, want, reinterpret_cast<uint8_t*>(&text[0]));
        text.resize(got);
        const std::string content = RecognizeContent(text, &bank.probe);
        bank.recognised = !content.empty();
        bank.content = bank.recognised ? content : Lower(name);

        IsoFs::Entry mb;
        bank.speech = iso.Find("CRASH6/" + name + ".MB", mb) && !mb.directory && mb.size > 0;
        bank.speech_language = LanguageOfSlotName(name);
        out.push_back(bank);
    }
    if (out.empty() && error) *error = "no Language\\Code\\<slot>.txt bank in CRASH.BH for any slot the executable names";
    return !out.empty();
}

std::vector<Language> LanguagesOf(const std::vector<Bank>& banks) {
    std::vector<Language> out;
    const bool any_speech = std::any_of(banks.begin(), banks.end(), [](const Bank& b) { return b.speech; });
    for (const Bank& text : banks) {

        if (std::any_of(out.begin(), out.end(), [&](const Language& l) { return l.name == text.content; })) continue;
        Language l;
        l.name = text.content;
        l.text_slot = int32_t(text.slot);
        const Bank* voice = nullptr;
        if (any_speech) {

            for (const Bank& b : banks) if (b.speech && b.speech_language == text.content) { voice = &b; break; }
            if (!voice) for (const Bank& b : banks) if (b.speech && b.speech_language == "english") { voice = &b; break; }
        }
        if (!voice) voice = &text;
        l.voice_slot = int32_t(voice->slot);
        l.voice_language = voice->speech_language;
        l.text_language = l.name;
        l.label = l.name + (l.voice_language != l.name ? " (" + l.voice_language + " voice)" : "");
        out.push_back(l);
    }

    std::stable_partition(out.begin(), out.end(), [](const Language& l) { return l.name == "english"; });

    int32_t english_text_slot = -1;
    for (const Language& l : out) if (l.name == "english") english_text_slot = l.text_slot;
    for (const Bank& speech : banks) {
        if (!speech.speech) continue;
        if (std::any_of(out.begin(), out.end(), [&](const Language& l) { return l.name == speech.speech_language; })) continue;
        Language l;
        l.name = speech.speech_language;
        l.voice_slot = int32_t(speech.slot);
        l.voice_language = speech.speech_language;
        l.text_slot = english_text_slot >= 0 ? english_text_slot : int32_t(speech.slot);
        l.text_language = english_text_slot >= 0 ? "english" : speech.content;
        l.label = l.name + " (" + l.text_language + " text)";
        out.push_back(l);
    }
    return out;
}

const Table& Resolve() {
    if (g_resolved) return g_table;
    g_resolved = true;
    g_table = Table{};
    const Settings& s = GetSettings();
    if (!s.iso_path.empty()) {
        std::vector<std::string> names = SlotNamesFromImage();
        if (names.empty()) names.assign(kPalSlots, kPalSlots + sizeof kPalSlots / sizeof *kPalSlots);
        std::string error;
        EnumerateFromDisc(s.iso_path, names, g_table.banks, &error);
    }
    g_table.languages = LanguagesOf(g_table.banks);
    Select(g_table);
    return g_table;
}

namespace {
const Language* Current() {
    const Table& t = Resolve();
    return t.selected >= 0 && size_t(t.selected) < t.languages.size() ? &t.languages[size_t(t.selected)] : nullptr;
}
}

int32_t ConsoleLanguage() {
    if (const Language* l = Current()) {
        const Bank* b = BankOfSlot(Resolve().banks, l->voice_slot);
        return b ? b->code : 1;
    }

    const int32_t raw = GetSettings().language;
    return raw >= 0 && raw <= 7 ? raw : 1;
}

int32_t TextBank() {
    const Language* l = Current();
    return l ? l->text_slot : -1;
}

uint32_t Count() {
    const Table& t = Resolve();
    uint32_t n = 0;
    for (const Language& l : t.languages) if (!l.raw) ++n;
    return n;
}

int32_t Selected() {
    const Table& t = Resolve();
    return t.selected;
}

std::string Label(uint32_t language) {
    const Table& t = Resolve();
    return language < t.languages.size() ? t.languages[language].label : "";
}

bool Select(uint32_t index) {
    Table& t = const_cast<Table&>(Resolve());
    if (index >= t.languages.size()) return false;
    t.selected = int32_t(index);
    const Language& l = t.languages[index];
    Settings s = GetSettings();
    s.language = -1;
    s.language_name = l.name;
    SetSettings(s);
    t.requested = l.name;
    if (!s.settings_path.empty()) {
        std::string err;
        ps2r::SettingsFile::Set(s.settings_path, "game.language", l.name, &err);
    }
    return true;
}

}
