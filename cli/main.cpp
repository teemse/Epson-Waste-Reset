#include <iostream>
#include <algorithm>
#include <string>
#include <cctype>
#include <filesystem>
#include "ewr/payload.h"
#include "ewr/parser.h"
#include "ewr/session.h"
#include "ewr/usb.h"
#include "ewr/deviceid.h"
#include "ewr/generator.h"
#include "ewr/status.h"
#include "ewr/updater.h"
#include "ewr/version.h"
#include "ewr/log.h"
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <ctime>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <climits>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

namespace fs = std::filesystem;

struct MenuOption
{
    std::string displayName;
    bool isReplay;
    ewr::PrinterModel replayModel;
    ewr::DbPrinterModel smartModel;
};

std::string toLower(std::string str) {
    std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) { return std::tolower(c); });
    return str;
}

struct CliOptions
{
    bool statusOnly = false;     // --status: только чтение статуса и счётчиков
    bool listOnly = false;       // --list: обзор интерфейсов, затем выход
    bool dryRun = false;         // --dry-run: всё, кроме записи
    bool dump = false;           // --dump: дамп EEPROM в файл (только чтение)
    bool noUpdate = false;       // --no-update: офлайн-режим, без проверки обновлений
    int interfaceCandidate = 0;  // --interface <n>: выбор интерфейса (1-based, 0 = авто)
    bool usbSoftReset = false;   // --usb-soft-reset: очистка канала при каждом открытии
    std::string modelOverride;   // --model <name>: пропустить меню
};

static void SetWorkingDirectoryToExecutable()
{
#ifdef _WIN32
    char path[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (len > 0 && len < MAX_PATH)
    {
        std::error_code ec;
        fs::current_path(fs::path(path).parent_path(), ec);
    }
#elif defined(__APPLE__)
    char path[PATH_MAX];
    uint32_t size = static_cast<uint32_t>(sizeof(path));
    if (_NSGetExecutablePath(path, &size) == 0)
    {
        std::error_code ec;
        const fs::path exe = fs::canonical(fs::path(path), ec);
        fs::current_path((ec ? fs::path(path) : exe).parent_path(), ec);
    }
#else
    char path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len > 0)
    {
        path[len] = '\0';
        std::error_code ec;
        fs::current_path(fs::path(path).parent_path(), ec);
    }
#endif
}

const std::string kEwrCurrentVersion = EWR_VERSION;

const std::string kReleasesPageUrl = "https://github.com/RxNaison/Epson-Waste-Reset/releases";

static void PrintUsage()
{
    std::cout << "EWR - Сброс отработки Epson " << kEwrCurrentVersion << "\n\n"
              << "Использование: ewr [опции]\n\n"
              << "Запуск без опций — рекомендуемый путь: EWR найдёт принтер,\n"
              << "покажет его статус и счётчики, спросит один раз, сбросит и проверит.\n\n"
              << "Опции:\n"
              << "  --status, -s     Только чтение: статус принтера, уровни чернил\n"
              << "                   и значения счётчиков отработки. Запись не выполняется.\n"
              << "  --list, -l       Список всех USB-интерфейсов Epson с их IEEE 1284\n"
              << "                   device ID и совпадением в базе, затем выход. Только чтение.\n"
              << "  --model <name>   Пропустить меню и использовать эту модель из базы.\n"
              << "                   Принимает точное имя, алиас или уникальную часть\n"
              << "                   (например, --model ET-2803).\n"
              << "  --interface <n>  Привязать весь запуск к интерфейсу <n> из --list\n"
              << "                   и отключить автоматический fallback. Для составных\n"
              << "                   устройств, где определяется не тот интерфейс.\n"
              << "  --dry-run        Определить, прочитать статус и счётчики, показать\n"
              << "                   что именно будет записано — затем остановиться. Без записи.\n"
              << "  --dump           Определить, выбрать модель, затем прочитать EEPROM\n"
              << "                   в файл с меткой времени рядом с ewr. Только чтение.\n"
              << "                   Сделайте дамп дважды вокруг изменения и сравните файлы,\n"
              << "                   чтобы картировать неизвестный принтер.\n"
              << "  --no-update      Полностью офлайн: без проверки обновлений, без загрузки,\n"
              << "                   без подмены базы при выходе. Для тестирования локальных\n"
              << "                   правок базы перед pull request.\n"
              << "  --usb-soft-reset Очистка USB-канала при каждом открытии сессии (только\n"
              << "                   Windows). По умолчанию выключено: на ET-2xxx может\n"
              << "                   заблокировать следующую запись. Диагностический ключ.\n"
              << "  --help, -h       Показать эту справку.\n";
}

static std::string GaugeBar(int percent)
{
    if (percent < 0)
        percent = 0;
    if (percent > 100)
        percent = 100;

    const int filled = (percent + 9) / 10;

    std::string bar = "[";
    for (int i = 0; i < 10; ++i)
        bar += (i < filled) ? '#' : '-';
    bar += "]";
    return bar;
}

