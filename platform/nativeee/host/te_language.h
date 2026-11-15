#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace te::language {

struct Bank {
    uint32_t slot = 0;
    std::string name;
    std::string content;

    bool recognised = false;
    std::string probe;
    int32_t code = 1;
    bool speech = false;
    std::string speech_language;
};

struct Language {
    std::string name;

    int32_t text_slot = 0;
    int32_t voice_slot = 0;
    std::string voice_language;
    std::string text_language;
    std::string label;

    bool raw = false;
};

struct Table {
    std::vector<Bank> banks;
    std::vector<Language> languages;

    std::string requested;
    int32_t selected = -1;
};

const Table& Resolve();


int32_t ConsoleLanguage();

int32_t TextBank();

uint32_t Count();
int32_t Selected();
std::string Label(uint32_t language);

bool Select(uint32_t index);

std::string RecognizeContent(const std::string& text, std::string* probe);

bool EnumerateFromDisc(const std::string& iso_path, const std::vector<std::string>& slot_names, std::vector<Bank>& out,
                       std::string* error);

std::vector<Language> LanguagesOf(const std::vector<Bank>& banks);

}