static void PrintPrinterStatus(const ewr::PrinterStatus& st)
{
    if (!st.valid)
    {
        std::cout << "[i] Принтер ответил, но статус не удалось разобрать." << std::endl;
        return;
    }

    std::cout << "\n----------- СТАТУС ПРИНТЕРА -----------" << std::endl;
    std::cout << "  Состояние: " << ewr::DescribePrinterCondition(st) << std::endl;

    if (!st.serial.empty())
        std::cout << "  Серийный номер: " << st.serial << std::endl;

    if (!st.inks.empty())
    {
        std::cout << "  Уровни чернил:" << std::endl;
        for (const auto& ink : st.inks)
        {
            char line[96];
            if (ink.level >= 0)
                snprintf(line, sizeof(line), "    %-14.14s %s %3d%%  %s",
                         ink.colorName.c_str(), GaugeBar(ink.level).c_str(),
                         ink.level, ink.statusText.c_str());
            else
                snprintf(line, sizeof(line), "    %-14.14s %s",
                         ink.colorName.c_str(), ink.statusText.c_str());
            std::cout << line << std::endl;
        }
    }

    if (st.maintenanceBoxLevel >= 0 || !st.maintenanceBoxText.empty())
    {
        char line[96];
        if (st.maintenanceBoxLevel >= 0)
            snprintf(line, sizeof(line), "    %-14.14s %s %3d%%  %s", "Сервис. бокс",
                     GaugeBar(st.maintenanceBoxLevel).c_str(),
                     st.maintenanceBoxLevel, st.maintenanceBoxText.c_str());
        else
            snprintf(line, sizeof(line), "    %-14.14s %s", "Сервис. бокс",
                     st.maintenanceBoxText.c_str());
        std::cout << line << std::endl;
    }

    std::cout << "--------------------------------------" << std::endl;
}

static void PrintCounterValues(const std::vector<std::pair<uint16_t, int>>& values, const char* label)
{
    if (values.empty())
        return;

    std::cout << "  " << label << std::endl;
    for (const auto& entry : values)
    {
        char line[64];
        if (entry.second >= 0)
            snprintf(line, sizeof(line), "    EEPROM 0x%04X = 0x%02X (%d)", entry.first, entry.second, entry.second);
        else
            snprintf(line, sizeof(line), "    EEPROM 0x%04X = (нет ответа)", entry.first);
        std::cout << line << std::endl;
    }
}

static void PrintCounterSummary(const ewr::DbPrinterModel& model,
                                const std::vector<std::pair<uint16_t, int>>& values)
{
    const auto specs = model.GetAllCounters();
    if (specs.empty() || values.empty())
        return;

    bool printedHeader = false;
    for (const auto& spec : specs)
    {
        const ewr::CounterReading reading = ewr::EvaluateCounter(spec, values);
        if (!reading.complete)
            continue;

        if (!printedHeader)
        {
            std::cout << "  Заполнение впитывающей прокладки:" << std::endl;
            printedHeader = true;
        }

        const std::string name = reading.description.empty() ? "Счётчик" : reading.description;
        const int percent = reading.Percent();

        char line[160];
        if (percent >= 0)
        {
            snprintf(line, sizeof(line), "    %-28.28s %s %3d%%  (%u / %u)%s",
                     name.c_str(), GaugeBar(percent).c_str(), percent,
                     static_cast<unsigned>(reading.value),
                     static_cast<unsigned>(reading.max_value),
                     percent >= 100 ? "  <-- достигнут лимит обслуживания" : "");
        }
        else
        {
            snprintf(line, sizeof(line), "    %-28.28s значение %u (нет лимита обслуживания в базе)",
                     name.c_str(), static_cast<unsigned>(reading.value));
        }
        std::cout << line << std::endl;
    }
}

static bool g_exitPause = true;

static int FinishRun(int exitCode)
{
    if (g_exitPause)
    {
        std::cout << "\nНажмите Enter для выхода..." << std::endl;
        std::cin.get();
    }

    ewr::BackgroundUpdater::Instance().ApplyStagedUpdatesOnExit();

    return exitCode;
}

static int FinishReset(bool resetOk)
{
    if (resetOk)
    {
        std::cout << "\n========================================" << std::endl;
        std::cout << " УСПЕХ! Выключите принтер, затем включите." << std::endl;
        std::cout << "========================================" << std::endl;
    }
    else
    {
        std::cerr << "\n========================================" << std::endl;
        std::cerr << " СБРОС НЕ УДАЛСЯ. Смотрите сообщения выше и" << std::endl;
        std::cerr << " ewr_trace.log для деталей." << std::endl;
        std::cerr << "========================================" << std::endl;
    }

    return FinishRun(resetOk ? 0 : 1);
}

static void PrintInkSummary(const ewr::DbPrinterModel& model,
                            const std::vector<std::pair<uint16_t, int>>& values)
{
    if (model.ink_groups.empty())
        return;

    std::cout << "  Потребление чернил картриджей (0 = полный, 100 = пустой):" << std::endl;
    for (const auto& group : model.ink_groups)
    {
        if (group.addresses.empty())
            continue;

        int used = -1;
        for (const auto& v : values)
        {
            if (v.first == group.addresses[0])
            {
                used = v.second;
                break;
            }
        }

        std::cout << "    " << (group.color.empty() ? std::string("(без имени)") : group.color) << ": ";
        if (used >= 0)
            std::cout << used << std::endl;
        else
            std::cout << "(нет ответа)" << std::endl;
    }
}

static std::string WriteEepromDump(const ewr::DbPrinterModel& model,
                                   const std::vector<std::pair<uint16_t, int>>& values)
{
    std::vector<std::pair<uint16_t, std::string>> notes;
    for (const auto& g : model.pad_groups)
    {
        const std::string label = g.description.empty() ? std::string("счётчик отработки") : g.description;
        for (uint16_t a : g.addresses)
            notes.push_back({ a, label });
    }
    for (const auto& g : model.ink_groups)
    {
        for (uint16_t a : g.addresses)
            notes.push_back({ a, "чернила картриджа: " + (g.color.empty() ? std::string("?") : g.color) });
    }

    auto noteFor = [&notes](uint16_t addr) -> std::string {
        for (const auto& n : notes)
        {
            if (n.first == addr)
                return n.second;
        }
        return "";
    };

    std::string safeName;
    for (char c : model.name)
        safeName += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';

    const std::string base = "ewr_dump_" + safeName + "_" +
        std::to_string(static_cast<long long>(std::time(nullptr)));

    std::string filename = base + ".txt";
    for (int n = 2; fs::exists(filename) && n < 100; ++n)
        filename = base + "_" + std::to_string(n) + ".txt";

    std::ofstream out(filename);
    if (!out)
        return "";

    out << "# Дамп EEPROM EWR\n";
    out << "# модель: " << model.name << "\n";
    out << "# адрес,значение,примечание ('--' = нет ответа)\n";
    for (const auto& v : values)
    {
        char line[24];
        if (v.second < 0)
            snprintf(line, sizeof(line), "0x%02X,--", v.first);
        else
            snprintf(line, sizeof(line), "0x%02X,0x%02X", v.first, v.second & 0xFF);

        out << line << "," << noteFor(v.first) << "\n";
    }

    return filename;
}

int main(int argc, char* argv[])
{
    SetWorkingDirectoryToExecutable();

    ewr::log::Default().AddSink(ewr::log::ConsoleSink(std::cout, std::cerr));

    CliOptions cli;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--status" || arg == "-s")
        {
            cli.statusOnly = true;
        }
        else if (arg == "--list" || arg == "-l")
        {
            cli.listOnly = true;
        }
        else if (arg == "--dry-run")
        {
            cli.dryRun = true;
        }
        else if (arg == "--dump")
        {
            cli.dump = true;
        }
        else if (arg == "--no-update")
        {
            cli.noUpdate = true;
        }
        else if (arg == "--usb-soft-reset")
        {
            cli.usbSoftReset = true;
        }
        else if (arg == "--model" || arg == "--interface")
        {
            if (i + 1 >= argc)
            {
                std::cerr << "[!] " << arg << " требует значение (см. --help)." << std::endl;
                return 2;
            }

            const std::string value = argv[++i];
            if (arg == "--model")
            {
                cli.modelOverride = value;
            }
            else
            {
                try { cli.interfaceCandidate = std::stoi(value); }
                catch (...) { cli.interfaceCandidate = 0; }

                if (cli.interfaceCandidate < 1)
                {
                    std::cerr << "[!] --interface требует номер из --list (1, 2, ...)." << std::endl;
                    return 2;
                }
            }
        }
        else if (arg == "--help" || arg == "-h")
        {
            PrintUsage();
            return 0;
        }
        else
        {
            std::cerr << "[!] Неизвестная опция: " << arg << " (см. --help)." << std::endl;
            return 2;
        }
    }

    const bool statusOnly = cli.statusOnly;
    g_exitPause = !(cli.statusOnly || cli.listOnly || cli.dryRun || cli.dump);

    std::cout << "========================================" << std::endl;
    std::cout << "       EWR - Сброс отработки Epson      " << std::endl;
    std::cout << "       Версия " << kEwrCurrentVersion << std::endl;
    std::cout << "========================================\n" << std::endl;

    if (statusOnly)
        std::cout << "[i] РЕЖИМ ТОЛЬКО ЧТЕНИЯ: запись в EEPROM не будет выполнена.\n" << std::endl;
    else if (cli.dryRun)
        std::cout << "[i] ПРОБНЫЙ ЗАПУСК: EWR определит, прочитает и покажет план, но ничего не запишет.\n" << std::endl;
    else if (cli.dump)
        std::cout << "[i] РЕЖИМ ДАМПА: чтение EEPROM в файл, запись не выполняется.\n" << std::endl;

    ewr::CleanupStaleTempFiles();

    if (cli.listOnly)
    {
        ewr::UniversalGenerator listGenerator;
        std::vector<ewr::ModelNameEntry> listEntries;
        if (listGenerator.LoadDatabase("database.json"))
        {
            for (const auto& m : listGenerator.GetAvailableModels())
                listEntries.push_back({ m.name, m.aliases });
        }

        std::cout << "[*] Сканирование USB-интерфейсов Epson (только чтение)..." << std::endl;
        ewr::UsbDeviceGateway listGateway;
        const std::vector<ewr::InterfaceInfo> interfaces = listGateway.ListInterfaces();

        if (interfaces.empty())
        {
            std::cout << "\nUSB-интерфейсы Epson не найдены. Принтер включён и подключён?" << std::endl;
            return 1;
        }

        std::cout << "\nОбнаруженные USB-интерфейсы Epson (в порядке автоматического fallback):" << std::endl;
        for (const auto& iface : interfaces)
        {
            std::cout << "\n  [" << iface.index << "] " << iface.className;
            if (iface.interfaceNumber >= 0)
                std::cout << " mi_" << (iface.interfaceNumber < 10 ? "0" : "") << iface.interfaceNumber;
            std::cout << "\n      " << iface.path << std::endl;

            if (iface.deviceId.empty())
            {
                std::cout << "      IEEE 1284 device ID: (нет ответа)" << std::endl;
                continue;
            }

            const ewr::DeviceIdInfo devId = ewr::ParseIeee1284DeviceId(iface.deviceId);
            std::cout << "      IEEE 1284 device ID: " << (devId.model.empty() ? iface.deviceId : "MDL \"" + devId.model + "\"") << std::endl;

            if (!devId.model.empty() && !listEntries.empty())
            {
                const std::vector<std::string> matches = ewr::MatchModelEntries(devId.model, listEntries);
                if (!matches.empty())
                    std::cout << "      Запись в базе:       " << matches[0] << std::endl;
            }
        }

        std::cout << "\nИспользуйте --interface <n> для привязки к конкретному интерфейсу." << std::endl;
        return 0;
    }

    if (!cli.noUpdate && !fs::exists("database.json"))
    {
        std::cout << "[i] Загрузка базы данных принтеров... ";
        std::cout.flush();
        if (ewr::Updater::SyncDatabaseNow("database.json"))
            std::cout << "УСПЕХ.\n" << std::endl;
        else
            std::cout << "ОШИБКА.\n" << std::endl;
    }

    if (cli.noUpdate)
    {
        std::cout << "[i] --no-update: офлайн-запуск, database.json не будет изменена." << std::endl;
    }
    else
    {
        std::cout << "[i] Проверка обновлений... " << std::flush;
        const ewr::UpdateMetadata update = ewr::Updater::CheckLatestRelease(kEwrCurrentVersion);
        if (update.updateAvailable)
            std::cout << "доступна версия " << update.latestVersion << "!\n"
                      << "    Скачайте на " << kReleasesPageUrl << std::endl;
        else if (update.latestVersion.empty())
            std::cout << "не удалось связаться с GitHub." << std::endl;
        else
            std::cout << "у вас актуальная версия." << std::endl;

        ewr::BackgroundUpdater::Instance().StartAsync(ewr::kMaxSupportedDatabaseSchema);
    }

    ewr::UniversalGenerator generator;
    if (!generator.LoadDatabase("database.json"))
        std::cerr << "[!] Не удалось загрузить database.json (отсутствует или повреждена). Модели Smart Protocol недоступны в этом запуске." << std::endl;

    auto replayModels = ewr::ScanModelsFolder("models");
    auto smartModels = generator.GetAvailableModels();

    if (replayModels.empty() && smartModels.empty())
    {
        std::cerr << "\n[!] Полезные данные не найдены: database.json отсутствует или не читается." << std::endl;
        std::cerr << "    Она должна быть рядом с ewr — распакуйте архив заново или запустите с интернетом для загрузки." << std::endl;
        return FinishRun(1);
    }

    std::vector<MenuOption> options;
    size_t hiddenModels = 0;

    for (const auto& sm : smartModels)
    {
        if (!sm.HasResettableCounters() && !sm.HasInkReset())
        {
            hiddenModels++;
            continue;
        }
        options.push_back({ sm.name + " (Smart Protocol - Рекомендуется)", false, {}, sm });
    }

    std::cout << "[i] Загружено " << (smartModels.size() - hiddenModels) << " моделей Smart Protocol." << std::endl;

    if (hiddenModels > 0)
        std::cout << "[i] " << hiddenModels << " записей базы не имеют сбрасываемых счётчиков и скрыты." << std::endl;

    std::cout << "[i] Загружено " << replayModels.size() << " пользовательских моделей." << std::endl;

    for (const auto& lm : replayModels)
        options.push_back({ lm.name + " (Replay)", true, lm, {} });

    std::sort(options.begin(), options.end(), [](const MenuOption& a, const MenuOption& b)
        {
            const std::string& an = a.isReplay ? a.replayModel.name : a.smartModel.name;
            const std::string& bn = b.isReplay ? b.replayModel.name : b.smartModel.name;
            if (an != bn)
                return an < bn;

            return !a.isReplay && b.isReplay;
        });

    ewr::UsbDeviceGateway gateway;

    std::string detectedMdl;
    std::string detectedMatch;

    std::cout << "\n[i] Определение подключённого принтера... " << std::flush;
    const std::vector<ewr::InterfaceInfo> interfaces = gateway.ListInterfaces();

    if (cli.interfaceCandidate >= 1 && cli.interfaceCandidate > static_cast<int>(interfaces.size()))
    {
        std::cout << "найдено " << interfaces.size() << " интерфейс(ов)." << std::endl;
        std::cerr << "[!] --interface " << cli.interfaceCandidate << " не существует: доступно только "
                  << interfaces.size() << " интерфейс(ов) Epson. Запустите 'ewr --list' для просмотра." << std::endl;
        return FinishRun(1);
    }

    ewr::DeviceIdQueryResult devIdQuery;
    for (const auto& iface : interfaces)
    {
        if (cli.interfaceCandidate >= 1 && iface.index != cli.interfaceCandidate)
            continue;

        if (!iface.deviceId.empty())
        {
            devIdQuery.found = true;
            devIdQuery.deviceId = iface.deviceId;
            break;
        }
    }

    if (devIdQuery.found)
    {
        const ewr::DeviceIdInfo devId = ewr::ParseIeee1284DeviceId(devIdQuery.deviceId);
        detectedMdl = devId.model;

        if (!detectedMdl.empty())
        {
            std::cout << "найдено \"" << detectedMdl << "\"." << std::endl;

            std::vector<ewr::ModelNameEntry> smartEntries;
            for (const auto& opt : options)
            {
                if (!opt.isReplay)
                    smartEntries.push_back({ opt.smartModel.name, opt.smartModel.aliases });
            }

            const std::vector<std::string> matches = ewr::MatchModelEntries(detectedMdl, smartEntries);
            if (!matches.empty())
            {
                detectedMatch = matches[0];
                std::cout << "[i] Совпадение в базе: " << detectedMatch << std::endl;
            }
            else
            {
                std::cout << "[!] Нет записи в базе для \"" << detectedMdl << "\" — выберите модель вручную." << std::endl;
            }
        }
        else
        {
            std::cout << "устройство ответило, но не сообщило имя модели." << std::endl;
        }
    }
    else
    {
        std::cout << "нет ответа (принтер выключен, отключён или ограничение драйвера)." << std::endl;
    }

    if (interfaces.size() > 1)
    {
        std::cout << "[i] Обнаружено " << interfaces.size() << " USB-интерфейсов Epson"
                  << (cli.interfaceCandidate >= 1 ? " (привязано через --interface):" : " (пробуются в этом порядке):") << std::endl;
        for (const auto& iface : interfaces)
        {
            std::cout << "      [" << iface.index << "] " << iface.className;
            if (iface.interfaceNumber >= 0)
                std::cout << " mi_" << (iface.interfaceNumber < 10 ? "0" : "") << iface.interfaceNumber;

            if (!iface.deviceId.empty())
            {
                const std::string bannerMdl = ewr::ParseIeee1284DeviceId(iface.deviceId).model;
                if (!bannerMdl.empty())
                    std::cout << " - \"" << bannerMdl << "\"";
                else
                    std::cout << " - ответил на запрос device ID";
            }
            else
            {
                std::cout << " - нет ответа на device ID";
            }

            if (cli.interfaceCandidate == iface.index)
                std::cout << "   <-- привязан";

            std::cout << std::endl;
        }
    }

    MenuOption selected;
    bool hasSelected = false;

    if (!cli.modelOverride.empty())
    {
        const std::string wantedLower = toLower(cli.modelOverride);

        std::string resolvedName;
        for (const auto& opt : options)
        {
            const std::string ownName = opt.isReplay ? opt.replayModel.name : opt.smartModel.name;
            if (toLower(ownName) == wantedLower)
            {
                resolvedName = ownName;
                break;
            }

            if (!opt.isReplay)
            {
                for (const auto& alias : opt.smartModel.aliases)
                {
                    if (toLower(alias) == wantedLower)
                    {
                        resolvedName = opt.smartModel.name;
                        break;
                    }
                }
            }

            if (!resolvedName.empty())
                break;
        }

        if (resolvedName.empty())
        {
            std::vector<ewr::ModelNameEntry> allEntries;
            for (const auto& opt : options)
            {
                if (opt.isReplay)
                    allEntries.push_back({ opt.replayModel.name, {} });
                else
                    allEntries.push_back({ opt.smartModel.name, opt.smartModel.aliases });
            }

            const std::vector<std::string> matches = ewr::MatchModelEntries(cli.modelOverride, allEntries);

            std::vector<std::string> uniqueNames;
            for (const auto& match : matches)
            {
                bool seen = false;
                for (const auto& name : uniqueNames)
                    seen = seen || (name == match);
                if (!seen)
                    uniqueNames.push_back(match);
            }

            if (uniqueNames.empty())
            {
                std::cerr << "[!] --model \"" << cli.modelOverride << "\" не соответствует ни одной доступной модели." << std::endl;
                std::cerr << "    (Модели базы без сбрасываемых счётчиков не предлагаются.)" << std::endl;
                std::cerr << "    Запустите без --model и используйте поиск в меню или проверьте 'ewr --list'." << std::endl;
                return FinishRun(1);
            }

            if (uniqueNames.size() > 1)
            {
                std::cerr << "[!] --model \"" << cli.modelOverride << "\" неоднозначен. Ближайшие совпадения:" << std::endl;
                for (size_t i = 0; i < uniqueNames.size() && i < 6; ++i)
                    std::cerr << "      " << uniqueNames[i] << std::endl;
                std::cerr << "    Уточните запрос — полное имя всегда работает." << std::endl;
                return FinishRun(1);
            }

            resolvedName = uniqueNames[0];
        }

        for (const auto& opt : options)
        {
            if (!opt.isReplay && opt.smartModel.name == resolvedName)
            {
                selected = opt;
                hasSelected = true;
                break;
            }
        }

        if (!hasSelected)
        {
            for (const auto& opt : options)
            {
                if (opt.isReplay && opt.replayModel.name == resolvedName)
                {
                    selected = opt;
                    hasSelected = true;
                    break;
                }
            }
        }

        if (hasSelected)
            std::cout << "\n[i] --model: используется " << selected.displayName << "." << std::endl;
    }

    while (!hasSelected)
    {
        if (!detectedMatch.empty())
            std::cout << "\nНажмите Enter для использования определённой модели [" << detectedMatch
                      << "], введите другую модель для поиска или 'exit' для выхода: ";
        else
            std::cout << "\nВведите модель принтера для поиска (например, 'L3150' или 'XP') или 'exit' для выхода: ";

        std::string searchQuery;
        if (!std::getline(std::cin, searchQuery))
        {
            std::cerr << "\n[ОШИБКА] Нет доступного ввода: EWR нужна модель для работы, а stdin\n"
                         "        закрыт. Передайте --model <имя> для неинтерактивного выбора." << std::endl;
            return FinishRun(1);
        }

        if (searchQuery.empty())
        {
            if (detectedMatch.empty())
                continue;

            for (const auto& opt : options)
            {
                if (!opt.isReplay && opt.smartModel.name == detectedMatch)
                {
                    selected = opt;
                    hasSelected = true;
                    break;
                }
            }
            continue;
        }

        std::string searchLower = toLower(searchQuery);
        if (searchLower == "exit" || searchLower == "quit")
            return FinishRun(0);

        std::vector<MenuOption> filteredOptions;
        for (const auto& opt : options)
        {
            if (toLower(opt.displayName).find(searchLower) != std::string::npos)
                filteredOptions.push_back(opt);
        }

        if (filteredOptions.empty())
        {
            std::cout << "[-] Принтеры по запросу '" << searchQuery << "' не найдены. Попробуйте снова.\n";
            continue;
        }

        std::cout << "\nНайдено " << filteredOptions.size() << " подходящих принтеров:\n";

        for (size_t i = 0; i < filteredOptions.size(); ++i)
            std::cout << "[" << i + 1 << "] " << filteredOptions[i].displayName << "\n";

        std::cout << "[0] Искать снова...\n";

        std::cout << "\nВыберите принтер [0-" << filteredOptions.size() << "]: ";
        std::string choiceStr;
        std::getline(std::cin, choiceStr);

        try
        {
            int choice = std::stoi(choiceStr);
            if (choice == 0)
            {
                continue;
            }
            else if (choice >= 1 && choice <= static_cast<int>(filteredOptions.size()))
            {
                selected = filteredOptions[choice - 1];
                hasSelected = true;
            }
            else
            {
                std::cout << "[-] Неверный выбор. Попробуйте снова.\n";
            }
        }
        catch (...)
        {
            std::cout << "[-] Неверный ввод. Введите число.\n";
        }
    }

    if (!statusOnly && !cli.dryRun && !cli.dump && !selected.isReplay
        && !detectedMatch.empty() && selected.smartModel.name != detectedMatch)
    {
        std::cout << "\n[!] ВНИМАНИЕ: подключённый принтер сообщает \"" << detectedMdl << "\""
                  << " (запись в базе: " << detectedMatch << ")," << std::endl;
        std::cout << "    но вы выбрали " << selected.smartModel.name << "." << std::endl;
        std::cout << "    Запись значений сброса другой модели в EEPROM может нарушить конфигурацию принтера." << std::endl;
        std::cout << "\nПродолжить с " << selected.smartModel.name << " всё равно? [y/N]: ";

        std::string answer;
        std::getline(std::cin, answer);
        const std::string a = toLower(answer);
        if (a != "y" && a != "yes")
        {
            std::cout << "[i] Прервано до записи в EEPROM. Перезапустите и нажмите Enter для использования" << std::endl;
            std::cout << "    определённой модели." << std::endl;
            return FinishRun(1);
        }
    }

    ewr::ExecutorOptions sessionOptions = ewr::DefaultQueryOptions();
    sessionOptions.interfaceCandidate = cli.interfaceCandidate;
    sessionOptions.usbSoftResetOnOpen = cli.usbSoftReset;

    if (statusOnly)
    {
        std::cout << "\n[*] Запрос статуса принтера (только чтение, без записи в EEPROM)..." << std::endl;

        ewr::StateSnapshot state;
        if (selected.isReplay)
        {
            state = ewr::ReadPrinterStatus(gateway, sessionOptions);
        }
        else
        {
            ewr::Session session(selected.smartModel, gateway, ewr::log::Default(), sessionOptions);
            state = session.ReadState();
        }

        if (!state.available)
        {
            std::cerr << "[ОШИБКА] Не удалось прочитать статус принтера. Он включён и подключён?" << std::endl;
            std::cerr << "        Смотрите ewr_trace.log для трассировки." << std::endl;
            return FinishRun(1);
        }

        PrintPrinterStatus(state.status);
        PrintCounterValues(state.values, "Значения счётчиков отработки в EEPROM:");

        if (!selected.isReplay)
            PrintCounterSummary(selected.smartModel, state.values);

        if (selected.isReplay)
            std::cout << "[i] Значения счётчиков недоступны для Replay моделей (нет ключа чтения в дампе)." << std::endl;

        return FinishRun(0);
    }

    if (cli.dump)
    {
        if (selected.isReplay)
        {
            std::cerr << "[!] --dump требует модель Smart Protocol: чтение EEPROM выполняется с\n"
                         "    ключом чтения из базы, а Replay дампы его не содержат." << std::endl;
            return FinishRun(1);
        }

        std::cout << "\n[*] ДАМП для " << selected.smartModel.name
                  << ": чтение EEPROM (только чтение, без записи)..." << std::endl;

        const uint32_t dumpEnd = std::min<uint32_t>(selected.smartModel.mem_high, 0xFF);
        std::vector<uint16_t> addresses;
        for (uint32_t a = 0; a <= dumpEnd; ++a)
            addresses.push_back(static_cast<uint16_t>(a));

        ewr::Session session(selected.smartModel, gateway, ewr::log::Default(), sessionOptions);
        const ewr::StateSnapshot state = session.ReadAddresses(addresses);

        if (!state.available)
        {
            std::cerr << "[ОШИБКА] Не удалось прочитать принтер. Он включён и подключён?" << std::endl;
            std::cerr << "        Смотрите ewr_trace.log для трассировки." << std::endl;
            return FinishRun(1);
        }

        PrintPrinterStatus(state.status);

        size_t answered = 0;
        for (const auto& v : state.values)
            answered += (v.second >= 0) ? 1 : 0;

        const std::string path = WriteEepromDump(selected.smartModel, state.values);
        if (path.empty())
        {
            std::cerr << "[ОШИБКА] EEPROM прочитан, но не удалось записать файл дампа." << std::endl;
            return FinishRun(1);
        }

        std::cout << "\n[УСПЕХ] Записано " << answered << " из " << state.values.size()
                  << " байт EEPROM в " << path << "." << std::endl;
        std::cout << "    Чтобы картировать цвет: сделайте дамп, напечатайте этим цветом (или замените картридж),\n"
                     "    сделайте дамп снова и сравните файлы. Изменившиеся байты — это\n"
                     "    счётчик чернил этого цвета.\n"
                     "    Помните о зеркальной ловушке: байт, который возвращается сам после\n"
                     "    цикла питания, перезаписывается прошивкой из чипа картриджа — этот\n"
                     "    уровень живёт на чипе и не может быть сброшен с ПК." << std::endl;
        return FinishRun(0);
    }

    if (cli.dryRun)
    {
        if (selected.isReplay)
        {
            const std::vector<std::vector<unsigned char>> dumpSequence =
                ewr::ParseWiresharkDump(selected.replayModel.filepath);

            size_t dumpWrites = 0;
            for (const auto& packet : dumpSequence)
            {
                if (ewr::IsWritePacket(packet))
                    dumpWrites++;
            }

            std::cout << "\n[ПРОБНЫЙ ЗАПУСК] " << selected.displayName << ": дамп содержит "
                      << dumpSequence.size() << " пакетов, " << dumpWrites << " из них — записи EEPROM." << std::endl;
            std::cout << "[ПРОБНЫЙ ЗАПУСК] Ничего не отправлено на принтер." << std::endl;
            return FinishRun(0);
        }

        std::cout << "\n[*] ПРОБНЫЙ ЗАПУСК для " << selected.smartModel.name << ": чтение статуса и счётчиков (без записи)..." << std::endl;

        ewr::Session session(selected.smartModel, gateway, ewr::log::Default(), sessionOptions);
        const ewr::StateSnapshot state = session.ReadState();

        if (state.available)
        {
            PrintPrinterStatus(state.status);
            PrintCounterValues(state.values, "Значения счётчиков отработки в EEPROM:");
            PrintCounterSummary(selected.smartModel, state.values);
        }
        else
        {
            std::cout << "[i] Принтер не ответил на запрос только для чтения — показываю план." << std::endl;
        }

        const std::vector<uint16_t> planAddresses = selected.smartModel.GetAllAddresses();
        const std::vector<uint8_t> planValues = selected.smartModel.GetAllResetValues();

        std::cout << "\nРеальный запуск записал бы " << planAddresses.size() << " байт(а) EEPROM:" << std::endl;
        for (size_t i = 0; i < planAddresses.size(); ++i)
        {
            char line[64];
            snprintf(line, sizeof(line), "    EEPROM 0x%04X <- 0x%02X", planAddresses[i],
                     i < planValues.size() ? planValues[i] : 0);
            std::cout << line << std::endl;
        }

        for (const auto& op : selected.smartModel.close_ops)
        {
            char line[96];
            snprintf(line, sizeof(line), "    фиксация: чтение 0x%04X, применить AND 0x%02X / OR 0x%02X, записать обратно",
                     op.address, op.and_mask, op.or_mask);
            std::cout << line << std::endl;
        }

        if (!selected.smartModel.wkey1.empty())
            std::cout << "    (доступен альтернативный ключ записи, если основной отклонён)" << std::endl;

        std::cout << "\n[ПРОБНЫЙ ЗАПУСК] Ничего не записано. Запустите без --dry-run для выполнения сброса." << std::endl;
        return FinishRun(0);
    }

    if (selected.isReplay)
    {
        std::cout << "\n[!] Разбор replay дампа Wireshark для " << selected.displayName << "..." << std::endl;
        const std::vector<std::vector<unsigned char>> executionSequence =
            ewr::ParseWiresharkDump(selected.replayModel.filepath);

        if (executionSequence.empty())
        {
            std::cerr << "[-] Не удалось построить полезную нагрузку. Выход.\n";
            return FinishRun(1);
        }

        std::cout << "Сканирование USB-портов для устройства Epson..." << std::endl;

        ewr::ExecutorOptions replayOptions;
        replayOptions.validateHandshake = false;
        replayOptions.resendCreditOnRetry = false;
        replayOptions.verifyWrites = false;
        replayOptions.useSessionLayer = false;
        replayOptions.interfaceCandidate = cli.interfaceCandidate;
        replayOptions.usbSoftResetOnOpen = cli.usbSoftReset;

        const ewr::ResetRunResult run = gateway.RunReset(executionSequence, replayOptions);

        if (!run.deviceFound)
        {
            std::cerr << "[ОШИБКА] Не удалось найти принтер Epson. Он включён и подключён?" << std::endl;
            return FinishRun(1);
        }

        return FinishReset(run.exec.success);
    }

    if (selected.smartModel.IsPlatenOnly())
    {
        std::cout << "\n================================================================================" << std::endl;
        std::cout << "[!] УВЕДОМЛЕНИЕ ДЛЯ " << selected.smartModel.name << ":" << std::endl;
        std::cout << "    Эта модель имеет EEPROM-счётчики ТОЛЬКО для ПЛАТЕНА (прокладка для печати без полей)." << std::endl;
        std::cout << "    ОСНОВНОЙ БОКС ОТРАБОТКИ на этом принтере использует физический чип на" << std::endl;
        std::cout << "    сервисном баке и НЕ МОЖЕТ быть сброшен через USB EEPROM." << std::endl;
        std::cout << "    Для сброса основного бокса замените сервисный бак или используйте физический чип-ресеттер." << std::endl;
        std::cout << "================================================================================\n" << std::endl;
    }

    bool resetInk = false;
    if (selected.smartModel.HasInkReset() && !selected.smartModel.HasResettableCounters())
    {
        std::cout << "\n" << selected.smartModel.name
                  << " имеет карту чернил картриджей, но нет сбрасываемых счётчиков отработки," << std::endl;
        std::cout << "    поэтому сброс уровня чернил — доступный путь." << std::endl;
        resetInk = true;
    }
    else if (selected.smartModel.HasInkReset())
    {
        std::cout << "\n" << selected.smartModel.name
                  << " также имеет карту чернил картриджей в базе." << std::endl;
        std::cout << "\nЧто нужно сбросить?" << std::endl;
        std::cout << "[1] Счётчики отработки (классический сброс EWR)" << std::endl;
        std::cout << "[2] Уровни чернил картриджей (работает только если уровни в EEPROM принтера)" << std::endl;

        while (true)
        {
            std::cout << "\nВыберите [1-2] (Enter = 1): ";
            std::string choice;
            std::getline(std::cin, choice);

            if (choice.empty() || choice == "1")
                break;

            if (choice == "2")
            {
                resetInk = true;
                break;
            }

            std::cout << "[-] Неверный выбор. Введите 1 или 2." << std::endl;
        }
    }

    if (resetInk)
    {
        std::cout << "\n[!] Сброс чернил перезаписывает учёт чернил принтера по цветам." << std::endl;
        std::cout << "    EWR не может долить чернила: действительно пустой картридж будет показывать полный и" << std::endl;
        std::cout << "    может закончиться во время печати, что повредит печатающую головку." << std::endl;
        std::cout << "    Сброс держится только на принтерах, где учёт чернил в собственном EEPROM." << std::endl;
        std::cout << "    Картриджи с чипом хранят уровни на чипе: прошивка считает чип истиной и" << std::endl;
        std::cout << "    перезаписывает EEPROM из него, поэтому на таких моделях сброс не держится" << std::endl;
        std::cout << "    (поколение R220 именно такое — проверено на железе)." << std::endl;
        std::cout << "\nВведите 'reset' для обнуления счётчиков всех цветов, что-то другое для отмены: ";

        std::string confirm;
        std::getline(std::cin, confirm);
        if (toLower(confirm) != "reset")
        {
            std::cout << "[i] Прервано до записи в EEPROM. Ничего не изменено." << std::endl;
            return FinishRun(0);
        }
    }

    ewr::Session session(selected.smartModel, gateway, ewr::log::Default(), sessionOptions);

    ewr::ResetHandlers handlers;

    handlers.onPreflight = [&](const ewr::StateSnapshot& before)
    {
        PrintPrinterStatus(before.status);
        if (resetInk)
        {
            PrintCounterValues(before.values, "Счётчики чернил ДО сброса:");
            PrintInkSummary(selected.smartModel, before.values);
        }
        else
        {
            PrintCounterValues(before.values, "Счётчики отработки ДО сброса:");
            PrintCounterSummary(selected.smartModel, before.values);
        }
    };

    handlers.onBlocker = [](const ewr::Blocker& blocker)
    {
        if (blocker.errorCode >= 0)
            std::cout << "\n[!] ВНИМАНИЕ: принтер сообщает об активной ошибке: "
                      << blocker.errorName << "." << std::endl;
        else
            std::cout << "\n[!] ВНИМАНИЕ: " << blocker.errorName << "." << std::endl;

        if (!blocker.explanation.empty())
            std::cout << "    " << blocker.explanation << std::endl;

        std::cout << "\nПопробовать сброс всё равно? [y/N]: ";

        std::string answer;
        std::getline(std::cin, answer);
        const std::string a = toLower(answer);
        return a == "y" || a == "yes";
    };

    handlers.confirmWrite = [&](const ewr::StateSnapshot&)
    {
        if (resetInk)
            return true;

        std::cout << "\nСбросить счётчики отработки " << selected.smartModel.name
                  << " сейчас? [y/N]: ";

        std::string answer;
        std::getline(std::cin, answer);
        const std::string a = toLower(answer);
        return a == "y" || a == "yes";
    };

    handlers.onVerify = [&](const ewr::StateSnapshot& after)
    {
        if (resetInk)
        {
            PrintCounterValues(after.values, "Счётчики чернил ПОСЛЕ сброса:");
            PrintInkSummary(selected.smartModel, after.values);
        }
        else
        {
            PrintCounterValues(after.values, "Счётчики отработки ПОСЛЕ сброса:");
            PrintCounterSummary(selected.smartModel, after.values);
        }
    };

    const ewr::ResetOutcome outcome = resetInk ? session.ResetInk(handlers)
                                               : session.Reset(handlers);

    if (outcome.phase == ewr::ResetPhase::Aborted
        || outcome.phase == ewr::ResetPhase::DeviceNotFound)
        return FinishRun(1);

    return FinishReset(outcome.success);
}
